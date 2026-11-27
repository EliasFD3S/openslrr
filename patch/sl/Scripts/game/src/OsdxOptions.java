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
import java.openslrr.*;

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
	final static int	CMD_CAMERA		= 1315;	// camera style selector (GAME page)
	final static int	CMD_KEY			= 1400;	// + ControlSet control index
	// Files and mod options sit above CMD_KEY + NCONTROLS (1400 + 134 with the
	// mod key slots), so no key command can be mistaken for them.
	final static int	CMD_FILE		= 1700;	// + file row
	final static int	CMD_FILE_LOAD	= 1720;
	final static int	CMD_FILE_SAVE	= 1721;
	final static int	CMD_FILE_NEW	= 1722;
	final static int	CMD_MODKEY		= 1760;	// + visible mod key row
	final static int	CMD_MODKEY_PREV	= 1770;
	final static int	CMD_MODKEY_NEXT	= 1771;
	final static int	CMD_MODPAGE_PREV = 1790;
	final static int	CMD_MODPAGE_NEXT = 1791;
	final static int	CMD_MODOPT		= 1800;	// + ModOptions row (any number)
	final static int	MODKEY_ROWS		= 6;	// mod key rows per page, under the 8 stock rows
	final static int	MODOPT_ROWS		= 16;	// mod settings per page: 8 per column

	// Car keys per device class (#405): the keyboard page is SUB_CAR, the pad
	// and wheel pages follow the files; then the force feedback page.
	final static int	CMD_CLASS		= 1320;	// + ControlSet.CLASS_*
	final static int	CMD_FFB_TEST	= 1330;
	final static int	CMD_FFB_PRESET	= 1331;
	// GAME: the gearbox and the clutch, two choices for one
	// Config.player_transmission.
	final static int	CMD_GEARBOX		= 1340;
	final static int	CMD_CLUTCH_MODE	= 1341;

	final static int	SUBS = 7;		// car keys, game keys, suspension keys, files, car pad, car wheel, FFB
	final static int	SUB_CAR = 0;
	final static int	SUB_FILES = 3;
	final static int	SUB_CAR_PAD = 4;
	final static int	SUB_CAR_WHEEL = 5;
	final static int	SUB_FFB = 6;
	final static int	FILE_ROWS = 8;

	final static int	PAGES = 7;
	final static int	PAGE_MODS = 6;
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
	int[]			modPage;			// one group per page of mod settings
	int				modCur;
	Text			modPageText;
	Gadget[]		modKeyButton = new Gadget[MODKEY_ROWS];
	Text[]			modKeyLabel = new Text[MODKEY_ROWS];
	Rectangle[]		modKeyPlate = new Rectangle[MODKEY_ROWS];
	Text			modKeyPageText;
	int				modKeyCur;

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
	Slider			lampShadows, tyreSmoke;
	Slider			aoStrength;
	Slider			aoRadius;
	int				resFirst = 3;
	// Kept so `new T(..)` is never a bare statement: the compiler marks that
	// form unverified and the VM then stored the next assignment elsewhere.
	SectionHeader	header;

	int				activePage;
	int				open;

	MultiChoice		gearbox;	// AUTO, SEMI-AUTO, MANUAL
	MultiChoice		clutchMode;	// AUTO, MANUAL
	int				manualClutch;	// the clutch asked for, kept while the gearbox is not MANUAL

	OsdAnimator		animator;
	int				framePage;

	// CONTROLS: live axes and the loaded control set
	LevelBar		meterThrottle;
	LevelBar		meterBrake;
	LevelBar		meterSteer;
	LevelBar		meterClutch;
	LevelBar		meterHandbrake;
	int				controlsDirty;
	Slider[]		gammaSlider = new Slider[4];
	Slider[]		deadSlider = new Slider[5];

	// CONTROLS sub-pages: key bindings and control files
	int[]			sub = new int[SUBS];
	int				activeSub = -1;
	Gadget[]		keyButton = new Gadget[ControlSet.NCONTROLS];
	int				getKey = -1;		// control being captured, -1 none
	int				getKeyStat;
	// Read by the host: hotkeys (Esc) do not fire while a key is captured.
	int				capturingInput;
	// The assignment dialog over the key page (InputDevices.capture*).
	int				captureGroup;
	Text			capTitle;
	Text			capStep;
	Text			capDetail;
	LevelBar		capMeter;
	String[]		controlName = new String[ControlSet.NCONTROLS];
	String			lastLabel;
	Gadget[]		fileButton = new Gadget[FILE_ROWS];
	String[]		fileName = new String[FILE_ROWS];
	int				fileCount;
	int				fileSelected = -1;
	Text			fileStatus;
	// Car keys: the class page shown, and a line per page on its devices.
	int				carClass;
	Text[]			classNote = new Text[ControlSet.CLASSES];
	// FORCE FEEDBACK page.
	Text			ffbStatus;
	MultiChoice		ffbPreset;
	Slider			ffbVibration;
	Slider			ffbDamper;
	Slider			ffbMinForce;
	Slider			ffbSmoothing;
	int				ffbPresetting;	// 1 while a preset moves the sliders
	int				captureClass = -1;	// ControlSet.CLASS_* listening, -1 all

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
		names[0] = "VIDEO"; names[1] = "GRAPHICS"; names[2] = "SOUND"; names[3] = "CONTROLS"; names[4] = "DEVICES"; names[5] = "GAME"; names[6] = "MODS";
		tabs = new TabBar( osd, 0.0, -0.72, 0.27, 0.08, names, CMD_TAB );

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
		buildMods();		page[PAGE_MODS] = osd.endGroup();
		buildCarKeys( ControlSet.CLASS_KEYBOARD );	sub[SUB_CAR] = osd.endGroup();
		buildKeys( 1 );		sub[1] = osd.endGroup();
		buildKeys( 2 );		sub[2] = osd.endGroup();
		buildFiles();		sub[3] = osd.endGroup();
		buildCarKeys( ControlSet.CLASS_PAD );	sub[SUB_CAR_PAD] = osd.endGroup();
		buildCarKeys( ControlSet.CLASS_WHEEL );	sub[SUB_CAR_WHEEL] = osd.endGroup();
		buildForceFeedback();	sub[SUB_FFB] = osd.endGroup();
		buildCapture();		captureGroup = osd.endGroup();
		for( int i=0; i<SUBS; i++ )
			osd.hideGroup( sub[i] );
		osd.hideGroup( captureGroup );

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
		tyreSmoke = m.addItem( "TYRE SMOKE", OptionsDialog.CMD_TYRE_SMOKE, option(33), 0.0, 400.0, 9, "How much smoke and dust the tyres make, in percent of the default." );
		tyreSmoke.printValue( "%.0f" );
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
		// LIVE INPUT reads the player's controller, but a Dialog leaves only
		// its menu bindings there (ControlSet.MENUSET): keys and wheels showed
		// nothing until a rebind loaded the whole set. Only the pad's built-in
		// fallback moved the meters. Load the whole set now, as a rebind does;
		// OptionsDialog's hide puts the previous bindings back.
		GameLogic.player.controller.setcontrol( owner.controlSet );

		header = new SectionHeader( osd, "LIVE INPUT", -0.84, TOP - 0.06, 0.72 );
		meterThrottle = liveMeter( "THROTTLE", TOP + 0.04, 0 );
		meterBrake = liveMeter( "BRAKE", TOP + 0.04 + 0.064, 0 );
		meterSteer = liveMeter( "STEERING", TOP + 0.04 + 0.128, 1 );
		meterClutch = liveMeter( "CLUTCH", TOP + 0.04 + 0.192, 0 );
		meterHandbrake = liveMeter( "HANDBRAKE", TOP + 0.04 + 0.256, 0 );

		Menu m = column( LEFT, TOP + 0.04 + 0.36 );
		Slider sl = m.addItem( "MOUSE", OptionsDialog.CMD_MOUSE_SENS, Config.mouseSensitivity, "Mouse pointer and mouse steering sensitivity (percent)." );
		sl.printValue( "%.0f", 100.0 );

		Style wide = new Style( 0.62, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		m = osd.createMenu( wide, -0.48, TOP + 0.04 + 0.48, 0.0 );
		m.addItem( "CAR CONTROLS", CMD_SUB + SUB_CAR, "Keys and axes for throttle, brake, steering, gears, handbrake, clutch, nitro and horn, for the keyboard, a pad and a wheel apart." );
		m.addItem( "FORCE FEEDBACK", CMD_SUB + SUB_FFB, "Strength, road feel, impacts, vibration and the wheel's dead band." );
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
		String[] dead = new String[5];
		dead[0] = axis[0]; dead[1] = axis[1]; dead[2] = axis[2]; dead[3] = axis[3]; dead[4] = "HANDBRAKE";
		for( int i=0; i<5; i++ )
		{
			sl = m.addItem( dead[i] + " DEAD ZONE", OptionsDialog.CMD_DEAD_ZONE + i, cs.dead_zone[OptionsDialog.deadZoneControl( i )] / 0.2, "Travel ignored around rest (percent, up to 20)." );
			sl.printValue( "%.0f", 20.0 );
			deadSlider[i] = sl;
		}
		sl = m.addItem( "STEERING SPEED SENS.", OptionsDialog.CMD_STEER_SPEED, OptionsDialog.getRendererOption(21), 0.0, 100.0, 21, "How much a wheel or stick's full lock narrows with speed (percent). 0 keeps full lock at any speed." );
		sl.printValue( "%.0f" );
	}

	// FORCE FEEDBACK sub-page (#405). Each part of the force has its own gain.
	void buildForceFeedback()
	{
		header = new SectionHeader( osd, "FORCE FEEDBACK", -0.84, TOP - 0.06, 1.30 );
		Style back = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( back, 0.75, TOP - 0.03, "< CONTROLS", CMD_SUB_BACK ).setToolTip( "Back to the controls page." );

		Menu m = column( LEFT, TOP + 0.08 );
		Slider sl = m.addItem( "STRENGTH", OptionsDialog.CMD_FFB + 0, Config.FFB_strength, 0.0, 1.0, 0, "Overall force of the wheel (percent). 0 turns force feedback off." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ROAD FEEL", OptionsDialog.CMD_FFB_DETAIL, option(26), 0.0, 200.0, 21, "Bumps and kerbs under one front wheel kick the steering (percent)." );
		sl.printValue( "%.0f" );
		sl = m.addItem( "IMPACTS", OptionsDialog.CMD_FFB_IMPACTS, option(27), 0.0, 200.0, 21, "Knock in the wheel when the car hits something (percent)." );
		sl.printValue( "%.0f" );
		ffbVibration = m.addItem( "TYRE VIBRATION", OptionsDialog.CMD_FFB_VIBRATION, option(28), 0.0, 200.0, 21, "Light shudder while the front tyres scrub past their grip (percent). Lower it if the wheel buzzes." );
		ffbVibration.printValue( "%.0f" );
		ffbDamper = m.addItem( "DAMPER", OptionsDialog.CMD_FFB_DAMPER, option(29), 0.0, 200.0, 21, "Weight of the wheel when parked and friction at speed (percent)." );
		ffbDamper.printValue( "%.0f" );
		m.addToggle( "INVERT FFB", OptionsDialog.CMD_FFB_INVERT, option(25), "Turn on if the wheel pulls away from the centre instead of back to it." );

		m = column( RIGHT, TOP + 0.08 );
		ffbPreset = m.addItem( "WHEEL TYPE", CMD_FFB_PRESET, 0, list( "CUSTOM", "GEAR DRIVE", "BELT DRIVE", "DIRECT DRIVE", null ), "Sets the minimum force, smoothing, vibration and damper for the kind of wheel base. Changing one afterwards shows CUSTOM." );
		ffbMinForce = m.addItem( "MIN FORCE", OptionsDialog.CMD_FFB_MIN_FORCE, option(30), 0.0, 20.0, 21, "Lifts small forces past the wheel's dead zone (percent). Gear driven wheels need some, direct drive none." );
		ffbMinForce.printValue( "%.0f" );
		ffbSmoothing = m.addItem( "SMOOTHING", OptionsDialog.CMD_FFB_SMOOTHING, option(31), 0.0, 50.0, 11, "Filters the force (milliseconds). Higher is smoother but a little later." );
		ffbSmoothing.printValue( "%.0f" );
		sl = m.addItem( "EMULATED FFB", OptionsDialog.CMD_FFB + 1, Config.FFB_strength_emulated, 0.0, 0.1, 0, "Steering resistance emulated on pads and wheels without force feedback." );
		sl.printValue( "%.0f", 1000.0 );
		m.addItem( "TEST", CMD_FFB_TEST, "Pulls the wheel left, then right, for a moment." );

		ffbStatus = osd.createText( null, Frontend.smallFont, Text.ALIGN_LEFT, -0.84, TOP + 0.52 );
		ffbStatus.changeColor( 0xFFB0B0B0 );
	}

	void refreshForceFeedback()
	{
		int i = InputDevices.forceFeedbackIndex();
		if( i < 0 )
			ffbStatus.changeText( "NO FORCE FEEDBACK WHEEL CONNECTED" );
		else
			ffbStatus.changeText( "FORCE FEEDBACK: " + InputDevices.name( i ) );
	}

	// A wheel type preset: minimum force, smoothing, vibration and damper.
	void applyFfbPreset( int p )
	{
		if( p <= 0 )
			return;
		float minForce = 0.0;
		float smoothing = 3.0;
		float vibration = 80.0;
		float damper = 100.0;
		if( p == 1 )
		{
			minForce = 8.0;
			smoothing = 15.0;
			vibration = 30.0;
			damper = 80.0;
		}
		else
		if( p == 2 )
		{
			minForce = 4.0;
			smoothing = 10.0;
			vibration = 50.0;
			damper = 100.0;
		}
		ffbPresetting = 1;
		ffbSet( ffbMinForce, OptionsDialog.CMD_FFB_MIN_FORCE, minForce );
		ffbSet( ffbSmoothing, OptionsDialog.CMD_FFB_SMOOTHING, smoothing );
		ffbSet( ffbVibration, OptionsDialog.CMD_FFB_VIBRATION, vibration );
		ffbSet( ffbDamper, OptionsDialog.CMD_FFB_DAMPER, damper );
		ffbPresetting = 0;
	}

	void ffbSet( Slider s, int cmd, float v )
	{
		s.setValue( v );
		owner.osd.sliderValue = v;
		owner.osdCommand( cmd );
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
			gammaSlider[i].setValue( (cs.vasp.elementAt(i).power - 1.0) / 4.0 );
		for( int i=0; i<5; i++ )
			deadSlider[i].setValue( cs.dead_zone[OptionsDialog.deadZoneControl( i )] / 0.2 );
	}

	// ---------------------------------------------------------------- key pages

	String className( int cls )
	{
		if( cls == ControlSet.CLASS_PAD )
			return "GAMEPAD";
		if( cls == ControlSet.CLASS_WHEEL )
			return "WHEEL / SIM";
		return "KEYBOARD & MOUSE";
	}

	// CAR CONTROLS of one device class (#405): tabs for the three classes,
	// then a primary and a secondary binding per control, in that class's
	// ControlSet slots. The listener only takes devices of the class.
	void buildCarKeys( int cls )
	{
		header = new SectionHeader( osd, "CAR CONTROLS", -0.84, TOP - 0.06, 1.30 );
		Style back = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( back, 0.75, TOP - 0.03, "< CONTROLS", CMD_SUB_BACK ).setToolTip( "Back to the controls page." );

		String[] names = new String[ControlSet.CLASSES];
		for( int k=0; k<ControlSet.CLASSES; k++ )
			names[k] = className( k );
		TabBar bar = new TabBar( osd, 0.0, TOP + 0.06, 0.40, 0.07, names, CMD_CLASS );
		bar.select( cls );

		classNote[cls] = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, 0.0, TOP + 0.26 + ControlSet.CAR_CONTROLS*0.058 );
		classNote[cls].changeColor( 0xFF808080 );

		float top = TOP + 0.10;
		Text t = osd.createText( "PRIMARY", Frontend.smallFont, Text.ALIGN_CENTER, -0.06, top + 0.08 );
		t.changeColor( 0xFFB0B0B0 );
		t = osd.createText( "SECONDARY", Frontend.smallFont, Text.ALIGN_CENTER, 0.52, top + 0.08 );
		t.changeColor( 0xFFB0B0B0 );
		String[] n = new String[ControlSet.CAR_CONTROLS];
		n[0] = "ACCELERATE"; n[1] = "BRAKE"; n[2] = "TURN LEFT"; n[3] = "TURN RIGHT"; n[4] = "HAND BRAKE";
		n[5] = "SHIFT UP"; n[6] = "SHIFT DOWN"; n[7] = "CLUTCH"; n[8] = "ENGAGE NOS"; n[9] = "HONK HORN";
		for( int i=0; i<ControlSet.CAR_CONTROLS; i++ )
		{
			float y = top + 0.16 + i*0.058;
			keyLabel( n[i], y );
			int a = ControlSet.carSlot( cls, 0, i );
			int b = ControlSet.carSlot( cls, 1, i );
			keyRow( a, -0.06, y );
			keyRow( b, 0.52, y );
			controlName[a] = n[i] + " (" + className( cls ) + ")";
			controlName[b] = n[i] + " (" + className( cls ) + ", SECONDARY)";
		}
	}

	// Opens CAR CONTROLS: old files are sorted into the class slots first,
	// then the page of the richest class connected (wheel, pad, keyboard).
	void showCar()
	{
		if( owner.controlSet.sortCarControls() )
			controlsDirty = 1;
		int cls = ControlSet.CLASS_KEYBOARD;
		if( InputDevices.classCount( ControlSet.CLASS_WHEEL ) > 0 )
			cls = ControlSet.CLASS_WHEEL;
		else
		if( InputDevices.classCount( ControlSet.CLASS_PAD ) > 0 )
			cls = ControlSet.CLASS_PAD;
		showSub( carSub( cls ) );
	}

	int carSub( int cls )
	{
		if( cls == ControlSet.CLASS_PAD )
			return SUB_CAR_PAD;
		if( cls == ControlSet.CLASS_WHEEL )
			return SUB_CAR_WHEEL;
		return SUB_CAR;
	}

	// A class tab: the sibling page replaces this one in place.
	void switchCarClass( int cls )
	{
		int k = carSub( cls );
		if( activeSub == k || activeSub < 0 )
			return;
		osd.hideGroup( sub[activeSub] );
		osd.showGroup( sub[k] );
		activeSub = k;
		carClass = cls;
		refreshKeys();
	}

	void refreshClassNote()
	{
		Text t = classNote[carClass];
		if( !t )
			return;
		int n = InputDevices.classCount( carClass );
		if( carClass == ControlSet.CLASS_KEYBOARD )
			t.changeText( "Keys and mouse buttons. Used together with the pad and the wheel." );
		else
		if( n == 0 )
			t.changeText( "NO " + className( carClass ) + " CONNECTED: the bindings below wait for one." );
		else
			t.changeText( Integer.toString( n ) + " " + className( carClass ) + " DEVICE(S) CONNECTED. Only these listen when you assign." );
	}

	// kind 1: game, 2: suspension (car controls: buildCarKeys). Control
	// indices are the ControlSet ones the classic OptionsDialog pages use.
	void buildKeys( int kind )
	{
		String title = "GAME CONTROLS";
		if( kind == 2 ) title = "SUSPENSION CONTROLS";
		header = new SectionHeader( osd, title, -0.84, TOP - 0.06, 1.30 );
		Style back = new Style( 0.32, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		osd.createButton( back, 0.75, TOP - 0.03, "< CONTROLS", CMD_SUB_BACK ).setToolTip( "Back to the controls page." );

		if( kind == 1 )
		{
			keyColumnTitles( "KEY", null );
			// CHANGE VIEW (#470): slot 35, V by default; cycles the camera views.
			int[] id = new int[8];
			id[0] = 11; id[1] = 12; id[2] = 13; id[3] = 14; id[4] = 75; id[5] = 76; id[6] = 77; id[7] = 35;
			String[] n = new String[8];
			n[0] = "NEXT RADIO STATION"; n[1] = "PREV RADIO STATION"; n[2] = "VOLUME UP"; n[3] = "VOLUME DOWN";
			n[4] = "CRUISE CONTROL"; n[5] = "LOOK BACK"; n[6] = "VIEW MAP"; n[7] = "CHANGE VIEW";
			for( int i=0; i<8; i++ )
			{
				float y = TOP + 0.10 + i*0.064;
				keyLabel( n[i], y );
				keyRow( id[i], -0.06, y );
			}
			buildModKeys( 8 );
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

	// Keys that mods registered with ModKeys, after the stock rows: a fixed
	// block of MODKEY_ROWS rows that pages through every registered key
	// (ModKeys.count() of them, 16 today). Needs ModKeys.count(), label(i)
	// and control(i) (the ControlSet slot).
	void buildModKeys( int first )
	{
		if( ModKeys.count() <= 0 )
			return;
		Style st = new Style( 0.72, 0.056, Frontend.mediumFont, Text.ALIGN_CENTER, null );
		Style nav = new Style( 0.22, 0.056, Frontend.smallFont, Text.ALIGN_CENTER, null );
		for( int r=0; r<MODKEY_ROWS; r++ )
		{
			float y = TOP + 0.10 + (first + r)*0.064;
			modKeyLabel[r] = osd.createText( " ", Frontend.mediumFont, Text.ALIGN_RIGHT, -0.36, y - 0.5*Text.getHeight( Frontend.mediumFont, osd.getViewport() ) );
			modKeyLabel[r].changeColor( 0xFFFFAE00 );
			modKeyPlate[r] = osd.createRectangle( -0.06, y, 0.54, 0.056, 1, 0x50000000 );
			modKeyButton[r] = osd.createButton( st, -0.06, y, "-", CMD_MODKEY + r );
			modKeyButton[r].setToolTip( "Click, then press the key or move the axis or button to use. ESC clears it." );
		}
		if( ModKeys.count() > MODKEY_ROWS )
		{
			float y = TOP + 0.10 + (first + MODKEY_ROWS)*0.064;
			osd.createButton( nav, -0.30, y, "< PREV", CMD_MODKEY_PREV ).setToolTip( "Previous mod keys." );
			osd.createButton( nav, 0.18, y, "NEXT >", CMD_MODKEY_NEXT ).setToolTip( "More mod keys." );
			modKeyPageText = osd.createText( " ", Frontend.smallFont, Text.ALIGN_CENTER, -0.06, y - 0.5*Text.getHeight( Frontend.smallFont, osd.getViewport() ) );
			modKeyPageText.changeColor( 0xFFB0B0B0 );
		}
		refreshModKeys();
	}

	// Shows mod key page modKeyCur: label, binding, and the control each row
	// captures.
	void refreshModKeys()
	{
		int n = ModKeys.count();
		if( n <= 0 )
			return;
		for( int r=0; r<MODKEY_ROWS; r++ )
		{
			int i = modKeyCur*MODKEY_ROWS + r;
			if( i < n )
			{
				int c = ModKeys.control( i );
				modKeyLabel[r].changeText( ModKeys.label( i ) );
				modKeyPlate[r].setColor( 0x50000000 );
				modKeyButton[r].changeLabelText( bindingName( c ) );
				modKeyButton[r].enable();
				controlName[c] = ModKeys.label( i );
			}
			else
			{
				modKeyLabel[r].changeText( " " );
				modKeyPlate[r].setColor( 0x00000000 );
				modKeyButton[r].changeLabelText( "" );
				modKeyButton[r].disable();
			}
		}
		if( modKeyPageText )
			modKeyPageText.changeText( Integer.toString( modKeyCur + 1 ) + " / " + Integer.toString( (n + MODKEY_ROWS - 1)/MODKEY_ROWS ) );
	}

	void modKeyPage( int d )
	{
		int pages = (ModKeys.count() + MODKEY_ROWS - 1)/MODKEY_ROWS;
		modKeyCur = modKeyCur + d;
		if( modKeyCur < 0 )
			modKeyCur = pages - 1;
		if( modKeyCur >= pages )
			modKeyCur = 0;
		refreshModKeys();
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
		lastLabel = name;
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
		controlName[control] = lastLabel;
		if( control >= 15 && control < 30 )
			controlName[control] = lastLabel + " (SECONDARY)";
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
		refreshModKeys();
		refreshClassNote();
	}

	// The assignment dialog replaces the key page while it listens.
	void buildCapture()
	{
		osd.createRectangle( 0.0, -0.06, 1.40, 0.64, 1, 0xE0101418 );
		osd.createRectangle( 0.0, -0.06 - 0.32 + 0.003, 1.40, 0.006, 2, 0xFFFFAE00 );
		capTitle = osd.createText( null, Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, -0.32 );
		capTitle.changeColor( 0xFFFFAE00 );
		capStep = osd.createText( null, Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, -0.18 );
		capDetail = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, 0.0, -0.08 );
		capDetail.changeColor( 0xFFB0B0B0 );
		capMeter = new LevelBar( osd, 0.0, 0.04, 1.0, 0.03, 0 );
		Text t = osd.createText( "ESC: CLEAR THE BINDING     CLICK: CANCEL", Frontend.smallFont, Text.ALIGN_CENTER, 0.0, 0.14 );
		t.changeColor( 0xFF808080 );
	}

	void startCapture( int control )
	{
		getKey = control;
		getKeyStat = 0;
		capturingInput = 1;
		// A car control listens only to its page's class (#405).
		int cls = ControlSet.slotClass( control );
		captureClass = cls;
		if( cls >= 0 )
			InputDevices.captureBeginKinds( InputDevices.classKinds( cls ) );
		else
			InputDevices.captureBegin();
		if( activeSub >= 0 )
			osd.hideGroup( sub[activeSub] );
		osd.showGroup( captureGroup );
		String name = controlName[control];
		if( !name )
			name = "CONTROL";
		capTitle.changeText( "ASSIGN: " + name );
		capStep.changeText( "RELEASE EVERY CONTROL" );
		capDetail.changeText( null );
		capMeter.setValue( 0.0 );
		if( cls >= 0 )
			info.setText( "Only " + className( cls ) + " devices listen. Inputs that stay held (a shifter in gear, a switch) are ignored." );
		else
			info.setText( "Every connected device listens at once. Inputs that stay held (a shifter in gear, a switch) are ignored." );
	}

	void endCapture( String message )
	{
		InputDevices.captureCancel();
		osd.hideGroup( captureGroup );
		if( activeSub >= 0 )
			osd.showGroup( sub[activeSub] );
		getKey = -1;
		getKeyStat = 0;
		capturingInput = 0;
		refreshKeys();
		info.setText( null );
		if( message )
			toast.show( message, 2.5 );
	}

	// Per frame while the dialog is up: release, listen, then wait for the
	// release of what was found so the same press does not reach the menu.
	void captureTick()
	{
		int st = InputDevices.captureState();
		if( getKeyStat == 1 )
		{
			// Cancelled by a click: end once the button is up, or its release
			// would click the row under the pointer.
			if( Input.getAxis( 1, 3 ) < 0.5 )
				endCapture( "ASSIGNMENT CANCELLED" );
			return;
		}
		if( st == InputDevices.CAPTURE_RELEASING )
		{
			capStep.changeText( "RELEASE EVERY CONTROL" );
			capDetail.changeText( "Let go of the wheel, the pedals, the pad and the keys." );
			capMeter.setValue( InputDevices.captureProgress() );
			return;
		}
		if( st == InputDevices.CAPTURE_LISTENING )
		{
			capStep.changeText( "PRESS A KEY OR BUTTON, OR MOVE AN AXIS" );
			int held = InputDevices.captureHeld();
			if( held > 0 )
				capDetail.changeText( Integer.toString( held ) + " INPUT(S) STILL HELD ARE IGNORED" );
			else
			if( captureClass >= 0 )
				capDetail.changeText( "Only " + className( captureClass ) + " devices are listening." );
			else
				capDetail.changeText( "Every connected device is listening." );
			capMeter.setValue( 0.0 );
			return;
		}
		int device = InputDevices.captureDevice();
		int axis = InputDevices.captureAxis();
		if( st == InputDevices.CAPTURE_HOLDING )
		{
			capStep.changeText( InputDevices.kindName( device ) + ": " + shortName( Input.axisName( device, axis ) ) );
			capDetail.changeText( "Now release it." );
			float d = InputDevices.capturePeak() - InputDevices.captureRest();
			if( d < 0.0 )
				d = -d;
			if( InputDevices.captureAnalog() )
				d = d * 0.5;
			capMeter.setValue( d );
			return;
		}
		if( st == InputDevices.CAPTURE_DONE )
		{
			ControlSet cs = owner.controlSet;
			int control = getKey;
			int r = InputDevices.applyCapture( cs, control );
			GameLogic.player.controller.setcontrol( cs );
			controlsDirty = 1;
			String name = controlName[control];
			if( r == 0 )
				endCapture( name + " CLEARED" );
			else
			if( r == 2 )
				endCapture( name + ": " + shortName( Input.axisName( device, axis ) ) + " (BOTH STEERING HALVES)" );
			else
				endCapture( name + ": " + shortName( Input.axisName( device, axis ) ) );
			return;
		}
		// Cancelled by the left button, or reset behind our back.
		getKeyStat = 1;
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
		if( k == SUB_CAR )
			carClass = ControlSet.CLASS_KEYBOARD;
		if( k == SUB_CAR_PAD )
			carClass = ControlSet.CLASS_PAD;
		if( k == SUB_CAR_WHEEL )
			carClass = ControlSet.CLASS_WHEEL;
		if( k == SUB_FILES )
			refreshFiles();
		else
		if( k == SUB_FFB )
			refreshForceFeedback();
		else
			refreshKeys();
		info.setText( null );
	}

	void hideSub( int slideBack )
	{
		if( activeSub < 0 )
			return;
		if( getKey >= 0 )
			endCapture( null );
		osd.hideGroup( sub[activeSub] );
		activeSub = -1;
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
			cardRumble[k] = osd.createButton( btn, 0.30, y + 0.06, "TEST", CMD_RUMBLE + k );
			cardRumble[k].setToolTip( "Vibrate this controller for a moment, or turn a force feedback wheel left then right." );
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
			cardRumble[k].changeLabelText( "TEST" );
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
		if( InputDevices.canForceFeedback( i ) )
			t = t + "  -  FORCE FEEDBACK";
		else
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
			meterHandbrake.setValue( Input.getInput( Input.AXIS_HANDBRAKE ) / 128.0 );
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
			// A force feedback wheel turns one way, then the other.
			if( InputDevices.canForceFeedback( i ) )
			{
				if( !InputDevices.testForceFeedback( i ) )
					toast.show( "FORCE FEEDBACK TEST FAILED", 2.5 );
			}
			else
			if( !InputDevices.canRumble( i ) || !InputDevices.rumble( i, 0.6, 600 ) )
				toast.show( "NO RUMBLE OR FORCE FEEDBACK ON THIS CONTROLLER", 2.5 );
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

	// The gearbox or the clutch changed: one Config.player_transmission for
	// both. A manual clutch only exists with the manual gearbox: asking for
	// it moves the gearbox to MANUAL, and another gearbox shows the clutch as
	// AUTO until MANUAL comes back.
	void setTransmission( int cmd )
	{
		if( cmd == CMD_CLUTCH_MODE )
		{
			manualClutch = clutchMode.value;
			if( manualClutch && gearbox.value != 2 )
			{
				gearbox.setValue( 2 );
				toast.show( "A MANUAL CLUTCH NEEDS THE MANUAL TRANSMISSION", 2.5 );
			}
		}
		int mode = gearbox.value;
		if( mode == 2 && manualClutch )
			mode = 3;
		if( gearbox.value == 2 )
			clutchMode.setValue( manualClutch );
		else
			clutchMode.setValue( 0 );
		owner.osd.multiValue = mode;
		owner.osdCommand( OptionsDialog.CMD_CLUTCH );
	}

	void buildGame()
	{
		header = new SectionHeader( osd, "DRIVING", -0.84, TOP - 0.06, 0.72 );
		Menu m = column( LEFT, TOP + 0.04 );
		// OptionsDialog.clutchText: AUTO, SEMI-AUTO and MANUAL with the
		// automatic clutch, then MANUAL with the clutch pedal.
		int gear = owner.clutchTrf[Config.player_transmission];
		if( gear < 0 )
			gear = 0;
		int box = gear;
		manualClutch = 0;
		if( gear == 3 )
		{
			box = 2;
			manualClutch = 1;
		}
		gearbox = m.addItem( "TRANSMISSION", CMD_GEARBOX, box, list( "AUTO", "SEMI-AUTO", "MANUAL", null, null ), "AUTO changes every gear for you. SEMI-AUTO takes first gear when you pull away; you change the others. MANUAL leaves every change to you." );
		clutchMode = m.addItem( "CLUTCH", CMD_CLUTCH_MODE, manualClutch, list( "AUTO", "MANUAL", null, null, null ), "AUTO works the clutch for you. MANUAL is the clutch pedal or key; it needs the MANUAL transmission." );
		Slider sl;
		sl = m.addItem( "DIFFICULTY", OptionsDialog.CMD_DIFFICULTY, Config.deformation, 0.5, 1.5, 0, "Damage and deformation (percent of the original)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "STEERING HELP", OptionsDialog.CMD_STEERHELP, Config.player_steeringhelp, "Steering assistance (percent)." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ABS", OptionsDialog.CMD_ABS_SLIDER, Config.player_abs, "Anti-lock brakes. 0 is off; 100 keeps the wheels nearest their best grip, lower settings let them slip further before the brakes are released." );
		sl.printValue( "%.0f", 100.0 );
		sl = m.addItem( "ASR", OptionsDialog.CMD_ASR_SLIDER, Config.player_asr, "Traction control: cuts the engine and brakes a spinning driven wheel. 0 is off; lower settings allow more wheelspin." );
		sl.printValue( "%.0f", 100.0 );
		m.addToggle( "TEMPERATURES", OptionsDialog.CMD_HUD_TEMPERATURES, option(32), "Shows each tyre's and brake's temperature above the gauges: blue cold, green working, yellow to red overheating. A red brake mark is boiling fluid." );

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

	// OPTIONS > MODS: every row a mod put in ModOptions, MODOPT_ROWS to a
	// page (two columns of 8), one group per page; PREV/NEXT switch pages.
	// The value lives in ModSettings under the row's key, written as the
	// gadget changes. The frame (header, PREV/NEXT) joins the MODS page group.
	void buildMods()
	{
		int count = ModOptions.count();
		int pages = (count + MODOPT_ROWS - 1)/MODOPT_ROWS;
		if( pages < 1 )
			pages = 1;
		modPage = new int[pages];
		for( int p=0; p<pages; p++ )
		{
			Menu m = column( LEFT, TOP + 0.04 );
			for( int k=0; k<MODOPT_ROWS; k++ )
			{
				int i = p*MODOPT_ROWS + k;
				if( i >= count )
					break;
				if( k == 8 )
					m = column( RIGHT, TOP + 0.04 );
				ModOption o = ModOptions.get( i );
				if( o.kind == ModOptions.TOGGLE )
					m.addToggle( o.label, CMD_MODOPT + i, ModOptions.getInt( i ), o.help );
				else
				if( o.kind == ModOptions.CHOICE )
					m.addItem( o.label, CMD_MODOPT + i, ModOptions.getInt( i ), o.choices, o.help );
				else
				{
					Slider s = m.addItem( o.label, CMD_MODOPT + i, ModOptions.getFloat( i ), o.min, o.max, 0, o.help );
					if( o.max - o.min >= 10.0 )
						s.printValue( "%.0f", 1.0 );
					else
						s.printValue( "%.2f", 1.0 );
				}
			}
			modPage[p] = osd.endGroup();
		}
		header = new SectionHeader( osd, "MOD SETTINGS", -0.84, TOP - 0.06, 1.74 );
		if( count <= 0 )
			osd.createText( "No mod settings", Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, 0.0 ).changeColor( 0xFFB0B0B0 );
		if( pages > 1 )
		{
			Style nav = new Style( 0.22, 0.064, Frontend.mediumFont, Text.ALIGN_CENTER, null );
			osd.createButton( nav, -0.30, 0.45, "< PREV", CMD_MODPAGE_PREV ).setToolTip( "Previous page of mod settings." );
			osd.createButton( nav, 0.30, 0.45, "NEXT >", CMD_MODPAGE_NEXT ).setToolTip( "More mod settings." );
			modPageText = osd.createText( " ", Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, 0.45 - 0.5*Text.getHeight( Frontend.mediumFont, osd.getViewport() ) );
			modPageText.changeColor( 0xFFB0B0B0 );
		}
	}

	// Shows page p of the mod settings and hides the others; -1 hides all
	// (another tab is open).
	void showModPage( int p )
	{
		int pages = modPage.length;
		if( p >= pages )
			p = 0;
		if( p < -1 )
			p = pages - 1;
		if( p >= 0 )
			modCur = p;
		for( int i=0; i<pages; i++ )
			if( i == p )
				osd.showGroup( modPage[i] );
			else
				osd.hideGroup( modPage[i] );
		if( p >= 0 && modPageText )
			modPageText.changeText( Integer.toString( p + 1 ) + " / " + Integer.toString( pages ) );
	}

	// ---------------------------------------------------------------- behaviour

	void selectPage( int p )
	{
		hideSub( 0 );
		for( int i=0; i<PAGES; i++ )
			if( i != p )
				osd.hideGroup( page[i] );
		osd.showGroup( page[p] );
		if( p == PAGE_MODS )
			showModPage( modCur );
		else
			showModPage( -1 );
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
		tyreSmoke.setValue( option(33) );
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
		if( getKey >= 0 )
			InputDevices.captureCancel();
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

		// The assignment dialog is modal: a click anywhere cancels it.
		if( getKey >= 0 )
		{
			if( getKeyStat == 0 )
			{
				InputDevices.captureCancel();
				getKeyStat = 1;
			}
			return;
		}

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
			if( cmd - CMD_SUB == SUB_CAR )
				showCar();
			else
				showSub( cmd - CMD_SUB );
			return;
		}

		if( cmd >= CMD_CLASS && cmd < CMD_CLASS + ControlSet.CLASSES )
		{
			switchCarClass( cmd - CMD_CLASS );
			return;
		}

		if( cmd == CMD_FFB_TEST )
		{
			int w = InputDevices.forceFeedbackIndex();
			if( w >= 0 && InputDevices.testForceFeedback( w ) )
				toast.show( "TESTING " + InputDevices.name( w ), 1.5 );
			else
				toast.show( "NO FORCE FEEDBACK WHEEL CONNECTED", 2.5 );
			return;
		}

		if( cmd == CMD_GEARBOX || cmd == CMD_CLUTCH_MODE )
		{
			setTransmission( cmd );
			return;
		}

		if( cmd == CMD_FFB_PRESET )
		{
			applyFfbPreset( ffbPreset.value );
			return;
		}

		// A hand-set wheel setting leaves the preset.
		if( ffbPresetting == 0 && ffbPreset.value != 0 )
		{
			if( cmd == OptionsDialog.CMD_FFB_MIN_FORCE || cmd == OptionsDialog.CMD_FFB_SMOOTHING ||
				cmd == OptionsDialog.CMD_FFB_VIBRATION || cmd == OptionsDialog.CMD_FFB_DAMPER )
				ffbPreset.setValue( 0 );
		}

		if( cmd == CMD_SUB_BACK )
		{
			hideSub( 1 );
			return;
		}

		if( cmd == CMD_MODPAGE_PREV || cmd == CMD_MODPAGE_NEXT )
		{
			int np = modCur - 1;
			if( cmd == CMD_MODPAGE_NEXT )
				np = modCur + 1;
			if( np < 0 )
				np = modPage.length - 1;
			showModPage( np );
			return;
		}

		if( cmd == CMD_MODKEY_PREV || cmd == CMD_MODKEY_NEXT )
		{
			if( cmd == CMD_MODKEY_NEXT )
				modKeyPage( 1 );
			else
				modKeyPage( -1 );
			return;
		}

		if( cmd >= CMD_MODKEY && cmd < CMD_MODKEY + MODKEY_ROWS )
		{
			int mk = modKeyCur*MODKEY_ROWS + cmd - CMD_MODKEY;
			if( mk < ModKeys.count() )
				startCapture( ModKeys.control( mk ) );
			return;
		}

		if( cmd >= CMD_MODOPT && cmd < CMD_MODOPT + ModOptions.count() )
		{
			int row = cmd - CMD_MODOPT;
			Gadget mg = e.gadget;
			if( mg )
			{
				if( mg instanceof Slider )
					ModOptions.setFloat( row, ((Slider)mg).value );
				else
				if( mg instanceof MultiChoice )
					ModOptions.setInt( row, ((MultiChoice)mg).value );
			}
			return;
		}

		if( cmd >= CMD_KEY && cmd < CMD_KEY + ControlSet.NCONTROLS )
		{
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
		if( cmd >= OptionsDialog.CMD_DEAD_ZONE && cmd < OptionsDialog.CMD_DEAD_ZONE + 5 )
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
