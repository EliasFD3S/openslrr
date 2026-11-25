package java.game;

import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.render.osdx.Event;
import java.render.osdx.Menu;
import java.render.osdx.Style;
import java.render.osdx.Gadget;
import java.render.osdx.MultiChoice;
import java.render.osdx.Slider;
import java.render.osdx.Rectangle;
import java.sound.*;

// Full-screen options built on the CRC 2005 OSD (java.render.osdx), the
// OSD v2 demo (docs/ui/osd-v2.md). OptionsDialog CMD_OPTIONS opens it from
// the main menu and the in-game menu.
//
// Every control carries the OptionsDialog command of the same setting. A
// change copies the value into owner.osd.multiValue / sliderValue and runs
// owner.osdCommand, so the settings are applied by the existing code.
public class OsdxOptions extends GameType implements EventReceiver, Animated
{
	final static int	CMD_BACK		= 1000;
	final static int	CMD_APPLY_RES	= 1001;
	final static int	CMD_RES			= 1002;
	final static int	CMD_WINDOWED	= 1003;
	final static int	CMD_HANDOFF		= 1010;	// + OptionsDialog command
	final static int	CMD_TAB			= 1100;	// + tab index
	final static int	CMD_RUMBLE		= 1200;	// + device card
	final static int	CMD_CALIBRATE	= 1210;	// + device card
	final static int	CMD_CALRESET	= 1220;	// + device card
	final static int	CMD_REFRESH		= 1230;
	final static int	CMD_SUB			= 1300;	// + sub-page of CONTROLS
	final static int	CMD_SUB_BACK	= 1310;
	final static int	CMD_KEY			= 1400;	// + ControlSet control index
	final static int	CMD_FILE		= 1500;	// + file row
	final static int	CMD_FILE_LOAD	= 1520;
	final static int	CMD_FILE_SAVE	= 1521;
	final static int	CMD_FILE_NEW	= 1522;

	final static int	SUBS = 4;		// car keys, game keys, suspension keys, files
	final static int	SUB_FILES = 3;
	final static int	FILE_ROWS = 8;

	final static int	PAGES = 6;
	final static int	PAGE_CONTROLS = 3;
	final static int	PAGE_DEVICES = 4;
	final static int	CARDS = 4;
	final static float	LIVE_PERIOD = 0.1;	// seconds between live refreshes

	Osd				osd;
	OptionsDialog	owner;
	TabBar			tabs;
	InfoPanel		info;
	Toast			toast;
	int[]			page = new int[PAGES];

	// Controls a preset changes; refreshVideo() re-reads them.
	MultiChoice		preset;
	MultiChoice		aa;
	MultiChoice		aniso;
	MultiChoice		refl;
	MultiChoice		clouds;
	MultiChoice		ao;
	MultiChoice		aoq;
	MultiChoice		shadows;
	MultiChoice		sq;
	MultiChoice		smooth;
	MultiChoice		vsync;
	MultiChoice		prop;
	MultiChoice		fog;
	MultiChoice		carLights;
	MultiChoice		headShadows;
	MultiChoice		lamps;
	MultiChoice		res;
	MultiChoice		windowed;
	Slider			viewDist;
	Slider			lampShadows;
	Slider			aoStrength;
	Slider			aoRadius;
	int				resFirst = 3;
	// Kept so `new T(..)` is never a bare statement: the compiler marks that
	// form unverified and the VM then stored the next assignment elsewhere.
	SectionHeader	header;

	int				activePage;
	int				open;

	OsdAnimator		animator;
	int				framePage;

	// CONTROLS: live axes and the loaded control set
	LevelBar		meterThrottle;
	LevelBar		meterBrake;
	LevelBar		meterSteer;
	LevelBar		meterClutch;
	int				controlsDirty;
	Slider[]		gammaSlider = new Slider[4];
	Slider[]		deadSlider = new Slider[4];

	// CONTROLS sub-pages: key bindings and control files
	int[]			sub = new int[SUBS];
	int				activeSub = -1;
	Gadget[]		keyButton = new Gadget[ControlSet.NCONTROLS];
	int				getKey = -1;		// control being captured, -1 none
	int				getKeyStat;
	// Read by the host: hotkeys (Esc) do not fire while a key is captured.
	int				capturingInput;
	Gadget[]		fileButton = new Gadget[FILE_ROWS];
	String[]		fileName = new String[FILE_ROWS];
	int				fileCount;
	int				fileSelected = -1;
	Text			fileStatus;

	// DEVICES: one card per connected controller
	Rectangle[]		cardPlate = new Rectangle[CARDS];
	Text[]			cardName = new Text[CARDS];
	Text[]			cardInfo = new Text[CARDS];
	LevelBar[]		cardActivity = new LevelBar[CARDS];
	Gadget[]		cardCalibrate = new Gadget[CARDS];
	Gadget[]		cardRumble = new Gadget[CARDS];
	Gadget[]		cardReset = new Gadget[CARDS];
	int[]			cardDevice = new int[CARDS];	// InputDevices index, -1 empty
	Text			noDevice;
	Text			deviceCount;

	public OsdxOptions( OptionsDialog o )
	{
		createNativeInstance();
		owner = o;

		osd = new Osd( 1.0, 0.0, 24 );
		osd.setEventHandler( this );
		osd.addEventFlags( Event.F_GAD_CHANGE|Event.F_GAD_LCLICK );
		osd.gadgetIntro = 0;	// pages slide in with the animator instead
		animator = new OsdAnimator( osd );

		// ---- frame, shared by every page ----
		// No picture: the screen behind (prime.avi in the main menu, the garage
		// in the in-game menu) shows through a full-screen translucent veil.
		osd.createRectangle( 0.0, 0.0, 2.0, 2.0, 0, 0xA0080A0E );
		osd.createText( "OPTIONS", Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.93 ).changeColor( 0xFFFFAE00 );

		String[] names = new String[PAGES];
		names[0] = "VIDEO"; names[1] = "GRAPHICS"; names[2] = "SOUND"; names[3] = "CONTROLS"; names[4] = "DEVICES"; names[5] = "GAME";
		tabs = new TabBar( osd, 0.0, -0.72, 0.31, 0.08, names, CMD_TAB );

		info = new InfoPanel( osd, "INFO", "Point at a setting to read what it does.", 0.0, 0.62, 1.7, 0.2 );

		HintBar hints = new HintBar( osd, -0.88, 0.83 );
		hints.add( "MOUSE", "SELECT" );
		hints.add( "ARROWS", "CHANGE" );
		hints.add( "ESC", "BACK" );

		// The plate is its own rectangle: a coloured Style tints the label too.
		Style buttonStyle = new Style( 0.32, 0.08, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createRectangle( 0.78, 0.85, 0.24, 0.08, 0, 0x80000000 );
		osd.createButton( buttonStyle, 0.78, 0.85, "BACK", CMD_BACK );
		osd.createHotkey( Input.AXIS_CANCEL, Input.VIRTUAL, CMD_BACK );

		toast = new Toast( osd, 0.62, -0.88, 0.62, 0.08 );
		toast.animator = animator;

		// ---- pages: one group each ----
		framePage = osd.endGroup();	// closes the frame group
		buildVideo();		page[0] = osd.endGroup();
		buildGraphics();	page[1] = osd.endGroup();
		buildSound();		page[2] = osd.endGroup();
		buildControls();	page[3] = osd.endGroup();
		buildDevices();		page[4] = osd.endGroup();
		buildGame();		page[5] = osd.endGroup();
		buildKeys( 0 );		sub[0] = osd.endGroup();
		buildKeys( 1 );		sub[1] = osd.endGroup();
		buildKeys( 2 );		sub[2] = osd.endGroup();
		buildFiles();		sub[3] = osd.endGroup();
		for( int i=0; i<SUBS; i++ )
			osd.hideGroup( sub[i] );

		activePage = -1;
		selectPage( 0 );
		animator.slideGroup( framePage, OsdAnimator.FADE, 0.0, 0.35, 0.0, 1 );
		osd.addAnimated( this );

		// Live meters and the device list refresh on a timer.
		open = 1;
		setEventMask( EVENT_TIME );
		addTimer( LIVE_PERIOD, 1 );
	}

	// ---------------------------------------------------------------- layout

	Menu column( float x, float y )
	{
		Style row = new Style( 0.62, 0.064, Frontend.mediumFont, Text.ALIGN_RIGHT, null );
		Menu m = osd.createMenu( row, x, y, 0.0 );
		m.setSliderStyle(
			// Label left of the track, right-aligned, like the other rows.
			new Style( 0.36, 0.05, Frontend.mediumFont, Text.ALIGN_RIGHT, new ResourceRef( frontend:0x0089r ) ).setLabelAlign( Text.ALIGN_LEFT, Text.ALIGN_RIGHT ).setLabelWidth( 0.24 ),
			new Style( 0.03, 0.05, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef( frontend:0x0091r ) ) );
		return m;
	}

	Vector list( String a, String b, String c, String d, String e )
	{
		Vector v = new Vector();
		if( a ) v.addElement( a );
		if( b ) v.addElement( b );
		if( c ) v.addElement( c );
		if( d ) v.addElement( d );
		if( e ) v.addElement( e );
		return v;
	}

	Vector onOff()
	{
		return list( "OFF", "ON", null, null, null );
	}

	Vector quality()
	{
		return list( "LOW", "MEDIUM", "HIGH", null, null );
	}

	int option( int i )
	{
		return OptionsDialog.getRendererOption( i );
	}

	final static float	LEFT = -0.12;
	final static float	RIGHT = 0.80;
	final static float	TOP = -0.52;

	void buildVideo()
	{
		header = new SectionHeader( osd, "DISPLAY", -0.84, TOP - 0.06, 0.72 );
		Menu m = column( LEFT, TOP + 0.04 );

		Vector r = new Vector();
		int current = resFirst;
		for( int i=resFirst; i<OptionsDialog.ResX_list.length; i++ )
		{
			r.addElement( Integer.toString( OptionsDialog.ResX_list[i] ) + " X " + Integer.toString( OptionsDialog.ResY_list[i] ) );
			if( OptionsDialog.ResX_list[i] == (int)Config.video_x && OptionsDialog.ResY_list[i] == (int)Config.video_y )
				current = i;
		}
		res = m.addItem( "RESOLUTION", CMD_RES, current - resFirst, r, "Screen resolution. Press APPLY to switch." );
		windowed = m.addItem( "DISPLAY MODE", CMD_WINDOWED, Config.video_windowed, list( "FULL SCREEN", "WINDOWED", null, null, null ), "Full screen or a window. Press APPLY to switch." );
		m.addItem( "APPLY", CMD_APPLY_RES, "Switch to the resolution and display mode above" );
		m.addSeparator();
		vsync = m.addToggle( "VSYNC", OptionsDialog.CMD_VSYNC, option(13), "Wait for the display refresh: no tearing, frame rate capped at the refresh rate." );

		header = new SectionHeader( osd, "QUALITY", 0.08, TOP - 0.06, 0.82 );
		m = column( RIGHT, TOP + 0.04 );
		preset = m.addItem( "PRESET", OptionsDialog.CMD_PRESET, 0, list( "CUSTOM", "LOW", "MEDIUM", "HIGH", "ULTRA" ), "Sets every graphics option at once. Changing one afterwards shows CUSTOM." );
		aa = m.addItem( "ANTI-ALIASING", OptionsDialog.CMD_RENDERER_FIRST, option(0), list( "OFF", "MSAA 2X", "MSAA 4X", "MSAA 8X", null ), "Smooths jagged edges. Higher costs more GPU time." );
		aniso = m.addToggle( "ANISOTROPIC", OptionsDialog.CMD_ANISOTROPIC, option(6), "Keeps road textures sharp at grazing angles." );
		refl = m.addToggle( "REFLECTIONS", OptionsDialog.CMD_RENDERER_FIRST+1, option(1), "Environment reflections on car paint and glass." );
		clouds = m.addToggle( "CLOUDS", OptionsDialog.CMD_RENDERER_FIRST+2, option(2), "Volumetric clouds in the sky." );
		m.addToggle( "SUN FLARE", OptionsDialog.CMD_FLARES, Config.flares, "Lens flare when looking towards the sun." );
	}

	void buildGraphics()
	{
		header = new SectionHeader( osd, "LIGHTING AND SHADOWS", -0.84, TOP - 0.06, 0.72 );
		Menu m = column( LEFT, TOP + 0.04 );
		ao = m.addItem( "AMBIENT OCCLUSION", OptionsDialog.CMD_AO_METHOD, option(7), list( "OFF", "HBAO", "GTAO", null, null ), "Contact shadows in corners and under cars. GTAO is the most accurate." );
		aoq = m.addItem( "AO QUALITY", OptionsDialog.CMD_AO_QUALITY, option(8), quality(), "Ambient occlusion samples and resolution." );
		int shadowOn = 0;
		if( Config.shadows > 0 )
			shadowOn = 1;
		shadows = m.addToggle( "SHADOWS", OptionsDialog.CMD_ENHANCED_SHADOWS, shadowOn, "Sun shadows." );
		sq = m.addItem( "SHADOW QUALITY", OptionsDialog.CMD_SHADOW_QUALITY, option(9), quality(), "Shadow map resolution." );
		smooth = m.addItem( "SHADOW EDGES", OptionsDialog.CMD_SHADOW_SMOOTHING, option(10), list( "SHARP", "SOFT", "VERY SOFT", null, null ), "How soft shadow edges are." );
		m.addToggle( "MAP SUNLIGHT", OptionsDialog.CMD_RENDERER_FIRST+3, option(3), "Use each map's own sun colour and direction." );
		m.addToggle( "LOCAL LIGHTS", OptionsDialog.CMD_RENDERER_FIRST+4, option(4), "Lights placed in the maps light the scene." );
		aoStrength = m.addItem( "AO STRENGTH", OptionsDialog.CMD_AO_STRENGTH, option(11), 0.0, 200.0, 21, "Ambient occlusion strength in percent." );
		aoStrength.printValue( "%.0f" );
		aoRadius = m.addItem( "AO RADIUS", OptionsDialog.CMD_AO_RADIUS, option(12), 25.0, 300.0, 12, "Ambient occlusion radius in centimetres." );
		aoRadius.printValue( "%.0f" );

		header = new SectionHeader( osd, "WORLD", 0.08, TOP - 0.06, 0.82 );
		m = column( RIGHT, TOP + 0.04 );
		prop = m.addItem( "PROP DETAIL", OptionsDialog.CMD_PROP_DETAIL, option(15), list( "LOW", "MEDIUM", "HIGH", "ULTRA", null ), "How far map props keep their detailed models." );
		fog = m.addToggle( "FOG", OptionsDialog.CMD_FOG, option(16), "Distance fog." );
		carLights = m.addToggle( "CAR LIGHTS", OptionsDialog.CMD_CAR_LIGHTS, option(17), "Headlight beams and brake lights." );
		headShadows = m.addToggle( "HEADLIGHT SHADOWS", OptionsDialog.CMD_HEADLIGHT_SHADOWS, option(18), "Headlights cast shadows." );
		lamps = m.addToggle( "STREET LAMPS", OptionsDialog.CMD_STREET_LAMPS, option(19), "Street lamps light the city at night." );
		viewDist = m.addItem( "VIEW DISTANCE", OptionsDialog.CMD_VIEW_DISTANCE, option(14), 500.0, 3000.0, 11, "Exterior scenery range in metres." );
		viewDist.printValue( "%.0f" );
		lampShadows = m.addItem( "LAMP SHADOWS", OptionsDialog.CMD_SHADOWED_LAMPS, option(20), 0.0, 6.0, 7, "Street lamps near the car that cast shadows." );
		lampShadows.printValue( "%.0f" );
	}

	void buildSound()
	{
		header = new SectionHeader( osd, "VOLUME", -0.84, TOP - 0.06, 0.72 );
		Menu m = column( LEFT, TOP + 0.04 );
		Slider sl;
		sl = m.addItem( "EFFECTS", OptionsDialog.CMD_EFFECTS_VOL, Sound.getVolume( Sound.CHANNEL_EFFECTS ), "Volume of effects: crashes, tyres, menus (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "MUSIC", OptionsDialog.CMD_MUSIC_VOL, Sound.getVolume( Sound.CHANNEL_MUSIC ), "Volume of the music (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ENGINE", OptionsDialog.CMD_ENGINE_VOL, Sound.getVolume( Sound.CHANNEL_ENGINE ), "Volume of the engines (percent)." );
		sl.printValue( "%.0f", 100.0 );

		header = new SectionHeader( osd, "3D SOUND", 0.08, TOP - 0.06, 0.82 );
		m = column( RIGHT, TOP + 0.04 );
		m.addToggle( "3D POSITIONING", OptionsDialog.CMD_AUDIO_POSITIONING, option(22), "Pans world sounds by their direction from the camera and fades them with distance." );
		m.addToggle( "DOPPLER", OptionsDialog.CMD_AUDIO_DOPPLER, option(23), "Pitch rises as a car comes towards the camera and falls as it moves away." );
		m.addToggle( "REVERB", OptionsDialog.CMD_AUDIO_REVERB, option(24), "Room reverb in the garage, echo of the streets and of open tracks, for world sounds." );
	}

	void buildControls()
	{
		// The control set the response sliders edit; saved when the screen closes.
		owner.controlSet = new ControlSet();
		owner.controlSet.load( GameLogic.activeControlFile );

		header = new SectionHeader( osd, "LIVE INPUT", -0.84, TOP - 0.06, 0.72 );
		meterThrottle = liveMeter( "THROTTLE", TOP + 0.04, 0 );
		meterBrake = liveMeter( "BRAKE", TOP + 0.04 + 0.064, 0 );
		meterSteer = liveMeter( "STEERING", TOP + 0.04 + 0.128, 1 );
		meterClutch = liveMeter( "CLUTCH", TOP + 0.04 + 0.192, 0 );

		Menu m = column( LEFT, TOP + 0.04 + 0.30 );
		Slider sl = m.addItem( "MOUSE", OptionsDialog.CMD_MOUSE_SENS, Config.mouseSensitivity, "Mouse pointer and mouse steering sensitivity (percent)." );
		sl.printValue( "%.0f", 100.0 );

		Style wide = new Style( 0.62, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		m = osd.createMenu( wide, -0.48, TOP + 0.04 + 0.42, 0.0 );
		m.addItem( "CAR CONTROLS", CMD_SUB + 0, "Keys and axes for throttle, brake, steering, gears, handbrake, clutch, nitro and horn." );
		m.addItem( "GAME CONTROLS", CMD_SUB + 1, "Keys for the radio, cruise control, look back and the map." );
		m.addItem( "SUSPENSION CONTROLS", CMD_SUB + 2, "Keys for adjustable suspension: each corner, axle and side." );
		m.addItem( "CONTROL FILES", CMD_SUB + 3, "Load or save a whole control set." );
		m.addItem( "RESET TO DEFAULTS", OptionsDialog.CMD_RESET_CONTROLS, "Restore the default key and axis bindings." );

		header = new SectionHeader( osd, "RESPONSE", 0.08, TOP - 0.06, 0.82 );
		m = column( RIGHT, TOP + 0.04 );
		ControlSet cs = owner.controlSet;
		String[] axis = new String[4];
		axis[0] = "THROTTLE"; axis[1] = "BRAKE"; axis[2] = "STEERING"; axis[3] = "CLUTCH";
		for( int i=0; i<4; i++ )
		{
			float pow = cs.vasp.elementAt(i).power;
			gammaSlider[i] = m.addItem( axis[i] + " GAMMA", OptionsDialog.CMD_POWER + i, (pow - 1.0) / 4.0, "Response curve of the axis: left is linear, right is gentle near rest and steep at the end." );
		}
		for( int i=0; i<4; i++ )
		{
			int dz = i;
			if( i == 3 )
				dz = 7;	// clutch
			sl = m.addItem( axis[i] + " DEAD ZONE", OptionsDialog.CMD_DEAD_ZONE + i, cs.dead_zone[dz] / 0.2, "Travel ignored around rest (percent, up to 20)." );
			sl.printValue( "%.0f", 20.0 );
			deadSlider[i] = sl;
		}
		sl = m.addItem( "STEERING SPEED SENS.", OptionsDialog.CMD_STEER_SPEED, OptionsDialog.getRendererOption(21), 0.0, 100.0, 21, "How much a wheel or stick's full lock narrows with speed (percent). 0 keeps full lock at any speed." );
		sl.printValue( "%.0f" );
		sl = m.addItem( "FORCE FEEDBACK", OptionsDialog.CMD_FFB + 0, Config.FFB_strength, 0.0, 1.0, 0, "Force feedback strength on wheels that support it (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "EMULATED FFB", OptionsDialog.CMD_FFB + 1, Config.FFB_strength_emulated, 0.0, 0.1, 0, "Steering resistance emulated on devices without force feedback." );
		sl.printValue( "%.0f", 1000.0 );
	}

	LevelBar liveMeter( String label, float y, int centred )
	{
		Text t = osd.createText( label, Frontend.mediumFont, Text.ALIGN_RIGHT, LEFT - 0.47, y - 0.5*Text.getHeight( Frontend.mediumFont, osd.getViewport() ) );
		t.changeColor( 0xFFFFAE00 );
		return new LevelBar( osd, LEFT - 0.21, y, 0.44, 0.022, centred );
	}

	// Response sliders show the loaded control set (after a load or a reset).
	void refreshResponse()
	{
		ControlSet cs = owner.controlSet;
		for( int i=0; i<4; i++ )
		{
			gammaSlider[i].setValue( (cs.vasp.elementAt(i).power - 1.0) / 4.0 );
			int dz = i;
			if( i == 3 )
				dz = 7;
			deadSlider[i].setValue( cs.dead_zone[dz] / 0.2 );
		}
	}

	// ---------------------------------------------------------------- key pages

	// kind 0: car, 1: game, 2: suspension. Control indices are the ControlSet
	// ones the classic OptionsDialog pages use.
	void buildKeys( int kind )
	{
		String title = "CAR CONTROLS";
		if( kind == 1 ) title = "GAME CONTROLS";
		if( kind == 2 ) title = "SUSPENSION CONTROLS";
		header = new SectionHeader( osd, title, -0.84, TOP - 0.06, 1.30 );
		Style back = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( back, 0.75, TOP - 0.03, "< CONTROLS", CMD_SUB_BACK ).setToolTip( "Back to the controls page." );

		if( kind == 0 )
		{
			keyColumnTitles( "PRIMARY", "SECONDARY" );
			String[] n = new String[10];
			n[0] = "ACCELERATE"; n[1] = "BRAKE"; n[2] = "TURN LEFT"; n[3] = "TURN RIGHT"; n[4] = "HAND BRAKE";
			n[5] = "SHIFT UP"; n[6] = "SHIFT DOWN"; n[7] = "CLUTCH"; n[8] = "ENGAGE NOS"; n[9] = "HONK HORN";
			for( int i=0; i<10; i++ )
			{
				float y = TOP + 0.10 + i*0.064;
				keyLabel( n[i], y );
				keyRow( i, -0.06, y );
				keyRow( i + 15, 0.52, y );
			}
		}
		else
		if( kind == 1 )
		{
			keyColumnTitles( "KEY", null );
			int[] id = new int[7];
			id[0] = 11; id[1] = 12; id[2] = 13; id[3] = 14; id[4] = 75; id[5] = 76; id[6] = 77;
			String[] n = new String[7];
			n[0] = "NEXT RADIO STATION"; n[1] = "PREV RADIO STATION"; n[2] = "VOLUME UP"; n[3] = "VOLUME DOWN";
			n[4] = "CRUISE CONTROL"; n[5] = "LOOK BACK"; n[6] = "VIEW MAP";
			for( int i=0; i<7; i++ )
			{
				float y = TOP + 0.10 + i*0.064;
				keyLabel( n[i], y );
				keyRow( id[i], -0.06, y );
			}
		}
		else
		{
			keyColumnTitles( "KEY", null );
			String[] n = new String[10];
			n[0] = "FRONT LEFT"; n[1] = "FRONT AXLE"; n[2] = "FRONT RIGHT"; n[3] = "LEFT SIDE"; n[4] = "ALL WHEELS";
			n[5] = "RIGHT SIDE"; n[6] = "REAR LEFT"; n[7] = "REAR AXLE"; n[8] = "REAR RIGHT"; n[9] = "CHANGE MODE (UP/DOWN)";
			for( int i=0; i<10; i++ )
			{
				float y = TOP + 0.10 + i*0.064;
				keyLabel( n[i], y );
				keyRow( 65 + i, -0.06, y );
			}
		}
	}

	void keyColumnTitles( String a, String b )
	{
		Text t = osd.createText( a, Frontend.smallFont, Text.ALIGN_CENTER, -0.06, TOP + 0.02 );
		t.changeColor( 0xFFB0B0B0 );
		if( b )
		{
			t = osd.createText( b, Frontend.smallFont, Text.ALIGN_CENTER, 0.52, TOP + 0.02 );
			t.changeColor( 0xFFB0B0B0 );
		}
	}

	void keyLabel( String name, float y )
	{
		Text t = osd.createText( name, Frontend.mediumFont, Text.ALIGN_RIGHT, -0.36, y - 0.5*Text.getHeight( Frontend.mediumFont, osd.getViewport() ) );
		t.changeColor( 0xFFFFAE00 );
	}

	void keyRow( int control, float x, float y )
	{
		osd.createRectangle( x, y, 0.54, 0.056, 1, 0x50000000 );
		Style st = new Style( 0.72, 0.056, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		Gadget b = osd.createButton( st, x, y, bindingName( control ), CMD_KEY + control );
		b.setToolTip( "Click, then press the key or move the axis or button to use. ESC clears it." );
		keyButton[control] = b;
	}

	String bindingName( int control )
	{
		ControlSet cs = owner.controlSet;
		if( cs.deviceID[control] < 0 || cs.axisID[control] < 0 )
			return "-";
		return shortName( Input.axisName( cs.deviceID[control], cs.axisID[control] ) );
	}

	// "Logitech G29 [0123abcd] Axis 1" -> "Logitech G29 Axis 1".
	String shortName( String s )
	{
		if( !s )
			return "-";
		String a = s.token( 0, "[" );
		String b = s.token( 2, "[]" );
		if( a && b && a.length() < s.length() )
			return a + b;
		return s;
	}

	void refreshKeys()
	{
		for( int i=0; i<ControlSet.NCONTROLS; i++ )
			if( keyButton[i] )
				keyButton[i].changeLabelText( bindingName( i ) );
	}

	void startCapture( int control )
	{
		if( getKey >= 0 && keyButton[getKey] )
			keyButton[getKey].changeLabelText( bindingName( getKey ) );
		getKey = control;
		getKeyStat = 0;
		capturingInput = 1;
		keyButton[control].changeLabelText( "PRESS A KEY..." );
		info.setText( "Press the key, move the axis or press the button to use. ESC clears the binding." );
	}

	// Per frame while capturing (the classic OptionsDialog.animate remapper):
	// wait for all inputs to be released, then take the first active one.
	void captureTick()
	{
		ControlSet cs = owner.controlSet;
		int device;
		int axis = -1;
		int devices = InputDevices.FIRST_DEVICE + InputDevices.count();
		for( device = 0; device < devices; device++ )
		{
			axis = Input.activeAxis( device );
			if( axis >= 0 )
				break;
		}
		if( getKeyStat == 0 )
		{
			if( axis < 0 )
				getKeyStat = 1;
			return;
		}
		if( getKeyStat == 1 )
		{
			if( axis < 0 )
				return;
			if( device == 0 && axis == Input.RCDIK_ESCAPE )
				cs.change( getKey, -1, -1 );
			else
			{
				float val = Input.getAxis( device, axis );
				cs.change( getKey, device, axis );
				int i;
				for( i = 0; i < ControlSet.NCONTROLS; i++ )
					if( i != getKey && cs.deviceID[i] == device && cs.axisID[i] == axis )
						break;
				if( device == 0 || val > 0.95 )
				{
					cs.from_min[getKey] = 0.0;
					cs.from_max[getKey] = 1.0;
				}
				else
				if( i < ControlSet.NCONTROLS )
				{
					// The same axis drives another control: each takes one half.
					if( val < 0.0 )
					{
						cs.from_min[getKey] = 0.0;
						cs.from_max[getKey] = -1.0;
						cs.from_min[i] = 0.0;
						cs.from_max[i] = 1.0;
					}
					else
					{
						cs.from_min[getKey] = 0.0;
						cs.from_max[getKey] = 1.0;
						cs.from_min[i] = 0.0;
						cs.from_max[i] = -1.0;
					}
				}
				else
				{
					cs.from_min[getKey] = -1.0;
					cs.from_max[getKey] = 1.0;
				}
			}
			GameLogic.player.controller.setcontrol( cs );
			controlsDirty = 1;
			keyButton[getKey].changeLabelText( bindingName( getKey ) );
			getKeyStat = 2;
			return;
		}
		// Wait for the release, so the same press does not reach the menu.
		if( axis < 0 )
		{
			getKey = -1;
			capturingInput = 0;
			info.setText( null );
		}
	}

	// ---------------------------------------------------------------- control files

	void buildFiles()
	{
		header = new SectionHeader( osd, "CONTROL FILES", -0.84, TOP - 0.06, 1.30 );
		Style back = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( back, 0.75, TOP - 0.03, "< CONTROLS", CMD_SUB_BACK ).setToolTip( "Back to the controls page." );

		Style row = new Style( 1.0, 0.06, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		for( int i=0; i<FILE_ROWS; i++ )
		{
			float y = TOP + 0.08 + i*0.068;
			osd.createRectangle( -0.36, y, 0.86, 0.06, 1, 0x50000000 );
			fileButton[i] = osd.createButton( row, -0.36, y, "", CMD_FILE + i );
			fileButton[i].setToolTip( "Select this control set." );
		}
		fileStatus = osd.createText( null, Frontend.smallFont, Text.ALIGN_LEFT, 0.18, TOP + 0.06 );
		fileStatus.changeColor( 0xFFB0B0B0 );

		Style act = new Style( 0.62, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		Menu m = osd.createMenu( act, 0.52, TOP + 0.20, 0.0 );
		m.addItem( "LOAD", CMD_FILE_LOAD, "Use the selected control set." );
		m.addItem( "SAVE", CMD_FILE_SAVE, "Write the current controls over the selected file." );
		m.addItem( "SAVE AS NEW", CMD_FILE_NEW, "Write the current controls to a new file." );
	}

	void refreshFiles()
	{
		fileCount = 0;
		FindFile ff = new FindFile();
		String name = ff.first( GameLogic.controlSaveDir + "*", FindFile.FILES_ONLY );
		while( name && fileCount < FILE_ROWS )
		{
			if( ControlSet.fileCheck( GameLogic.controlSaveDir + name ) )
			{
				fileName[fileCount] = name;
				fileCount++;
			}
			name = ff.next();
		}
		ff.close();
		if( fileSelected >= fileCount )
			fileSelected = -1;
		for( int i=0; i<FILE_ROWS; i++ )
		{
			if( i < fileCount )
			{
				if( i == fileSelected )
					fileButton[i].changeLabelText( "> " + fileName[i] + " <" );
				else
					fileButton[i].changeLabelText( fileName[i] );
				fileButton[i].enable();
			}
			else
			{
				fileButton[i].changeLabelText( "" );
				fileButton[i].disable();
			}
		}
		if( fileCount == 0 )
			fileStatus.changeText( "NO SAVED CONTROL SET YET" );
		else
		if( fileSelected < 0 )
			fileStatus.changeText( "SELECT A FILE" );
		else
			fileStatus.changeText( "SELECTED: " + fileName[fileSelected] );
	}

	void fileCommand( int cmd )
	{
		if( cmd < CMD_FILE_LOAD )
		{
			fileSelected = cmd - CMD_FILE;
			refreshFiles();
			return;
		}
		if( cmd == CMD_FILE_NEW )
		{
			saveControls();
			String target = null;
			for( int n=1; n<100 && !target; n++ )
			{
				String candidate = "CONTROLS " + Integer.toString( n );
				if( !File.exists( GameLogic.controlSaveDir + candidate ) )
					target = candidate;
			}
			if( target )
			{
				writeControls( GameLogic.controlSaveDir + target );
				toast.show( "SAVED AS " + target, 2.5 );
			}
			refreshFiles();
			return;
		}
		if( fileSelected < 0 )
		{
			toast.show( "SELECT A FILE FIRST", 2.0 );
			return;
		}
		String path = GameLogic.controlSaveDir + fileName[fileSelected];
		if( cmd == CMD_FILE_LOAD )
		{
			ControlSet cs = new ControlSet();
			cs.load( path );
			cs.save( GameLogic.activeControlFile );
			owner.controlSet = cs;
			GameLogic.player.controller.setcontrol( cs );
			controlsDirty = 0;
			refreshKeys();
			refreshResponse();
			toast.show( "LOADED " + fileName[fileSelected], 2.5 );
		}
		else
		{
			saveControls();
			writeControls( path );
			toast.show( "SAVED " + fileName[fileSelected], 2.5 );
		}
	}

	void writeControls( String path )
	{
		owner.controlSet.save( path );
	}

	void showSub( int k )
	{
		if( activeSub == k )
			return;
		osd.hideGroup( page[PAGE_CONTROLS] );
		for( int i=0; i<SUBS; i++ )
			if( i != k )
				osd.hideGroup( sub[i] );
		osd.showGroup( sub[k] );
		animator.slideGroup( sub[k], OsdAnimator.FROM_RIGHT, 0.25, 0.35, 0.12, 1 );
		activeSub = k;
		if( k == SUB_FILES )
			refreshFiles();
		else
			refreshKeys();
		info.setText( null );
	}

	void hideSub( int slideBack )
	{
		if( activeSub < 0 )
			return;
		osd.hideGroup( sub[activeSub] );
		activeSub = -1;
		getKey = -1;
		capturingInput = 0;
		if( slideBack )
		{
			osd.showGroup( page[PAGE_CONTROLS] );
			animator.slideGroup( page[PAGE_CONTROLS], OsdAnimator.FROM_LEFT, 0.25, 0.35, 0.12, 1 );
		}
	}

	void buildDevices()
	{
		header = new SectionHeader( osd, "CONNECTED CONTROLLERS", -0.84, TOP - 0.06, 1.30 );
		deviceCount = osd.createText( null, Frontend.smallFont, Text.ALIGN_RIGHT, 0.44, TOP - 0.06 );
		Style refresh = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( refresh, 0.75, TOP - 0.03, "REFRESH", CMD_REFRESH ).setToolTip( "Look for controllers again now. Plugging one in also shows it within a moment." );
		deviceCount.changeColor( 0xFFB0B0B0 );
		noDevice = osd.createText( null, Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, -0.1 );
		noDevice.changeColor( 0xFFB0B0B0 );

		Style btn = new Style( 0.22, 0.07, Frontend.smallFont, Text.ALIGN_CENTER, null );
		for( int k=0; k<CARDS; k++ )
		{
			float y = TOP + 0.04 + k*0.25;
			cardPlate[k] = osd.createRectangle( 0.0, y + 0.06, 1.74, 0.22, 1, 0x50000000 );
			cardName[k] = osd.createText( null, Frontend.mediumFont, Text.ALIGN_LEFT, -0.84, y - 0.02 );
			cardInfo[k] = osd.createText( null, Frontend.smallFont, Text.ALIGN_LEFT, -0.84, y + 0.06 );
			cardInfo[k].changeColor( 0xFFB0B0B0 );
			cardActivity[k] = new LevelBar( osd, -0.60, y + 0.13, 0.48, 0.014, 0 );
			cardRumble[k] = osd.createButton( btn, 0.30, y + 0.06, "RUMBLE", CMD_RUMBLE + k );
			cardRumble[k].setToolTip( "Vibrate this controller for a moment (force feedback test)." );
			cardCalibrate[k] = osd.createButton( btn, 0.53, y + 0.06, "CALIBRATE", CMD_CALIBRATE + k );
			cardCalibrate[k].setToolTip( "Press, move every axis (pedals, wheel, sticks) to both ends, then press DONE. The ranges are saved for this controller." );
			cardReset[k] = osd.createButton( btn, 0.76, y + 0.06, "RESET", CMD_CALRESET + k );
			cardReset[k].setToolTip( "Forget this controller's calibration: its axes use their full range again." );
			cardDevice[k] = -2;	// refreshDevices empties it
		}
	}

	void showCard( int k, int on )
	{
		cardActivity[k].setVisible( on );
		if( on )
		{
			cardPlate[k].setColor( 0x50000000 );
			cardRumble[k].changeLabelText( "RUMBLE" );
			cardReset[k].changeLabelText( "RESET" );
			cardRumble[k].enable();
			cardCalibrate[k].enable();
			cardReset[k].enable();
		}
		else
		{
			cardPlate[k].setColor( 0x00000000 );
			cardName[k].changeText( null );
			cardInfo[k].changeText( null );
			cardRumble[k].changeLabelText( "" );
			cardCalibrate[k].changeLabelText( "" );
			cardReset[k].changeLabelText( "" );
			cardRumble[k].disable();
			cardCalibrate[k].disable();
			cardReset[k].disable();
		}
	}

	// Reads the controllers again: plugging or unplugging one shows at once.
	void refreshDevices()
	{
		int k = 0;
		int n = InputDevices.count();
		for( int i=0; i<n && k<CARDS; i++ )
			if( InputDevices.connected( i ) )
			{
				if( cardDevice[k] != i )
				{
					if( cardDevice[k] < 0 )
						showCard( k, 1 );
					cardDevice[k] = i;
					cardName[k].changeText( InputDevices.name( i ) );
				}
				cardInfo[k].changeText( deviceInfo( i ) );
				if( InputDevices.calibrating( i ) )
					cardCalibrate[k].changeLabelText( "DONE" );
				else
					cardCalibrate[k].changeLabelText( "CALIBRATE" );
				cardActivity[k].setValue( InputDevices.activity( i ) );
				k++;
			}
		int shown = k;
		for( ; k<CARDS; k++ )
			if( cardDevice[k] != -1 )
			{
				cardDevice[k] = -1;
				showCard( k, 0 );
			}
		if( shown == 0 )
		{
			noDevice.changeText( "NO CONTROLLER CONNECTED. PLUG ONE IN: IT SHOWS UP HERE." );
			deviceCount.changeText( null );
		}
		else
		{
			noDevice.changeText( null );
			deviceCount.changeText( Integer.toString( shown ) + " CONNECTED" );
		}
	}

	String deviceInfo( int i )
	{
		String t = InputDevices.type( i ) + "  -  " + Integer.toString( InputDevices.axes( i ) ) + " AXES, " + Integer.toString( InputDevices.buttons( i ) ) + " BUTTONS";
		if( InputDevices.hats( i ) > 0 )
			t = t + ", " + Integer.toString( InputDevices.hats( i ) ) + " HAT";
		String power = InputDevices.power( i );
		int battery = InputDevices.battery( i );
		if( battery >= 0 )
			t = t + "  -  " + power + " " + Integer.toString( battery ) + "%";
		else
		if( power.length() > 0 )
			t = t + "  -  " + power;
		if( InputDevices.canRumble( i ) )
			t = t + "  -  RUMBLE";
		if( InputDevices.calibrating( i ) )
			t = t + "  -  CALIBRATING: MOVE EVERY AXIS TO BOTH ENDS";
		else
		if( InputDevices.calibrated( i ) )
			t = t + "  -  CALIBRATED";
		return t;
	}

	// Every frame (Osd animate hook): live meters and key capture.
	public void tick( float dt )
	{
		if( !open )
			return;
		if( activePage == PAGE_CONTROLS && activeSub < 0 )
			refreshLive();
		if( getKey >= 0 )
			captureTick();
	}

	void refreshLive()
	{
		if( activePage == PAGE_CONTROLS )
		{
			meterThrottle.setValue( Input.getInput( Input.AXIS_THROTTLE ) / 128.0 );
			meterBrake.setValue( Input.getInput( Input.AXIS_BRAKE ) / 128.0 );
			meterSteer.setValue( Input.getInput( Input.AXIS_TURN_LEFTRIGHT ) / -128.0 );
			meterClutch.setValue( Input.getInput( Input.AXIS_CLUTCH ) / 128.0 );
		}
		else
		if( activePage == PAGE_DEVICES )
			refreshDevices();
	}

	// EVENT_TIME: the live refresh.
	public void handleEvent( GameRef obj_ref, int event, int param )
	{
		if( !open )
			return;
		if( activePage == PAGE_DEVICES )
			refreshDevices();
		addTimer( LIVE_PERIOD, 1 );
	}

	void saveControls()
	{
		if( controlsDirty && owner && owner.controlSet )
		{
			owner.controlSet.save( GameLogic.activeControlFile );
			controlsDirty = 0;
		}
	}

	void deviceCommand( int cmd )
	{
		int k = cmd % 10;
		int i = cardDevice[k];
		if( i < 0 )
			return;
		if( cmd < CMD_CALIBRATE )
		{
			if( !InputDevices.canRumble( i ) || !InputDevices.rumble( i, 0.6, 600 ) )
				toast.show( "NO RUMBLE ON THIS CONTROLLER", 2.5 );
		}
		else
		if( cmd < CMD_CALRESET )
		{
			if( InputDevices.calibrating( i ) )
			{
				if( InputDevices.calibrateEnd( i ) )
					toast.show( "CALIBRATION SAVED", 2.5 );
				else
					toast.show( "NO AXIS MOVED: NOTHING SAVED", 2.5 );
			}
			else
			{
				InputDevices.calibrateBegin( i );
				toast.show( "MOVE EVERY AXIS TO BOTH ENDS", 3.0 );
			}
		}
		else
		{
			InputDevices.calibrateReset( i );
			toast.show( "CALIBRATION CLEARED", 2.5 );
		}
		refreshDevices();
	}

	void buildGame()
	{
		header = new SectionHeader( osd, "DRIVING", -0.84, TOP - 0.06, 0.72 );
		Menu m = column( LEFT, TOP + 0.04 );
		Vector gears = list( "AUTO", "SEMI-AUTO", "MANUAL", "CLUTCH", null );
		int gear = owner.clutchTrf[Config.player_transmission];
		if( gear < 0 )
			gear = 0;
		m.addItem( "TRANSMISSION", OptionsDialog.CMD_CLUTCH, gear, gears, "Gearbox and clutch handling of your car." );
		Slider sl;
		sl = m.addItem( "DIFFICULTY", OptionsDialog.CMD_DIFFICULTY, Config.deformation, 0.5, 1.5, 0, "Damage and deformation (percent of the original)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "STEERING HELP", OptionsDialog.CMD_STEERHELP, Config.player_steeringhelp, "Steering assistance (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ABS", OptionsDialog.CMD_ABS_SLIDER, Config.player_abs, "Anti-lock brakes (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ASR", OptionsDialog.CMD_ASR_SLIDER, Config.player_asr, "Traction control (percent)." );
		sl.printValue( "%.0f", 100.0 );
		m.addItem( "FRAMERATE", OptionsDialog.CMD_FRAMERATE_MODE, option(OptionsDialog.RENDERER_FRAMERATE_MODE), list( "CLASSIC", "ADAPTIVE", null, null, null ), "CLASSIC moves cars in 60 Hz steps; ADAPTIVE is smooth at any frame rate." );

		header = new SectionHeader( osd, "WORLD AND DISPLAY", 0.08, TOP - 0.06, 0.82 );
		m = column( RIGHT, TOP + 0.04 );
		m.addItem( "UNITS", OptionsDialog.CMD_METRIC, Config.metricSystem, list( "MILES", "KILOMETRES", null, null, null ), "Speed and distance units." );
		m.addItem( "GPS", OptionsDialog.CMD_GPSMODE, Config.gpsMode, list( "CAR, MAP UP", "CAR, CAR UP", "CAMERA, MAP UP", "CAMERA, CAMERA UP", null ), "What the GPS map follows and which way is up." );
		Slider sw;
		sw = m.addItem( "TRAFFIC", OptionsDialog.CMD_TRAFFICDENSITY, Config.trafficDensity, 0.0, 1.5, 0, "Traffic density in the city (percent)." );
		sw.printValue( "%.0f", 100.0 );
		sw = m.addItem( "PEDESTRIANS", OptionsDialog.CMD_PEDESTRIANDENSITY, Config.pedestrianDensity, 0.5, 1.5, 0, "Pedestrian density (percent). Below 50 the streets empty out." );
		sw.printValue( "%.0f", 100.0 );
		sw = m.addItem( "HEAD / STEER", OptionsDialog.CMD_HMF_1, Config.head_move_steer, 0.0, 2.0, 0, "How the steering wheel position moves the driver's head (percent)." );
		sw.printValue( "%.0f", 100.0 );
		sw = m.addItem( "HEAD / SPEED", OptionsDialog.CMD_HMF_2, Config.head_move_vel, 0.0, 2.0, 0, "How speed moves the driver's head (percent)." );
		sw.printValue( "%.0f", 100.0 );
		sw = m.addItem( "HEAD / ACCEL.", OptionsDialog.CMD_HMF_3, Config.head_move_acc, 0.0, 2.0, 0, "How acceleration moves the driver's head (percent)." );
		sw.printValue( "%.0f", 100.0 );
	}

	// ---------------------------------------------------------------- behaviour

	void selectPage( int p )
	{
		hideSub( 0 );
		for( int i=0; i<PAGES; i++ )
			if( i != p )
				osd.hideGroup( page[i] );
		osd.showGroup( page[p] );
		// The page comes from the side of the tab that was clicked.
		int from = OsdAnimator.FROM_RIGHT;
		if( p < activePage )
			from = OsdAnimator.FROM_LEFT;
		if( activePage < 0 )
			from = OsdAnimator.FROM_BOTTOM;
		animator.slideGroup( page[p], from, 0.25, 0.35, 0.12, 1 );
		tabs.select( p );
		activePage = p;
		refreshLive();
		info.setText( null );
	}

	// A preset changed the renderer options: show the new values.
	void refreshVideo()
	{
		aa.setValue( option(0) );
		refl.setValue( option(1) );
		clouds.setValue( option(2) );
		aniso.setValue( option(6) );
		ao.setValue( option(7) );
		aoq.setValue( option(8) );
		sq.setValue( option(9) );
		smooth.setValue( option(10) );
		vsync.setValue( option(13) );
		prop.setValue( option(15) );
		fog.setValue( option(16) );
		carLights.setValue( option(17) );
		headShadows.setValue( option(18) );
		lamps.setValue( option(19) );
		viewDist.setValue( option(14) );
		lampShadows.setValue( option(20) );
		int shadowOn = 0;
		if( Config.shadows > 0 )
			shadowOn = 1;
		shadows.setValue( shadowOn );
	}

	public void show()
	{
		if( owner )
			owner.osd.hide();
		osd.show();
	}

	public void close()
	{
		open = 0;
		getKey = -1;
		capturingInput = 0;
		osd.removeAnimated( this );
		saveControls();
		System.getConfigOptions();
		osd.hide();
		if( owner )
			owner.osd.show();
	}

	public void handleEvent( Event e )
	{
		int cmd = e.cmd;

		int t = tabs.indexOf( cmd );
		if( t >= 0 )
		{
			selectPage( t );
			return;
		}

		if( cmd == CMD_BACK )
		{
			close();
			return;
		}

		if( !owner )
			return;

		if( cmd == CMD_RES || cmd == CMD_WINDOWED )
			return;	// applied by APPLY

		if( cmd == CMD_APPLY_RES )
		{
			OptionsDialog.aspectX = OptionsDialog.ResX_list[res.value + resFirst];
			OptionsDialog.aspectY = OptionsDialog.ResY_list[res.value + resFirst];
			owner.windowed = windowed.value;
			owner.osdCommand( OptionsDialog.CMD_VIDEO_OPTIONS_DONE );
			toast.show( "DISPLAY: " + res.getChoiceText(), 2.5 );
			return;
		}

		if( cmd >= CMD_RUMBLE && cmd < CMD_CALRESET + CARDS )
		{
			deviceCommand( cmd );
			return;
		}

		if( cmd == CMD_REFRESH )
		{
			int n = InputDevices.refresh();
			refreshDevices();
			toast.show( "CONTROLLERS REFRESHED", 2.0 );
			return;
		}

		if( cmd >= CMD_SUB && cmd < CMD_SUB + SUBS )
		{
			showSub( cmd - CMD_SUB );
			return;
		}

		if( cmd == CMD_SUB_BACK )
		{
			hideSub( 1 );
			return;
		}

		if( cmd >= CMD_KEY && cmd < CMD_KEY + ControlSet.NCONTROLS )
		{
			if( getKey < 0 || getKeyStat < 2 )
				startCapture( cmd - CMD_KEY );
			return;
		}

		if( cmd >= CMD_FILE && cmd <= CMD_FILE_NEW )
		{
			fileCommand( cmd );
			return;
		}

		Gadget g = e.gadget;
		if( g instanceof Slider )
			owner.osd.sliderValue = ((Slider)g).value;
		else
		if( g instanceof MultiChoice )
			owner.osd.multiValue = ((MultiChoice)g).value;
		owner.osdCommand( cmd );
		if( cmd >= OptionsDialog.CMD_DEAD_ZONE && cmd < OptionsDialog.CMD_DEAD_ZONE + 4 )
			controlsDirty = 1;
		if( cmd >= OptionsDialog.CMD_POWER && cmd < OptionsDialog.CMD_POWER + 4 )
			controlsDirty = 1;

		if( cmd == OptionsDialog.CMD_PRESET && preset.value > 0 )
		{
			refreshVideo();
			toast.show( "PRESET " + preset.getChoiceText() + " APPLIED", 2.5 );
		}
		else
		if( cmd == OptionsDialog.CMD_RESET_CONTROLS )
		{
			controlsDirty = 0;	// the reset saved a fresh set
			refreshKeys();
			refreshResponse();
			toast.show( "CONTROLS RESET TO DEFAULTS", 2.5 );
		}
		else
		if( preset.value > 0 && cmd >= OptionsDialog.CMD_RENDERER_FIRST && cmd <= OptionsDialog.CMD_SHADOWED_LAMPS )
			preset.setValue( 0 );	// any graphics change makes it a custom setup
	}
}
