package java.render.osdx;

import java.render.*;
import java.io.*;
import java.sound.*;
import java.util.*;
import java.util.resource.*;


//called Osd but it's more and more like a Window
public class Osd extends GameType
{
	//design related constants
	public	final static int COLOR_UNSELECTED = 0xFFFFAE00;
	public	final static int COLOR_SELECTED   = 0xFFFFFFFF;
	public	final static int COLOR_DISABLED   = 0xFF404040;
	public	final static int COLOR_PRESSED	  = 0xFFA0A0A0;

	public	final static int COLOR_UNSELECTED2= 0xFF808080;

	public	final static int  RID_SLD_BACK = frontend:0x0089r;
	public	final static int  RID_SLD_KNOB = frontend:0x0091r;

	public	final static int  RID_ARROWUP = frontend:0x004Cr;
	public	final static int  RID_ARROWDN = frontend:0x0084r;
	public	final static int  RID_ARROWLF = frontend:0x0085r;
	public	final static int  RID_ARROWRG = frontend:0x0088r;

	public	final static int  RID_ARROWUP_HL = frontend:0x004Cr;
	public	final static int  RID_ARROWDN_HL = frontend:0x0084r;

	final static ResourceRef RRT_HEADERBG; // = new ResourceRef(frontend:0x0028r);

	final static ResourceRef	RRT_TOOLTIP = new ResourceRef(frontend:0x00CBr);

	//internal constants

	final static int HL_LABELCOLOR	  = 0x00000001;
	final static int HL_BACKLIGHT	  = 0x00000002;

	public	final static int MD_VERTICAL	= 0;
	public	final static int MD_HORIZONTAL = 1;

	public	final static ResourceRef RRT_NONE = new ResourceRef(frontend:0x0018r);	//atlatszo textura, mesh-t se renderelunk ha ezt kapja
	public	final static ResourceRef RRT_EMPTY = new ResourceRef(frontend:0x00AFr);		//default ures atlatszo textura
	public	final static ResourceRef RRT_WHITEALPHA = new ResourceRef(frontend:0x00A2r);	//sotetito textura

	public	final static ResourceRef RRT_TEST = RRT_NONE;
//	final static ResourceRef RRT_TEST = new ResourceRef(frontend:0x0065r);

	public	final static int HK_STATIC   = 0x04;
	final static int PRIVATEEVENT = 0x80000000;

	final static int CMD_MENU_INC = 0x00010000;
	final static int CMD_MENU_DEC = 0x00020000;
	final static int CMD_MENU_UP =	0x00030000;
	final static int CMD_MENU_DN =	0x00040000;
	final static int CMD_MENU_RG =	0x00050000;
	final static int CMD_MENU_LF =	0x00060000;
	final static int CMD_MENU_SEL=	0x00070000;

	public	final static float CAM_AOV = 30;		//osd camera latoszoge 
	public	final static float CAM_OFFSET = 5.48;	//pozicioja (vpHeight foggo!)
	public	final static float SCALE_FS = 1.4685;	//1x1m mesh fullscreen scale faktora (y) (FS = 2.0 width!)  (jo a (-1;1) FS tartomanynak a 3Ds ter Z=0 FS sikrakepezesehez)
	public	final static float LAYER_FACTOR = 0.0001;	//

	public	final static int FADE_OFF		= 0;
	public	final static int FADE_DONOTWAIT = 1;
	public	final static int FADE_WAIT		= 2;
	
	//Osd flags
	public	final static int OF_MODAL			= 0x00000001;		//jobb modal input kezeleshez (egyelore csak a click()-re hat, bovitendo!)
	public	final static int OF_NOAUTOUNFOCUS	= 0x00000002;		//

	public	int				flags;

	//Event Flag Mask
	int				eventFilter = Event.F_GAD_CHANGE|Event.F_GAD_LCLICK;

	//ParticleSystem	psPress, psMove;

	Viewport		vp;
	Camera			cam;

	public	float			screenAspect, cameraAspect;
	public	float			vpAspect, vpWidth, vpHeight, vpTop, vpLeft;
	public	int				orientation, menuKeysCreated;

	int				eatUpNext;

	public/*private*/ EventReceiver	globalHandler;

	//mennyire legyen interaktiv az osd
	public	final static int IL_NONE = 0;	//csak renderelget
	public	final static int IL_TIPS = 1;	//rajta levo gadgetek tooltipjeit megmutatja
	public	final static int IL_KEYS = 2;	//mukodnek a hotkeyek is
	public	final static int IL_FULL = 3;	//teljes controll

	public	int iLevel=IL_FULL;	//egyelore csak hideolt allapotban szabad kivulrol allitani, nem onmukodo (meg)!

	float			lastUserActivityTimeStamp;
	int				focusTracker;

	int				controlCount;	//for un-running keys
	int				prevKey;
	int				prevKeyCount;

	public	Vector			groups = new Vector();
	public	Vector			hotkey = new Vector();	//for osd-global hotkeys

	public	int				defSelection;
	public	Gadget			pointedGadget;			//amire mutatunk, vagy allunk a billentyukkel
//	Vector3			pointedGadgetpos;		//for text shaking

	public	int				clickLock;
	public	Gadget			actDown;				//az epp lenyomva tartott gadget
	public	Gadget			actGadget;				//az epp mukodo gadget (stringinput, etc)

	public	static	int		controlMode;			//0:mouse; 1:keyboard

	Group			lastPointedGadgetGroup;
	Gadget			lastPointedGadget;		//itt jegyezzuk meg az utoljara kijelolt gadgetet
											//akkor kell, ha billentyus kontrollra valtunk

	//toltip, v1
	Text			tooltipText;
	Rectangle		tooltipRect;

	//tooltip, v2
	TextBox			tooltipTextBox;

	float				scrollmargin_top;
	float				scrollmargin_bottom;
	float				scrollmargin_left;
	float				scrollmargin_right;

	static String title;

	//def fullscreen viewportot fog hasznalni
	public Osd()
	{
		this( 1.0, 0.0, 10 );
	}

	public Osd( float size, float aspect, int pri )
	{
		this( new Viewport( pri, 0.5, 0.5, size, aspect, -0.5, -0.5 ) );
	}

	public Osd( float size, float aspect, float cx, float cy, float refx, float refy, int pri )
	{
		this( new Viewport( pri, cx, cy, size, aspect, refx, refy ) );
	}


	private Osd( Viewport viewport )
	{
		createNativeInstance();
		OsdHost.attach( this );	//OSD v2: cursor events go to the groups

		vp = viewport;

		setFlags( WORLDTREEROOT );

		vpWidth=vp.getWidth();
		vpHeight=vp.getHeight();
		vpAspect=vp.getAspect();
		vpTop=vp.getTop();
		vpLeft=vp.getLeft();

		//create default group
		groups.addElement( new Group(this) );

		scrollmargin_top = -0.9;			// <
		scrollmargin_bottom = 0.9;		// >=
		scrollmargin_left = 0.0;			// <
		scrollmargin_right = 0.0;			// >=
	}

	public void finalize()
	{
		//hide();	->emiatt megfagy ALTF4-es kilepeskor,
		//mivel eloszor megszunit az inputqueue ->lehet h a statikuskent letrejott instanceokat a vegere kellene hagyni?
		//talan most mar jo lenne

		//groups.removeAllElements();	//a destroyt a rootokra a kozos root vegzi el, amit a super.finalize intez!
		for( int i=groups.size()-1; i>=0; i-- )
			((Group)groups.elementAt(i)).deactivate();

		while( !hotkey.isEmpty() )
		{
			Hotkey hk =  hotkey.removeLastElement();
			hk.inactivate();
		}

		vp.destroy();

		super.finalize();
	}

	public void rebuild()
	{
		int hidden = (cam == null);

		if( !hidden )
			hide();

		vp.destroy();
		vp = new Viewport( 10, vpTop, vpLeft, vpWidth, vpHeight );

		vpAspect=vp.getAspect();

		//text, textbox, gadget.label.txt....
		//Text tartson nyilvan egy listat?
		//hash tabla kell hogy legyen, kulonben q lassu lesz!!
		//oooo, mir nem skalazhatoak a fontjaink!!!!!????!!!


		if( !hidden )
			show();
	}

	public Viewport getViewport()
	{
		return vp;
	}

	//a rakovetkezo gadget, hotkey, stb hivasok ide kuldik az eventjeiket!
	public void setEventHandler( EventReceiver er )
	{
		globalHandler = er;
	}

	public void addEventFlags( int flags )
	{
		eventFilter |= flags;
	}

	public void remEventFlags( int flags )
	{
		eventFilter &= ~flags;
	}

	//altalanositas szukseges!!
	//barmalyik gadget [ amelyik implemental ilyen 'mousemapper' interfacet (x0, x1, y0, y1 ) ]
	//kerhet hookot az eger mozgasara akar sima, akar clickelt allapotban
	//ezalatt eventeket kell kapjon az eger poziciojarol, ha az valtozik (mar mappelve!)
	public float mapClickPosH( Vector3 cv, float x0, float x1 )
	{
		cv.x = (cv.x - (vpLeft + vpLeft + vpWidth - 1.0)) / vpWidth;
		cv.y = (cv.y - (vpTop + vpTop + vpHeight - 1.0)) / vpHeight;

		if (cv.x < x0)
			cv.x = x0;
		if (cv.x > x1)
			cv.x = x1;

		return (cv.x - x0) / (x1-x0);
	}

	public int idleFor( float sec )
	{
		//nem tudjuk, mikor jon le rola/m,egy ra a focus az osd-re:(
		if( focusTracker )
		{
			if( isFocused() )
				return ( lastUserActivityTimeStamp+sec < System.currentTime() );
			else
				focusTracker = 0;
		}
		else
		{
			if( isFocused() )
			{
				lastUserActivityTimeStamp = System.currentTime();
				focusTracker = 1;
			}
		}

		return 0;
	}

	public float getIdleTime()
	{
		return 	System.currentTime() - lastUserActivityTimeStamp;
	}

	public void setIdleTime(float sec)
	{
		lastUserActivityTimeStamp = System.currentTime() - sec;
	}



	public void unFocusGadget()
	{
		if( pointedGadget )
		{
			//restore shaking texts
			//Text text = pointedGadget.getLabel();
			//if (text)
			//	text.setPos(pointedGadgetpos.x, pointedGadgetpos.y);

			pointedGadget.unfocus();
			pointedGadget = null;
			//pointedGadgetpos = null;

			if( tooltipText )
			{
				tooltipText.changeText( null );
				tooltipText = null;

				tooltipRect.finalize();
				tooltipRect = null;
			}
			else
			if( tooltipTextBox )
				tooltipTextBox.changeText( null );

		}
	}

	public void focusGadget(Group grp, Gadget g)
	{
		if( g )
		{
			lastUserActivityTimeStamp = System.currentTime();

			pointedGadget = g;
			lastPointedGadget = g;
			lastPointedGadgetGroup = grp;

			//for text shaking
			//Text text = pointedGadget.getLabel();
			//if (text)
			//	pointedGadgetpos = text.getPos();

			g.focus();

			if( tooltipTextBox && pointedGadget.tooltip )
				tooltipTextBox.changeText( pointedGadget.tooltip );

			//visszamentek egy eredetileg lenyomottra, amirol eljottek, de kozben nyomva tartottak
			if( actDown == pointedGadget )
			{
				pointedGadget.press();
			}
		}
	}

	//directly focuses on the given gadget (if it's group active!)
	public void changeFocusGadget( Gadget mygad )
	{
		Group grp;
		Gadget gad;

		for( int grpIndex=0; grpIndex<groups.size(); grpIndex++ )
		{
			grp = groups.elementAt(grpIndex);

			if( grp.active )
			{
				for( int index=grp.gadget.size()-1; index>=0; index-- )
				{
					gad = grp.gadget.elementAt(index);

					if( gad == mygad )
					{
						if( !(gad.disabled || gad.nofocus) )
						{
							unFocusGadget();
							focusGadget( grp, gad );
						}
						return;
					}
				}
			}
		}
	}


	public void changeFocusGadget( int direction, int playsnd )
	{
		Group	grp, newgrp;
		Gadget	gad, newgad;
		int		grpIndex, index;

		if( !pointedGadget )
			direction = -1;

		//if( direction < 0 )
		//	controlMode = 1;		//billentyus kontroll ON!

		if( direction < 0 )
		{
//			System.log("chgFocus "+direction);
			for( grpIndex=0; grpIndex<groups.size(); grpIndex++ )
			{
				grp = groups.elementAt(grpIndex);

				if( grp.active )
				{
					for( index=-direction-1; index<grp.gadget.size(); index++ )
					{
						gad = grp.gadget.elementAt(index);
//						System.log("gadget#"+index+" = "+gad.getLabelText());

						if( !(gad.disabled || gad.nofocus || gad.nokeyfocus) )
						{
							newgad=gad;
							newgrp=grp;
							break;
						}

						gad = grp.gadget.elementAt(index);
					}
				}

				if( newgad )
					break;
			}
		}
		else
		{
			Vector3 cur = pointedGadget.getHotSpot();

			float	d=100000;
			float	t;

			for( grpIndex=0; grpIndex<groups.size(); grpIndex++ )
			{
				grp = groups.elementAt(grpIndex);

				if( grp.active )
				{
					for( index=0; index<grp.gadget.size(); index++ )
					{
						gad = grp.gadget.elementAt(index);

						if( !(gad.disabled || gad.nofocus || gad.nokeyfocus) && gad!=pointedGadget )
						{
							Vector3 pos = gad.getHotSpot();
							pos.sub( cur );
							
							//fol/le/bal/jobb
							if( direction == 0 && pos.y > 0 && (t=pos.x*pos.x+pos.y*pos.y*0.4) < d ||
								direction == 1 && pos.y < 0 && (t=pos.x*pos.x+pos.y*pos.y*0.4) < d ||
								direction == 2 && pos.x < 0 && (t=pos.x*pos.x*0.14+pos.y*pos.y) < d ||
								direction == 3 && pos.x > 0 && (t=pos.x*pos.x*0.14+pos.y*pos.y) < d 	)
							{
								d=t;
								newgad=gad;
								newgrp=grp;
							}
						}
					}
				}
			}
		}

		if( newgad )
		{
			Group lastGroup = lastPointedGadgetGroup;
			unFocusGadget();
			focusGadget( newgrp, newgad );

			//group changed?
			if( lastGroup && lastGroup != lastPointedGadgetGroup )
			{
				if( lastGroup.flags & Group.AUTOHIDE )
					lastGroup.deactivate();
			}

			if( playsnd )
			{
				new SfxRef( Frontend.SFX_MENU_MOVE ).play();
			}
		}
	}

	// The SLRR engine calls the animate hook without arguments; the CRC
	// engine passed the frame time. Slider dragging and the gadget slide-in
	// animations live in animate(dt).
	float	lastAnimateTime;

	// OpenSLRR: objects moved every frame (LevelBar, OsdAnimator...).
	public	Vector	animated = new Vector();
	// 0: gadgets do not play the CRC label slide-in when built (a screen that
	// animates its pages with an OsdAnimator turns it off).
	public	int		gadgetIntro = 1;

	public void addAnimated( Animated a )
	{
		animated.addElement( a );
	}

	public void removeAnimated( Animated a )
	{
		animated.removeElement( a );
	}

	public void animate()
	{
		float now = System.currentTime();
		float dt = 0.0;
		if( lastAnimateTime > 0.0 )
			dt = now - lastAnimateTime;
		if( dt > 0.1 )
			dt = 0.1;	// after a pause, do not skip the animation
		if( dt < 0.0 )
			dt = 0.0;
		lastAnimateTime = now;
		animate( dt );
	}

	// Replays the slide-in of every gadget of a group (a page shown again).
	public void animateGroupIn( int hash )
	{
		for( int i=groups.size()-1; i>=0; i-- )
		{
			Group g = groups.elementAt(i);
			if( g.hashCode() == hash )
			{
				for( int j=g.gadget.size()-1; j>=0; j-- )
					((Gadget)g.gadget.elementAt(j)).restartAnimate();
				break;
			}
		}
	}

	public void animate(float dt)
	{
		for( int i=animated.size()-1; i>=0; i-- )
			((Animated)animated.elementAt(i)).tick( dt );

		// animate groups
		int grps = groups.size();
		for( int i=groups.size()-1; i>=0; i-- )
		{
			Group			group = groups.elementAt(i);
			if ( group && group.active )
				group.animate(dt);
			if (grps != groups.size())
				System.exit("Osd internal error 490");
		}


		//tooltip handling
		if( pointedGadget && !controlMode && pointedGadget.tooltip && !tooltipTextBox && !tooltipText && idleFor(1) )
		{
			ResourceRef	tooltipFont = Frontend.smallFont;
			Vector3 cv = Input.cursor.getPos();

			float	fontYCorrection = 0.4;
			float	aspect = Input.cursor.vp.getAspect();

			float	h = Text.getHeight( tooltipFont, Input.cursor.vp );
			float	w = Text.getWidth(pointedGadget.tooltip, tooltipFont, Input.cursor.vp);
			float	border = h*0.6;

			h+=border;
			w+=border;

			cv.x+=w*0.5+0.05;
			cv.y+=h*0.5+0.1;

			float right = cv.x + 0.5*w;
			if( right > 1.0 )
				cv.x-=right - 1.0;

			float bottom = cv.y + 0.5*h;
			if( bottom > 1.0 )
				cv.y-=bottom - 1.0;

			tooltipText = new Text( Input.cursor, tooltipFont, pointedGadget.tooltip, cv.x, cv.y-fontYCorrection*h );
			tooltipText.changeAlign( Text.ALIGN_CENTER );

			cv.x *= Osd.SCALE_FS*aspect;
			cv.y *= -Osd.SCALE_FS;

			tooltipRect = new Rectangle( Input.cursor, cv, new RectangleTemplate( w*Osd.SCALE_FS*aspect, h*Osd.SCALE_FS, RRT_TOOLTIP ) );
		}

		if( !isFocused() )
			unFocusGadget();


		if( Frontend.inputQueue.lastElement() == this )
		{
			controlCount++;

			int lastKey = Input.lastKey();
			
			if( controlMode && idleFor( 10 ) && !(flags&OF_NOAUTOUNFOCUS) )
			{
				unFocusGadget();
				controlMode = 0;	//billentyus kontroll AUTO OFF!
			}

			//text shaking
			//if ( pointedGadget && pointedGadgetpos)
			//{
			//	Text text = pointedGadget.getLabel();
			//	if (text)
			//		text.setPos(pointedGadgetpos.x-0.01f+Math.random()*0.02f, pointedGadgetpos.y-0.01f+Math.random()*0.02f);
			//
			//	if (actDown == pointedGadget)
			//		pressEffect();
			//}

			if( lastKey )
			{
				int runningKey;
				if( prevKey==lastKey && prevKeyCount+1==controlCount)
					runningKey=1;
				
				prevKeyCount=controlCount;
				prevKey=lastKey;

				/* nativ szinten implementalva!!
				else
				//hotkey kezeles
				if( !hotkey.isEmpty() )
				{
					Hotkey hk;
					for( int i = hotkey.size()-1; i >= 0 ; i-- )
					{
						hk = hotkey.elementAt(i);
						if( ( lastKey & 0xFF ) == hk.key )
						{
							Integer wrapper = groupStates.elementAt(hk.group);
							if( wrapper.intValue() == 1 )
							{	//in active group!
								hk.handler.handleEvent( new Event( hk.command, hk.state ));
								break;
							}
						}
					}
				}
				*/
			}

			//slider handling
			if( pointedGadget instanceof Slider )
			{
				if( pointedGadget.pressed )
				{
					Slider sl = (Slider)pointedGadget;

					Vector3 cv = Input.cursor.getPos();
					float value = mapClickPosH( cv, sl.x0, sl.x1 ) * (sl.b - sl.a) + sl.a;						

					if( value != sl.value )
					{
						lastUserActivityTimeStamp = System.currentTime();

						sl.setValue( value );
						sl.sendEvent(Event.F_GAD_CHANGE);
					}
				}
			}

			//string auto select handling
			if( !actGadget && pointedGadget instanceof StringInput )
			{
				//int ascii = lastKey >> 16;
				if( lastKey >= 33 && lastKey <= 126 )
					pointedGadget.click();
			}

			//string input handling
			if( actGadget instanceof StringInput )
			{
				//int ascii = lastKey >> 16;
				if( lastKey > 0 )
				{
					lastUserActivityTimeStamp = System.currentTime();

					if( ((StringInput)actGadget).key( lastKey ) )
					{
						new SfxRef( Frontend.SFX_MENU_SELECT ).play();
					}
				}
			}
		}
	}

	//handle hotkeys
	public void handleEvent( Hotkey hk )
	{
		lastUserActivityTimeStamp = System.currentTime();

		int command = hk.command;
		// SLRR Hotkey has no negCommand.

		if( command & PRIVATEEVENT )
		{
			command &= ~PRIVATEEVENT;

			if( eatUpNext )
			{
				eatUpNext--;
				return;
			}

			if( Frontend.inputQueue.lastElement() == this && iLevel >= IL_FULL )
			{
				if( !actGadget )//csak ha nincs stringinput, etc hiperaktiv gadget!
				{
					if( !controlMode && (command == CMD_MENU_UP || command == CMD_MENU_DN || command == CMD_MENU_LF || command == CMD_MENU_RG || command == CMD_MENU_SEL) && !pointedGadget && lastPointedGadgetGroup)
					{
						focusGadget( lastPointedGadgetGroup, lastPointedGadget );
						controlMode = 1;	//billentyus kontroll ON!
					}
					else
					{
						if( !controlMode )
							controlMode = 1;	//billentyus kontroll ON!

						if( command == CMD_MENU_UP )
						{
							changeFocusGadget( 0, 1 );
						}
						else
						if( command == CMD_MENU_DN )
						{
							changeFocusGadget( 1, 1 );
						}
						else
						if( command == CMD_MENU_LF )
						{
							changeFocusGadget( 2, 1 );
						}
						else
						if( command == CMD_MENU_RG )
						{
							changeFocusGadget( 3, 1 );
						}
						else 
						if( command == CMD_MENU_SEL )
						{
							if( pointedGadget )
								if( !pointedGadget.disabled )
								{
									if( hk.state )
									{	//lenyomas
										new SfxRef( Frontend.SFX_MENU_SELECT ).play(); 
										pointedGadget.press();	//ENTER KEY -> press
										actDown = pointedGadget;
									}
									else
									{	//felengedes
										if( actDown == pointedGadget )
										{
											pointedGadget.release();
											pointedGadget.click();	//ENTER KEY
										}
										actDown = null;
									}
								}
								else
									if( hk.state )
										new SfxRef( Frontend.SFX_WARNING ).play(); 

						}
						else 
						if( command == CMD_MENU_INC )
						{
							if( pointedGadget )
							{
								pointedGadget.increase();
							}
						}
						else 
						if( command == CMD_MENU_DEC )
						{
							if( pointedGadget )
							{
								pointedGadget.decrease();
							}
						}
					}
				}
			}
		}
		else
		{
			Event e = new Event( hk.command, hk.state );

			if( hk.handler )
				hk.handler.handleEvent( e );
		}
	}

	public void show()
	{
		show(FADE_DONOTWAIT, 0.25);
	}

	//for compatibility:
	public void show(int fade, float time)
	{
		if (fade!=FADE_OFF)
		{
			// SLRR has no FullscreenFade: show at once.
		}
		show(1);
	}

	public void hide()
	{
		hide(1);
	}


	//full=0: akkor erdekes, ha csak az osd rendereleset akarjuk kikapcsolni valamiert (keyek, etc marad)
	public void show( int full )
	{
//		System.profileStartTimer("java_Osd_show");

//		System.log("defSel = "+defSelection);
		if( !pointedGadget )
			changeFocusGadget( -1, 0 );	//must initialize for the first time to make it work
		changeFocusGadget( -(defSelection+1), 0 );

		lastUserActivityTimeStamp = System.currentTime();

		cam = new Camera( this, vp, 2, CAM_AOV, 0.1, 20.0 );
		cam.setMatrix( new Vector3(0.0, 0.0, CAM_OFFSET*vpHeight), null );

		if( full )
		{
			vp.activate( Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET );

			if( iLevel >= IL_TIPS )
			{
				enableAnimateHook();	//needed for tooltips

				if( iLevel >= IL_KEYS )
				{
					setEventMask( EVENT_HOTKEY );

					if( iLevel >= IL_FULL )
					{
						if( !menuKeysCreated )
							createMenuKeys();
					}

					setFocus(1);
				}
			}
		}

		for( int i=groups.size()-1; i>=0; i-- )
			((Group)groups.elementAt(i)).show();

//		System.profileStopTimer("java_Osd_show");
	}

	public void hide( int full )
	{
		unFocusGadget();
		if( actGadget )
		{
			actGadget.stop();
			actGadget = null;
		}

		if( full )
		{
			if( iLevel >= IL_TIPS )
			{
				disableAnimateHook();

				if( iLevel >= IL_KEYS )
				{
					setFocus(0);
					clearEventMask( EVENT_ANY );
				}
			}

			vp.deactivate();
		}

		if( cam )
		{	
			cam.destroy();	//stop rendering right now
			cam = null;		//wait for gc to collect
		}

		for( int i=groups.size()-1; i>=0; i-- )
			((Group)groups.elementAt(i)).hide();
	}

	//ha valaki allitgatni akarja 1 gadget erteket, de nem tarolta el:
	public Gadget findGadget( Object h, int cmd )
	{
		int ng = groups.size();
		while( ng-- )
		{
			Group grp = groups.elementAt( ng );
			
			int n=grp.gadget.size();
			Gadget g;

			while( n-- )
			{
				g = grp.gadget.elementAt(n);
				if( g.handler == h && g.cmd == cmd )
				{
					//lehet tobb ilyen is, csak az utolsot adjuk vissza!
					return g;
				}
			}
		}
		return null;
	}

	//pri: higher overlaps lower
	public Vector3 convertTextCoordinates( float x, float y, int pri )
	{
		return new Vector3( x*SCALE_FS*vpAspect*vpWidth, -y*SCALE_FS*vpHeight, pri*LAYER_FACTOR );
	}

	public void addHook( GameRef obj, GameType handler )
	{
		addHook( obj, handler, null );
	}

	public void addHook( GameRef obj, GameType handler, String handlerName )
	{
		handler.addNotification( obj, EVENT_CURSOR, EVENT_SAME, null, "event_handler" + handlerName );
	}

	public void setTooltipBox( TextBox tb )
	{
		tooltipTextBox = tb;
	}


	//osd builder methods:
	public Menu createMenu( Style s, float x, float y, float spc, int ori )
	{
		return new Menu( this, s, x, y, spc, ori );
	}

	public Menu createMenu( Style s, float x, float y, float spc )
	{
		return new Menu( this, s, x, y, spc );
	}

	public Menu createMenu( Style s, float x, float y, float spc, Style ss, Style sks )
	{
		Menu m = new Menu( this, s, x, y, spc );
		m.setSliderStyle( ss, sks );
		return m;
	}

	public Text createHeader( String title )
	{
		if( RRT_HEADERBG )
			createRectangle( 0.0, -0.95, 2.0, 0.1, -1, RRT_HEADERBG );

		Text txt = createText( title, Frontend.smallFont, Text.ALIGN_CENTER,	0.74, -0.96 );
		return txt;
	}

	public void setTitle( String t)
	{
		title = t;
	}

	public String getTitle()
	{
		return title;
	}

	public Rectangle createBG( ResourceRef texture )
	{
		return createRectangle( 0, 0, 2, 2, -2, texture );
	}

	public void createWindowBG()
	{
		int borderColor = 0xFFFF0000;
		int bgColor = 0xEE333333;
		float borderWidth = 0.001;

		//a leheto legnagyobb 4/3-as screen magassaganak x%-a, 
		//1:1 es pixelaspectet feltetelezve
		float aspect = Config.video_x/Config.video_y;
		int bpix = Config.video_y * ( aspect > 4.0/3.0 ? 1.0: 1/aspect ) *borderWidth;
		if( !bpix )
			bpix++;
		
		float wpix = Config.video_x*vpWidth;
		float hpix = Config.video_y*vpHeight;

		float w=2.0*bpix/wpix;
		float h=2.0*bpix/hpix;
		createRectangle( 0, h/2-1, 2, h, -1, Osd.RRT_WHITEALPHA, borderColor );
		createRectangle( 0, 1-h/2, 2, h, -1, Osd.RRT_WHITEALPHA, borderColor );
		createRectangle( w/2-1, 0, w, 2, -1, Osd.RRT_WHITEALPHA, borderColor );
		createRectangle( 1-w/2, 0, w, 2, -1, Osd.RRT_WHITEALPHA, borderColor );

		createRectangle( 0, 0, 2-2*w, 2-2*h, -2, Osd.RRT_WHITEALPHA, bgColor );
	}

	//------------rectangles----------
	///textured
	public Rectangle createRectangle( float x, float y, float w, float h, int pri, ResourceRef texture )
	{
		return createRectangle( x, y, w, h, vpAspect, 0.0, 0.0, pri, texture );
	}

	public Rectangle createRectangle( float x, float y, float w, float h, float aspect, float dx, float dy, int pri, ResourceRef texture )
	{
		Group	group = groups.lastElement();

		x += dx*w*aspect/vpAspect;
		y += dy*h;

		Vector3 pos = group.convertTextCoordinates( x, y, pri );

		Rectangle rect =  new Rectangle( group, pos, new RectangleTemplate( w*Osd.SCALE_FS*aspect*vpWidth, h*Osd.SCALE_FS*vpHeight, texture ) );
		group.misc.addElement( rect );
		return rect;
	}

	///colored
	public Rectangle createRectangle( float x, float y, float w, float h, int pri, int color )
	{
		return createRectangle( x, y, w, h, vpAspect, 0.0, 0.0, pri, color );
	}


	public Rectangle createRectangle( float x, float y, float w, float h, float aspect, float dx, float dy, int pri, int color )
	{
		Group	group = groups.lastElement();

		x += dx*w*aspect/vpAspect;
		y += dy*h;

		Vector3 pos = group.convertTextCoordinates( x, y, pri );

		Rectangle rect =  new Rectangle( group, pos, new RectangleTemplate( w*Osd.SCALE_FS*aspect*vpWidth, h*Osd.SCALE_FS*vpHeight, color ) );
		rect.setColor( color );

		group.misc.addElement( rect );
		return rect;
	}

	///textured and colored
	public Rectangle createRectangle( float x, float y, float w, float h, int pri, ResourceRef texture, int color )
	{
		return createRectangle( x, y, w, h, vpAspect, 0.0, 0.0, pri, texture, color );
	}


	public Rectangle createRectangle( float x, float y, float w, float h, float aspect, float dx, float dy, int pri, ResourceRef texture, int color )
	{
		Group	group = groups.lastElement();

		x += dx*w*aspect/vpAspect;
		y += dy*h;

		Vector3 pos = group.convertTextCoordinates( x, y, pri );

		Rectangle rect =  new Rectangle( group, pos, new RectangleTemplate( w*Osd.SCALE_FS*aspect*vpWidth, h*Osd.SCALE_FS*vpHeight, texture, color ) );
		rect.setColor( color );

		group.misc.addElement( rect );
		return rect;
	}

	public void deleteRectangle( Rectangle rect )
	{
		if ( rect )
		for( int i=groups.size()-1; i>=0; i-- )
			((Group)groups.elementAt(i)).misc.removeElement( rect );
	}

	//----------text & textbox----------
	public Text createText( String string, ResourceRef charset, int align, float x, float y, int line )
	{
		Group group = groups.lastElement();

		Text txt = new Text( group, charset, string, x, y, line, vp );
		txt.changeAlign( align );
		group.misc.addElement( txt );
		return txt;
	}

	public Text createText( String string, ResourceRef charset, int align, float x, float y )
	{
		return createText( string, charset, align, x, y, 0 );
	}

	public TextBox createTextBox( String string, ResourceRef charset, int align, float x, float y, float width )
	{
		return createTextBox( string, charset, align, x, y, width, null );
	}

	public TextBox createTextBox( String string, ResourceRef charset, int align, float x, float y, float width, ResourceRef bg )
	{
		Group group = groups.lastElement();

		TextBox tb = new TextBox( group, charset, vp, align, string, x, y, width, bg );
		group.misc.addElement( tb );
		return tb;
	}

	//-------------gadgets-------------

	//-------------buttons-------------
	//icon buttons
	public Button createButton( Style sty, float x, float y, int command, String tooltip )
	{
		return createButton( sty, x, y, 0, command, tooltip );
	}

	public Button createButton( Style sty, float x, float y, int pri, int command, String tooltip )
	{
		Button b = createButton( sty, x, y, pri, null, command );
		b.setToolTip( tooltip );
		b.highliteMethod=HL_BACKLIGHT;
		return b;
	}

	//szoveges buttons:
	public Button createButton( Style sty, float x, float y, String label, int command )
	{
		return createButton( sty, x, y, 0, label, command );
	}

	public Button createButton( Style sty, float x, float y, int pri, String label, int command )
	{
		return new Button( this, sty, x, y, pri, label, command );
	}

	public TextButton createButton( Style sty, float x, float y, int pri, String label, int command, String value )
	{
		return new TextButton( this, sty, x, y, pri, label, command, value );
	}

	//progress bar
	public ProgressBar createProgressBar( Style sty, float x, float y, String label, float value )
	{
		return new ProgressBar( this, sty, x, y, 0, label, value );
	}

	public ProgressBar createProgressBar( Style sty, float x, float y, int pri, String label, float value )
	{
		return new ProgressBar( this, sty, x, y, pri, label, value );
	}

	//extra, intervallumozhato es diszkretizalhato!
	public Slider createSlider( Style sty, Style styKnob, float x, float y, String label, int command, float value, float a, float b, int t )
	{
		Slider s = createSlider( sty, styKnob, x, y, label, command, value);
		s.setRange( a, b );
		s.setTicks( t );
		s.setValue( value );
		return s;
	}

	public Slider createSlider( Style sty, Style styKnob, float x, float y, String label, int command, float value )
	{
		return new Slider( this, sty, styKnob, x, y, 0, label, command, value );
	}

	public MultiChoice createMulti( Style sty, float x, float y, String label, int command, int value, Vector chs, int flags, int cs, int cu )
	{
		return new MultiChoice( this, sty, x, y, 0, label, command, value, chs, flags | MultiChoice.MCAF_LOOP, cs, cu);
	}

	public MultiChoice createMulti( Style sty, float x, float y, String label, int command, int value, Vector chs, int flags )
	{
		return new MultiChoice( this, sty, x, y, 0, label, command, value, chs, flags | MultiChoice.MCAF_LOOP );
	}

	public MultiChoiceArrow createMultiArrow( Style sty, Style LBS, Style RBS, float x, float y, String label, int command, int value, Vector chs, int flags )
	{
		return new MultiChoiceArrow( this, sty, LBS, RBS, x, y, 0, label, command, value, chs, flags );
	}

	public MultiChoiceArrow_Header createMultiArrowHeader( Style sty, Style LBS, Style RBS, float x, float y, String label, int command, int value, Vector chs, int flags )
	{
		return new MultiChoiceArrow_Header( this, sty, LBS, RBS, x, y, 0, label, command, value, chs, flags );
	}

	public MultiChoiceNumeric createMultiNumeric( Style sty, float x, float y, String label, int command, int value, int min, int levels, int step, int flags)
	{
		MultiChoiceNumeric	mc = new MultiChoiceNumeric( this, sty, x, y, 0, label, command, min, levels, step, value, flags);
		return mc;
	}
	
	public MultiChoiceNumericArrow createMultiNumericArrow( Style sty, Style LBS, Style RBS, float x, float y, String label, int command, int value, int min, int levels, int step, int flags)
	{
		MultiChoiceNumericArrow	mc = new MultiChoiceNumericArrow( this, sty, LBS, RBS, x, y, 0, label, command, min, levels, step, value, flags);
		return mc;
	}

	public StringInput createStringInput( Style sty, float x, float y, String label, int command, String value )
	{
		return new StringInput( this, sty, x, y, 0, label, command, value );
	}

	public StringInput createStringInput( Style sty, float x, float y, String label, int command, String value, int secure )
	{
		return new StringInput( this, sty, x, y, 0, label, command, value, secure );
	}

	public StringInput_Header createStringInputHeader( Style sty, float x, float y, String label, int command, String value, int secure, int flags )
	{
		return new StringInput_Header( this, sty, x, y, 0, label, command, value, secure, flags );
	}

	//create default menu hotkeys
	public void createMenuKeys()
	{
		createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|HK_STATIC, CMD_MENU_LF|PRIVATEEVENT );
		createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|HK_STATIC, CMD_MENU_RG|PRIVATEEVENT );
		createHotkey( Input.AXIS_MENU_UP, Input.VIRTUAL|HK_STATIC, CMD_MENU_UP|PRIVATEEVENT );
		createHotkey( Input.AXIS_MENU_DOWN, Input.VIRTUAL|HK_STATIC, CMD_MENU_DN|PRIVATEEVENT );
		createHotkey( Input.AXIS_SELECT, Input.VIRTUAL|HK_STATIC, CMD_MENU_SEL|PRIVATEEVENT, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );


		// SLRR Hotkey has no useNegative: steering/gear menu keys dropped.
		//nem tehetem a gazra addig, amig meg nem oldott hogy smootholas nelkuli ertek alapjan mukodjenek a hotkeyek!
		createHotkey( Input.AXIS_HORN, Input.VIRTUAL|HK_STATIC, CMD_MENU_SEL|PRIVATEEVENT, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );

		menuKeysCreated = 1;
	}

	public Hotkey createHotkey( int key, int flags, int command  )
	{
		return createHotkey( key, flags, command, Event.F_KEY_PRESS );
	}

	public Hotkey createHotkey( int key, int flags, int command, int eventflags )
	{
		Group g = groups.lastElement();
		Hotkey hk;

		if( flags & HK_STATIC )
		{
			flags &= ~HK_STATIC;
			hk = new Hotkey( key, flags, command, globalHandler, this, eventflags );
			this.hotkey.addElement( hk );
			hk.activate();
		}
		else
		{
			hk = new Hotkey( key, flags, command, globalHandler, this, eventflags );
			g.hotkey.addElement( hk );
			if( g.active )
				hk.activate();
		}

		return hk;
	}



	//----------group handling-----------
	public int endGroup()
	{
		//eleg hulye megoldas, hogy most csinaljuk a kovetkezot, hat ez igy alakult ki..
		int hash = curGroup();
		groups.addElement( new Group(this) );
		return hash;
	}

	public int curGroup()
	{
		return groups.lastElement().hashCode();
	}

	public void remGroup( int hash )
	{
		for( int i=groups.size()-1; i>=0; i-- )
			if( ((Group)groups.elementAt(i)).hashCode() == hash )
			{
				Group g = groups.removeElementAt(i);
				g.deactivate();

				if( tooltipTextBox && g.misc.contains( tooltipTextBox ) )
					setTooltipBox(null);

				if( g == lastPointedGadgetGroup )
				{
					lastPointedGadgetGroup = null;
					lastPointedGadget = null;
				}
				break;
			}
	}


	public void hideGroup( int hash )
	{
		for( int i=groups.size()-1; i>=0; i-- )
			if( ((Group)groups.elementAt(i)).hashCode() == hash )
			{
				((Group)groups.elementAt(i)).deactivate();
				break;
			}
	}

	public void showGroup( int hash )
	{
		for( int i=groups.size()-1; i>=0; i-- )
			if( ((Group)groups.elementAt(i)).hashCode() == hash )
			{
				((Group)groups.elementAt(i)).activate();
				break;
			}
	}

	public void setGroupFlags( int hash, int flags )
	{
		for( int i=groups.size()-1; i>=0; i-- )
			if( ((Group)groups.elementAt(i)).hashCode() == hash )
			{
				((Group)groups.elementAt(i)).flags = flags;
				break;
			}
	}

	public void setFocus( int f )
	{
		Input.flushKeys();

		if( f )
		{
			Frontend.inputQueue.addElement( this );
		}
		else
		{
			//ez kicsit tulbiztositott megoldas, de jo.
			int index = Frontend.inputQueue.lastIndexOf( this );

			if( index >= 0 )	//lehet, hogy az utolso volt!
			{
				Frontend.inputQueue.removeElementAt( index ); 
			}

		//(azert mert a setFocust a show/hide hivja, es a show-hide nem hivodhat tobbszorosen,
		//rossz sorrendben)

		}
	}

	public int isFocused()
	{
		return ( iLevel < IL_KEYS || Frontend.inputQueue.lastElement() == this);
	}

	public void pressEffect()
	{
/*		Vector3 p = Input.cursor.getPos();
		p.x *=  SCALE_FS*Input.cursor.vp.getAspect();
		p.y *= -SCALE_FS;
		Vector3 v1 = new Vector3(0.0, -0.5, 0.0);

		if( !psPress || !psPress.id() )
			psPress = new ParticleSystem( Input.cursor, new RenderRef(particles:0x0024r) );

		Vector3 v2 = new Vector3(0.0, 0.0, 4.0);
		psPress.setSource("hooo", p, 0.0, 0.1, v2, 0.0, 4.0, 1000.0, null);
*/	}

	public void pressEffectStop()
	{
/*		if( psPress )
			psPress.delAction("hooo");
*/	}

	public void hoverEffect()
	{
/*		Vector3 p = Input.cursor.getPos();
		p.x *=  SCALE_FS*Input.cursor.vp.getAspect();
		p.y *= -SCALE_FS;
		Vector3 v1 = new Vector3(0.0, -0.5, 0.0);

		if( !psMove || !psMove.id() )
			psMove = new ParticleSystem( Input.cursor, new RenderRef(particles:0x0024r) );

		Vector3 v2 = new Vector3(0.0, 0.0, 4.0);
		psMove.setDirectSource("ping", p, 0.0, 0.1, v2, 0.0, 4.0, 50.0, null);
*/	}

	public void leaveEffect()
	{
/*		Vector3 p = Input.cursor.getPos();
		p.x *=  SCALE_FS*Input.cursor.vp.getAspect();
		p.y *= -SCALE_FS;
		Vector3 v1 = new Vector3(0.0, -0.5, 0.0);

		if( !psMove || !psMove.id() )
			psMove = new ParticleSystem( Input.cursor, new RenderRef(particles:0x0024r) );

		Vector3 v2 = new Vector3(0.0, 0.0, 4.0);
		psMove.setDirectSource("ping", p, 0.0, 0.1, v2, 0.0, 4.0, 50.0, null);
*/	}
}

