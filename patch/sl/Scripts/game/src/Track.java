package java.game;

import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.render.*;	//Text
import java.render.osd.*;	//Text
import java.sound.*;
import java.openslrr.*;	// OpenSLRR (#487): mod camera providers

abstract public class Track extends Scene implements GameState
{
	final static int RID_CHEVYVAN = cars.traffic.chevy_van:0x00000005r;

	//ezeket a leszarmazott allitsa be!
	abstract Vector3	posStart, posStart2;
	abstract Ypr		oriStart, oriStart2;

	//hold the id of the currently active trigger (shop, garage, etc)
	int				activeTrigger;
	Vector			trigger = new Vector();

	GameState		parentState;

	Osd				osd, MirrorOsd;
	int				osdCounter;

	IngameMenu		ingameMenu;
	int				im_active;

	int				camRotate;

	int				killCar;
	int				Color_Number;

	final static int CMD_CHANGECAM_TV = 0;
	final static int CMD_INGAMEMENU = 1;
	final static int CMD_MECHANIC = 2;
	final static int CMD_CAMMOVE = 3;
	final static int CMD_OPPCAM = 4;
	final static int CMD_PAINT = 5;
	final static int CMD_OSDONOFF = 6;
	final static int CMD_CHANGECAM_INT = 7;
	final static int CMD_CHANGECAM_EXT = 8;
	final static int CMD_CRUISECONTROL = 9;
	final static int CMD_CHANGECAM_CHASE = 10;
	final static int CMD_CHANGECAM_INVCH = 11;
	final static int CMD_CHANGECAM_FREE = 12;
	final static int CMD_SIMSPEEDINC = 13;
	final static int CMD_SIMSPEEDDEC = 14;
	final static int CMD_SIMPAUSE = 15;
	final static int CMD_CAMROTATE = 16;
	final static int CMD_QUICKREPAIR = 17;
	final static int CMD_CHANGECAMTARGET = 18;
	final static int CMD_EXIT = 19;

	final static int CMD_MIRROR = 20;
	final static int CMD_REPAIR = 21;
	final static int CMD_MAP = 22;
	final static int CMD_LOOKBACK = 23;
	// OpenSLRR (#470): the change-view key (ControlSet slot 35, V).
	final static int CMD_NEXTCAM = 4700;

// Hydraulic suspension:
	final static int	CMD_SUSP_FL		= 41;
	final static int	CMD_SUSP_F		= 42;
	final static int	CMD_SUSP_FR		= 43;
	final static int	CMD_SUSP_LS		= 44;
	final static int	CMD_SUSP_ALL	= 45;
	final static int	CMD_SUSP_RS		= 46;
	final static int	CMD_SUSP_RL		= 47;
	final static int	CMD_SUSP_R		= 48;
	final static int	CMD_SUSP_RR		= 49;

	final static int	CMD_SUSP_MODE	= 50;

	Navigator		nav;
	Marker			mPlayer, mPlayer2;

	Player			player, player2;
	GameRef			cursor;

	//message display
	Text			messages;
	Text			infoline;
	int				msgtimers;
	int				osdEnabled;

	//cam
	GameRef			cam_external;	//external camera instance
	GameRef			cam, MirCam;	//currently active camera instance
	//GameRef		renderer;   // Current using camera
	GameRef			cameraTarget;	//car of the player (usually) or another bot
	GameRef			cameraTarget2;	//another car or object (police, racer, etc.)
	int				cameraMode;
	int				cameraNum;		//ha pl. tobb camera van az adott cameraMode-hoz (pl. belso, de lehet TV, ext, stb. is)

	Vector3			lastCamPos;
	Vector3			lastCrossPos;	//for TV-camera
	
	Viewport		vport2, vp2;

	final static int CAMMODE_NONE = 0;
	final static int CAMMODE_FOLLOW = 1;
	final static int CAMMODE_TV = 2;
	final static int CAMMODE_CHASE = 3;
	final static int CAMMODE_INVCH = 4;
	final static int CAMMODE_INTERNAL = 5;
	final static int CAMMODE_FREE = 6;
	final static int CAMMODE_POINT = 7;
	// OpenSLRR (#487): a mod CameraProvider has the follow view.
	final static int CAMMODE_PROVIDER = 8;

	CameraProvider	camProvider;		// the active provider, or null
	CameraProvider	camProviderSynced;	// the selection syncCameraProvider last saw

	Mechanic mechanic;
	Painter painter;
	int		mc_active, pt_active;

	Rectangle	MirrorFrame, MapViewFrame;
	GameRef		MirCam2;
	float		Rot, Speed, TimeTextWait, camDist;
	int			IntCamNumber, MirrorStatus = 0, TimeTextStatus, GetCarStatus = 0, RepairStatus = 0, MapStatus, OsdStatus, MapZ1, MapZ2;
	Text		TimeText, TimeText2;
	final static float GPS_Min_Zoom = 2, GPS_Max_Zoom = 3;

	GameRef		Test;
	RenderRef	Test_Render;

	Vector3 rota;


	public Track()
	{
		createNativeInstance();

		player=GameLogic.player;
		player2=GameLogic.player2;
	}

	
	public void animate()
	{
		syncCameraProvider();	// OpenSLRR (#487)

		/*if(player.car)
		{
			rota = new Vector3(Math.random()*0.1-0.05, Math.random()*0.1-0.05, 0);
			player.car.chassis.setSlotPos( 18, rota, null );
			player.car.chassis.setSlotPos( 23, rota, null );
		}*/

		//setMessage( Math.sqrt(player.car.getSpeedSquare())*3.6 ); // Car Speed

		if( TimeTextStatus == 1 && TimeTextWait < System.currentTime() )
		{
			float tim = System.timeWarp(-1.0);
			if( tim == 1.0 || tim == 0.0)
			{
				TimeTextStatus = 0;
				TimeText.changeText( "" );
			}
		}

		if( nav && nav.cam )
		{
			if( player.car )
			{
				Speed = Math.sqrt(player.car.getSpeedSquare()) * 0.08;
				if (Speed > GPS_Max_Zoom)
					Speed = GPS_Max_Zoom;
			}

			Vector3 CamPos;
			Ypr CamYpr;
			Vector3 RendererPos;
			Ypr RendererOri;

			// OpenSLRR (#487): a mod camera provider has its own camera, and
			// Track's renderer is null while it is active.
			GameRef view = renderer;
			if( !view && camProvider && camProvider.camera && camProvider.camera.id() )
				view = camProvider.camera;

			if( view )
			{
				RendererPos = view.getPos();
				RendererOri = view.getOri();

				nav.gmCamera.setMatrix( new Vector3(RendererPos.x, 0.1, RendererPos.z),  new Ypr(RendererOri.y, 0.0, 0.0) );
				nav.rmCamera.setMatrix( new Vector3(RendererPos.x/100, 0.1, RendererPos.z/100),  new Ypr(RendererOri.y, 0.0, 0.0) );
			}

			if( Config.gpsMode == 0 && player.car || Config.gpsMode == 1 && player.car )// Follow car
			{	
				CamPos = player.car.getPos();
				Rot = 0;

				if(Config.gpsMode == 1)
					Rot = player.car.getOri().y;	
			}
			else
			if( Config.gpsMode == 2 && view || Config.gpsMode == 3 && view )
			{
				CamPos = RendererPos;
				Rot = 0;

				if(Config.gpsMode == 3)
					Rot = RendererOri.y;
			}
			
			else
			if( player.car )	// OpenSLRR (#487): no camera to follow, follow the car
			{
				CamPos = player.car.getPos();
				Rot = 0;
				if(Config.gpsMode == 3)
					Rot = player.car.getOri().y;
			}

			nav.updateNavigator( player.car );			
			if( CamPos )
				nav.cam.setMatrix( new Vector3(CamPos.x/100, GPS_Min_Zoom + Speed, CamPos.z/100),  new Ypr(Rot, -1.57, 0.0) );
		}

	// Car position for modders
	/*	if(player.car)
		{
			Vector3	CarVector = player.car.getPos();
			TimeText.changeText( "Pos: X: " + CarVector.x + ", Y: " + CarVector.y + ", Z: " + CarVector.z );

			Ypr CarYpr = player.car.getOri();
			TimeText2.changeText( "Car Rotation: Y: " + CarYpr.y + ", P: " + CarYpr.p + ", R: " + CarYpr.r );
		}//*/
	}

	public void Mirror( int mode )
	{
		if ( mode == 1)
		{
			if (!MirrorFrame)
				MirrorFrame = MirrorOsd.createRectangle( 0.0, -0.76, 1.04, 0.46, 2, new ResourceRef(MW_Mod:0x0023r) );
			
			if( !vp2 )
				vp2 = new Viewport( 16, 0.25, 0.02, 0.5, 0.2 );
			GameRef MirTrg = new GameRef(cameraTarget.getInfo(GII_OWNER));
			cameraTarget.command( "render " + vp2.id() +" "+ MirTrg.id() +" "+ 6);
			//renderer = cameraTarget;

			//MirCam = new Camera( map, vp2, 10, 100, 0.1, 50.0, 0.2, 5.0, 0, 0 );

		//MirCam2 = new GameRef( player.car.chassis, GameRef.RID_CAMERA, player.car.getPos().toString() +","+ player.car.getOri().toString()+", 0x01,1.5,0.0", "s_cam" );
		//cam.command( "zoom 60 5");
		//cam.queueEvent( null, GameType.EVENT_COMMAND, "render " + vp2.id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
		//cam.queueEvent( null, GameType.EVENT_COMMAND, "look "+ cars[curcar].id() +" 0.0, 0.0, 0.0");
		//cam.queueEvent( null, GameType.EVENT_COMMAND, "look "+ map.id() +" 0.0, 0.0, -1000.0");

		//MirCam2.command( "install 0 " + player.car.id()+" 0 0 0 "+"0 0 0");//pos.x+" "+pos.y+" "+pos.z );

			vp2.activate( Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET );
		}
		else
		if ( mode == 0)
		{
			if (MirrorFrame)
			{
				MirrorFrame.finalize();
				MirrorFrame=null;
			}
			if (vp2)
			{
				vp2.deactivate(); 
				vp2=null;
			}
		}
	}

//----------------------------------------------------------------------

	final static public float Check( float key )
	{
		return Math.sqrt(key)+3.75;
	}



	public void enter( GameState prev_state )
	{

		float key = Math.random()*100;
		if( MainMenu.Check(key) != Math.sqrt(key)+3.75 ) Mirans_Mod_protection_error______Please_Resintal_Mirans_Mod();

		if( prev_state instanceof RaceSetup )
		{
		}
		else
		{
//			cameraMode = CAMMODE_FOLLOW;	//reset

			enterAsyncMode_Script();
			t.setPriority( Thread.MAX_PRIORITY );

			//test:
			//GameLogic.autoSaveQuiet();
	
			//a gyerekosztalyok implementaljak a kov ket sort, az o enter() metodusukban!
			//Frontend.loadingScreen.show();
			//GfxEngine.flush();

			parentState=prev_state;

			MirrorOsd = new Osd( 1.0, 0.0, 18 );
			osd = new Osd();
			osd.menuKeysCreated = 1;
			//osd.iLevel=Osd.IL_KEYS;
			osd.alienMp = mp;

			//only for ects/intel demo
			//osd.createRectangle( 0.70, -0.80, 0.5, 0.4, -1, new ResourceRef(frontend:0x00AAr) );


			//---------------------------------time of day dependent stuff:
			addSceneElements( GameLogic.getTime() );
			//-----------------------------------------------------------------------

			msgtimers=0;
			messages=osd.createText( null, Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.4 );
			TimeText=osd.createText( null, Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.5 ); TimeText.changeColor(0xFFFFD600); //Yellow
			TimeText2=osd.createText( null, Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.4 ); TimeText2.changeColor(0xFFFFD600); //Yellow

			setEventMask( EVENT_CURSOR|EVENT_COMMAND|EVENT_HOTKEY|EVENT_TRIGGER_ON|EVENT_TRIGGER_OFF|EVENT_TIME );

			//player
			player.render = new RenderRef( map, player.driverID, "player" );
			player.controller.command( "renderinstance " + player.render.id() );

			if (player.car)
			{
				if( GameLogic.gameMode != GameLogic.GM_DEMO )
				{
					lockCar();
					player.car.command( "setsteer 0" );

					//igy ha valtoztak a config beallitasok, az is szamit neki!
					player.car.setDefaultTransmission();
					player.car.setDefaultSteeringHelp();
					player.controller.command( "controllable " + player.car.id() );
					player.car.chassis.Refresher_Start();
				}

				lastCamPos = new Vector3( 0, 3, 6);
				lastCamPos.add( player.car.getPos() );
				changeCamTarget(player.car);
				changeCamFollow();// cameraPos );
				// OpenSLRR (#487): a selected mod camera takes the follow view.
				camProviderSynced = null;
				syncCameraProvider();

				map.command( "obs_add " + player.car.id() );
			}

			if( GameLogic.gameMode == GameLogic.GM_MULTIPLAYER )
			{
				player2.render = new RenderRef( map, player2.driverID, "player2" );
				player2.controller.command( "renderinstance " + player2.render.id() );
				
				if(player2.car)
				{
					if( !posStart2 )
					{
						posStart2 = new Vector3(posStart.x+10, posStart.y, posStart.z);
						oriStart2 = new Ypr ( oriStart.y, oriStart.p, oriStart.r );
					}

					player2.car.setParent( map );
					player2.car.setMatrix( posStart2, oriStart2 );
					player2.car.command( "reset" );
					player2.car.command( "reload" );	//Fuel and NOS

					player2.car.command( "setsteer 0" );

					player2.car.setDefaultTransmission();
					player2.car.setDefaultSteeringHelp();
					player2.controller.command( "controllable " + player2.car.id() );
				}
			}

			ingameMenu=new IngameMenu( this );

			infoline=osd.createText( null, Frontend.mediumFont, Text.ALIGN_RIGHT, 0.97, 0.57);

			osd.endGroup();

			painter =  new Painter( player, osd, null, infoline, 1 );
			mechanic = new Mechanic( player, osd, null, infoline, 1 );

				osd.createHotkey( Input.RCDIK_TAB, Input.KEY|Osd.HK_STATIC, CMD_OSDONOFF, this );
				osd.createHotkey( Input.RCDIK_1, Input.KEY|Osd.HK_STATIC, CMD_SIMSPEEDDEC, this );
				osd.createHotkey( Input.RCDIK_2, Input.KEY|Osd.HK_STATIC, CMD_SIMSPEEDINC, this );
				osd.createHotkey( Input.RCDIK_3, Input.KEY|Osd.HK_STATIC, CMD_SIMPAUSE, this );
				osd.createHotkey( Input.RCDIK_4, Input.KEY|Osd.HK_STATIC, CMD_CAMROTATE, this );

			// OpenSLRR (#470): one rebindable change-view key (V) replaces the
			// F1 (cockpit), F2 (TV), F3 (follow), F4 (chase), F5 (reverse
			// chase) and F9 (free) view keys; see nextCamera.
			osd.createHotkey( Input.AXIS_LOOK_PREVNEXT, Input.VIRTUAL|Osd.HK_STATIC, CMD_NEXTCAM, this );
		//cheat keys:
			if( GameLogic.cheats )
			{
				osd.createHotkey( Input.RCDIK_F6, Input.KEY|Osd.HK_STATIC, CMD_REPAIR, this );
				osd.createHotkey( Input.RCDIK_F10, Input.KEY|Osd.HK_STATIC, CMD_QUICKREPAIR, this );
				osd.createHotkey( Input.RCDIK_F11, Input.KEY|Osd.HK_STATIC, CMD_CHANGECAMTARGET, this );
			}

			osd.createHotkey( Input.AXIS_MENU, Input.VIRTUAL|Osd.HK_STATIC, CMD_INGAMEMENU, this );
			osd.createHotkey( Input.AXIS_CURSOR_BUTTON2, Input.VIRTUAL|Osd.HK_STATIC, CMD_CAMMOVE, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );

			osd.createHotkey( Input.RCDIK_0, Input.KEY|Osd.HK_STATIC, CMD_MIRROR, this );
			
			osd.createHotkey( Input.AXIS_MAP, Input.VIRTUAL|Osd.HK_STATIC, CMD_MAP, this );
			osd.createHotkey( Input.AXIS_HELP, Input.VIRTUAL|Osd.HK_STATIC, CMD_CRUISECONTROL, this );
			osd.createHotkey( Input.AXIS_LOOK_REAR, Input.VIRTUAL|Osd.HK_STATIC, CMD_LOOKBACK, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );			

		// Pneumatic suspension
			osd.createHotkey( Input.AXIS_SPECIAL_1, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_FL, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_2, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_F, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_3, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_FR, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_4, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_LS, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_5, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_ALL, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_6, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_RS, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_7, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_RL, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_SPECIAL_8, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_R, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_FIRE_PRI, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_RR, Event.F_KEY_PRESS|Event.F_KEY_RELEASE );
			osd.createHotkey( Input.AXIS_FIRE_SEC, Input.VIRTUAL|Osd.HK_STATIC, CMD_SUSP_MODE, this );

			MirrorOsd.show();

			osd.show();	//kulonfele messageknek, hotkeyeknek kell
			osdEnabled = 1;
			enableOsd( osdEnabled );	//player osd, navigator

			if( nav )
			{
				mPlayer = nav.addMarker( player );

				if( GameLogic.gameMode == GameLogic.GM_MULTIPLAYER )
					mPlayer2 = nav.addMarker( GameLogic.player2 );
			}

			
			Sound.changeMusicSet( Sound.MUSIC_SET_DRIVING );

			// special request: reset mouse and set sensitivity
			Input.getAxis (1, -1 - (Config.mouseSensitivity * 100.0f));

			addTimer( 1, 2 );	//one sec tick starter

//start automatic camerachanges
			if (1)
			if( GameLogic.gameMode == GameLogic.GM_DEMO )
				addTimer (5, 4 );
		}

		new ResourceRef( particles:0x0106r ).cache(); //def spark
		new ResourceRef( particles:0x0002r ).cache(); //def smoke
		new ResourceRef( particles:0x000Ar ).cache(); //def skidmark

		new SfxRef( sound:0x000Br ).cache(); //def collision
		new SfxRef( sound:0x0006r ).cache(); //def skid
		new SfxRef( sound:0x0015r ).cache(); //def horn
		new SfxRef( sound:0x000Ar ).cache(); //def ignition
		new SfxRef( sound:0x0017r ).cache(); //def gear up
		new SfxRef( sound:0x0018r ).cache(); //def gear down
		new SfxRef( sound:0x001Ar ).precache(); //def air
		new SfxRef( sound:0x000Dr ).precache(); //tyre out

		if( GameLogic.gameMode == GameLogic.GM_QUICKRACE && !(prev_state instanceof RaceSetup) )
		{
		}
		else
		{
			Frontend.loadingScreen.display();	//waits! -------------------------------------
		}

		player.controller.reset();
		player.controller.activateState( ControlSet.DRIVERSET );

		player2.controller.reset();
		player2.controller.activateState( ControlSet.DRIVERSET );

		enableAnimateHook();
		
		Mirror(MirrorStatus);

		//Test = new GameRef( map, MW_Mod:0x0076r, "0,5,0"+","+"0,0,0," + 1, "nightracer1" );
		//Test = new GameRef( map, parts.wings:0x0028r, "0,5,0"+","+"0,0,0," + 1, "Wing" );
		//Test = new GameRef( map, cars.racers.addon:0x00000015r, "0,1,0"+","+"0,0,0," + 1, "Wing" );

		//Test_Render = new RenderRef( map, maps.skydome:0x0013r, "egbolt-dawn_02" );
		Test_Render = new RenderRef( map, MW_Mod:0x0079r, "egbolt-dawn_02" );
	}

	public void exit( GameState next_state )
	{
		disableAnimateHook();

		if( next_state instanceof RaceSetup )
		{
		}
		else
		{	
			removeAllTimers();
			trigger.removeAllElements();

			changeCamNone();

			if (player.car)
			{
				map.command( "obs_rem " + player.car.id() );

				posStart = player.car.getPos();
				oriStart = player.car.getOri();
				player.controller.command( "leave " + player.car.id() );
				player.car.chassis.Refresher_Stop();
			}

			player.render.destroy();
			releaseCar();

			if( killCar || GetCarStatus == 1 && GameLogic.gameMode == GameLogic.GM_CARREER )
			{
				player.car.destroy();
				player.car=null;
				killCar = 0;
			}

			if (GameLogic.gameMode==GameLogic.GM_MULTIPLAYER && player2.car)
			{
				map.command( "obs_rem " + player2.car.id() );

				posStart2 = player2.car.getPos();
				oriStart2 = player2.car.getOri();
				player2.controller.command( "leave " + player2.car.id() );

				player2.render.destroy();

				player2.car.command( "start" );
				player2.car.command( "reset" );
				player2.car.setParent( player2 );
				player2.car.setMatrix( null, null );
			}

			mechanic.hide();
			painter.hide();

			//kill sky and sun
			remSceneElements();

			map.setWater(new Vector3(0.0,-1000000.0,-1500.0), new Vector3(0.0,1.0,0.0), 300.0, 50.0);
			map.delTraffic();
			map.setPedestrianDensity( 0.0 );
			map.unload();
			
			osdEnabled = 0;
			enableOsd( osdEnabled );

			if( nav )
			{
				//nav.hide();
				nav.remMarker( mPlayer );	mPlayer=null;

				if(GameLogic.gameMode==GameLogic.GM_MULTIPLAYER)
					nav.remMarker( mPlayer2 );	
				mPlayer2=null;
			}

			if (MirrorFrame)
				MirrorFrame.finalize();
			Mirror(0);

			osd.hide();
			osd=null;

			clearEventMask( EVENT_ANY );

			leaveAsyncMode_Script();

			GfxEngine.flush();

		}

		//allitsa vissza az 'idoszamitast' ha pl verseny vegen a lassitott mod kozben lepteti ki!
		System.timeWarp(1.0);


		player.controller.reset();
		player.controller.activateState( ControlSet.MENUSET );
	}

	//call this after camera settings have been changed
	public void resetCamera()
	{
		if( cameraMode == CAMMODE_TV || cameraMode == CAMMODE_FOLLOW )
		{
			cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
			renderer = cam;

			if(MB_Animator) MB_Animator.changeCam(renderer);
		}
//		else
//			System.log( "cannot reset unsupported cam type" );
	}


	public void changeCamTV( )//Vector3 destPos )
	{
		if ( !cameraTarget ) return;
		if ( !cameraTarget.id() ) return;
		Vector3	destPos = cameraTarget.getPos();
		if ( !destPos ) return;
		Vector3 actdestPos = new Vector3(destPos);
        Vector3 vel = cameraTarget.getVel();
		if (vel)
		{
			vel.mul(3.0f);
			destPos.add(vel);
		}

		if( cameraMode == CAMMODE_INTERNAL && !cameraNum )
			player.car.command( "filter 3 0" );	//running geart kifilterezzuk

		Vector3 camPos;

		if (cameraMode == CAMMODE_FREE)
		{
			player.controller.command( "leave " + cam.id() );
			player.controller.command( "controllable " + player.car.id() );
			player.controller.activateState( 5, 0 );

			lastCrossPos = map.getNearestCross( destPos, 0 );	//ToDo: getnearestCamera
			if (cam)
				camPos = cam.getPos();
			else
				camPos = destPos;
		} else
		{
			camPos = map.getNearestCross( destPos, 0 );	//ToDo: getnearestCamera
			if (camPos)
			{
				if (cameraMode == CAMMODE_TV)
				if (lastCrossPos)
				if (camPos.distance( lastCrossPos ) < 0.1)
				{
					//ha esetleg valtozott a cameratarget...
					ResourceRef bone = new ResourceRef(	cameraTarget.getInfo(GameType.GII_BONE) );
					cam.command( "look " + bone.id() + " 0,0,-1" );

					return;
				}
				lastCrossPos = new Vector3(camPos);
				camPos.y += 0.2 + 3.0*Math.random();
			} else
			{
				if (cam)
					camPos = cam.getPos();
				else
					camPos = destPos;
				if (!camPos)
					camPos = lastCamPos;
			}

			if (cameraMode == CAMMODE_TV)
			{
				if (lastCamPos)
				{
					if (camPos.distance( lastCamPos ) < 0.1)
						return;
					if (lastCamPos.distance(actdestPos) < camPos.distance(actdestPos))
						return;	//jobb a mostani
				}
			} else
				addTimer( 3,3 );	//start changing cameras every 3 seconds
		}

		changeCamNone();
		if (!camPos)	return;

		Vector3 pTemp = new Vector3(actdestPos);
		pTemp.sub( camPos );
		Ypr yTemp = new Ypr( pTemp );

//		cam = new GameRef( map, GameRef.RID_CAMERA, camPos.toString() + "," + yTemp.toString() + ",0x06", "track tv cam" );
		cam = new GameRef( map, GameRef.RID_CAMERA, camPos.toString() + "," + yTemp.toString() + ",0x02", "track tv cam" );
		cam.command( "autozoom 1 0 " + (0.2+Math.random()*0.8) + " " + (Math.random()*0.2) );
		cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
		renderer = cam;
		if(MB_Animator) MB_Animator.changeCam(renderer);
		cam.command( "torque 0.1" );
		cam.command( "roll " + (Math.random()-0.5) );

		lastCamPos = new Vector3(camPos);
		cameraMode = CAMMODE_TV;

//		if (cameraTarget)
//		{
//			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
//			con.command( "viewport 0");// + osd.getViewport().id() );
			ResourceRef bone = new ResourceRef(	cameraTarget.getInfo(GameType.GII_BONE) );
			cam.command( "look " + bone.id() + " 0,0,-1" );
//			if ( osdEnabled )
//				cameraTarget.command( "osd "+ osd.id() +" "+ con.id());	//enable osd
//		} else
//			cam.command( "look " + map.id() + destPos.toString() );
	}

	public void changeCamInternal( )//Vector3 pos )
	{
		if (!cameraTarget)	return;
		if (!cameraTarget.id())	return;

		if (cameraMode == CAMMODE_FREE)
		{
			player.controller.command( "leave " + cam.id() );
			player.controller.command( "controllable " + player.car.id() );
			player.controller.activateState( 5, 0 );
		}

//		changeCamNone();

		int cameras = cameraTarget.getInfo( GameType.GII_RENDER );
		if (cameras == 0)	return;

		providerOff();	// OpenSLRR (#487)

		if (cameraMode == CAMMODE_INTERNAL)
		{//change to _next_ int.cam.
			if (++cameraNum >= cameras)
				cameraNum = 0;

			if(cameras>2 && cameraNum >= cameras-2) // Skip mirror and look back cameras
				cameraNum = 0;

			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			cameraTarget.command( "render " + osd.getViewport().id() +" "+ con.id() +" "+ cameraNum);
			renderer = cameraTarget;
			if(MB_Animator) MB_Animator.changeCam(renderer);
		} else
		{
			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			if (cam)
			{
				lastCamPos = cam.getPos();
				cam.command( "hide " + osd.getViewport().id());
				if (cam != cam_external)
					cam.destroy();	//TV
			}

			if (cameraNum < 0)
				cameraNum = (-cameraNum)%cameras;
			else
				cameraNum = 0;

			cameraTarget.command( "render " + osd.getViewport().id() +" "+ con.id() +" "+ cameraNum);
			renderer = cameraTarget;
			if(MB_Animator) MB_Animator.changeCam(renderer);
//			if ( osdEnabled )
//				cameraTarget.command( "osd 0 " + con.id());	//disable osd
		}
		cameraMode = CAMMODE_INTERNAL;

		if( !cameraNum )
			player.car.command( "filter 3 2" );	//running geart kifilterezzuk
		else
			player.car.command( "filter 3 0" );	//running geart kifilterezzuk

	}

	public void changeCamInternal( int num )
	{
		if (cameraMode == CAMMODE_INTERNAL)
		{
			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			cameraTarget.command( "render " + osd.getViewport().id() +" "+ con.id() +" "+ num);
			renderer = cameraTarget;
			if(MB_Animator) MB_Animator.changeCam(renderer);
		}
		else
		{
			cameraNum = -num;
			changeCamInternal();
		}
	}

	void makeCamExternal()	//ennek ki kellene kapcsolnia a renderert, ha az nem az ext. cam.
	{
		Vector3	pos;

		providerOff();	// OpenSLRR (#487)

		if (cameraMode == CAMMODE_FREE)
		{
			player.controller.command( "leave " + cam.id() );
			player.controller.command( "controllable " + player.car.id() );
			player.controller.activateState( 5, 0 );
		}

		if( cameraMode == CAMMODE_INTERNAL && !cameraNum )
			player.car.command( "filter 3 0" );	//running geart kifilterezzuk


		if (cam_external)
			if (!cam_external.id())
				cam_external = null;	//drop

		if( cam_external )
		{
			if (renderer)
			if (renderer != cam_external)
			{
				renderer.command( "hide " + osd.getViewport().id());
				renderer = null;
			}
/*			if (cameraMode == CAMMODE_INTERNAL)
			{
//				GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
				cameraTarget.command( "hide " + osd.getViewport().id());// +" "+ con.id());
//				if ( osdEnabled )
//					cameraTarget.command( "osd "+ osd.id() +" "+ con.id());	//enable osd
			}
*/			if (cam && cam != cam_external)
			{
				cam.destroy();	//TV
				cam = null;
			}

			cam = cam_external;
		}

		if (!cam_external)
		{
			changeCamNone();
			
			if (lastCamPos)
				pos = lastCamPos;
			else
				pos = new Vector3(0,-10000,0);	//patch
				
			Vector3 pTemp = cameraTarget.getPos();
			if (!pTemp)
			{
//				System.log("Missing cameratarget! Ext.camera not created!");
				return;
			}
			Ypr yTemp;
			pTemp.sub( pos );

			//ha tul mesze ment (tvben), lerakjuk moge
			if( pTemp.length() > 70.0 )
			{
				pos = cameraTarget.getPos();
				if (!pos)	return;
				Vector3 displ = new Vector3( pTemp );
				displ.normalize();
				displ.mul( 5.0 );

				pTemp = new Vector3( pos );
				pos.sub( displ );
				pos.y+=2.0;

				pTemp.sub( pos );

				yTemp = new Ypr( pTemp );
			}
			else
			{
				yTemp = new Ypr( pTemp );
			}

			cam = new GameRef( map, GameRef.RID_CAMERA, pos.toString() + "," + yTemp.toString() + ",0x02, 1.0,0, 0.01", "external track cam" );
			cam_external = cam;

			lastCamPos = new Vector3(pos);
		} 
	}

	public void changeCamFollow( )//Vector3 pos )
	{
		makeCamExternal();

		cam.command( "dist 2.0 10.0");
		cam.command( "smooth 0.5 0.5");
		cam.command( "force 1.6 0.5 -0.7" );	//defaults are in config.java
		cam.command( "torque 0.1" );
		cam.command( "roll 0" );
		cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
		renderer = cam;
		if(MB_Animator) MB_Animator.changeCam(renderer);

		GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
		con.command( "viewport " + osd.getViewport().id() );
		con.command( "camera " + cam.id() );

		cameraMode = CAMMODE_FOLLOW;
	}

	public void changeCamPoint( Vector3 pos )
	{
		makeCamExternal();

		GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
		con.command( "viewport 0");// + osd.getViewport().id() );
		ResourceRef bone = new ResourceRef(	cameraTarget.getInfo(GameType.GII_BONE) );

		Vector3 pTemp = cameraTarget.getPos();
		if (!pTemp)	return;
		pTemp.sub( pos );
		Ypr yTemp = new Ypr( pTemp );

		cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
		renderer = cam;
		if(MB_Animator) MB_Animator.changeCam(renderer);
		cam.command( "move " + bone.id() + " 0,1,0 0.1 0,-0.2,-3" );
		cam.command( "look " + map.id() + " "+pos.x+","+pos.y+","+pos.z );
		cam.command( "roll " + (Math.random()-0.5) );
		cameraMode = CAMMODE_POINT;
	}

	public void changeCamChase()
	{
		if (!cameraTarget2)
		{
			changeCamFollow();
		} else
		if (!cameraTarget2.id())
		{
			changeCamFollow();
		} else
		{
			makeCamExternal();

			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			con.command( "viewport 0");// + osd.getViewport().id() );
			ResourceRef bone = new ResourceRef(	cameraTarget.getInfo(GameType.GII_BONE) );
			cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
			renderer = cam;
			if(MB_Animator) MB_Animator.changeCam(renderer);
//			cam.command( "move " + bone.id() + " 0,1,0 0.1 0,-0.2,-8" );
			float roll = Math.random()-0.5;
			cam.command( "move " + bone.id() + " 0,"+(Math.random())+",0 0.01 " + roll*-3.0 + ",-0.2," + (-3-5*Math.random()) );
			cam.command( "look " + cameraTarget2.id() + " 0,0,-1 " + roll*0.6 + ",0,-1" );
			cam.command( "roll " + roll );
			cam.command( "zoom " + (20.0+70.0*Math.random()) +" 5" );
		}
		cameraMode = CAMMODE_CHASE;
	}

	public void changeCamInvChase()
	{
		if (!cameraTarget2)
		{
			changeCamFollow();
		} else
		if (!cameraTarget2.id())
		{
			changeCamFollow();
		} else
		{
			makeCamExternal();

			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			con.command( "viewport 0");// + osd.getViewport().id() );
			ResourceRef bone = new ResourceRef(	cameraTarget2.getInfo(GameType.GII_BONE) );
			cam.command( "render " + osd.getViewport().id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
			renderer = cam;
			if(MB_Animator) MB_Animator.changeCam(renderer);
//			cam.command( "move " + bone.id() + " 0,1,0 0.1 0,-0.2,-8" );
//			cam.command( "look " + cameraTarget.id() + " 0,0,-1" );
			float roll = Math.random()-0.5;
			cam.command( "move " + bone.id() + " 0,"+(Math.random())+",0 0.01 " + roll*-3.0 + ",-0.2," + (-3-5*Math.random()) );
			cam.command( "look " + cameraTarget.id() + " 0,0,-1 " + roll*0.6 + ",0,-1" );
			cam.command( "roll " + roll );
			cam.command( "zoom " + (20.0+70.0*Math.random()) +" 5" );
		}
		cameraMode = CAMMODE_INVCH;
	}

	// OpenSLRR (#470): the change-view key cycles the driving views:
	// follow, chase, then each cockpit camera (not the mirror and look-back
	// ones), back to follow. With a mod camera selected (#487), the follow
	// view is the provider's, and the key cycles the provider's views.
	public void nextCamera()
	{
		if (!cameraTarget)	return;
		if (!cameraTarget.id())	return;
		if (closeUps)	return;	// OpenSLRR (#487): repair, mechanic or paint view

		CameraProvider p = selectedCameraProvider();
		if( p && (cameraMode == CAMMODE_FOLLOW || cameraMode == CAMMODE_PROVIDER) )
		{
			int v = Cameras.view( p );
			if( cameraMode == CAMMODE_PROVIDER && camProvider == p )
			{
				v++;
				if( v >= p.views() )
					v = 0;
				Cameras.setView( p, v );
				p.setView( v );
			}
			else
				providerOn( p, v );
			if( cameraMode == CAMMODE_PROVIDER )
				setMessage( "Camera: " + p.viewName( v ) );
			return;
		}

		if( cameraMode == CAMMODE_FOLLOW )
		{
			changeCamChase();
			setMessage( "Camera: chase" );
		}
		else
		if( cameraMode == CAMMODE_CHASE )
		{
			cameraNum = 0;
			changeCamInternal();
			if( cameraMode == CAMMODE_INTERNAL )
				setMessage( "Camera: cockpit" );
			else
			{
				// A car without cockpit cameras goes back to follow.
				changeCamFollow();
				setMessage( "Camera: follow" );
			}
		}
		else
		if( cameraMode == CAMMODE_INTERNAL )
		{
			// Look back temporarily renders another camera; release restores cameraNum.
			float rear = player.controller.user_GetAxisVal( Input.AXIS_LOOK_REAR );
			if( rear > 0.5 || rear < -0.5 ) return;

			// changeCamInternal's next camera, skipping the mirror and
			// look-back cameras; after the last one, back to follow.
			int cameras = cameraTarget.getInfo( GameType.GII_RENDER );
			int next = cameraNum + 1;
			if( next >= cameras || (cameras > 2 && next >= cameras - 2) )
			{
				changeCamFollow();
				setMessage( "Camera: follow" );
			}
			else
			{
				changeCamInternal();
				setMessage( "Camera: cockpit " + (cameraNum + 1) );
			}
		}
		else
		{
			if (cameraMode != CAMMODE_FOLLOW)
				changeCamFollow();
			setMessage( "Camera: follow" );
		}
	}

	// OpenSLRR (#487): mod camera providers (java.openslrr.CameraProvider).
	CameraProvider selectedCameraProvider()
	{
		return Cameras.get( Cameras.selected() );
	}

	// Hands the follow view to provider p in view v. Track's follow camera
	// stays alive, hidden and bound to the car, because subclasses and race
	// scripts use the cam field directly.
	void providerOn( CameraProvider p, int v )
	{
		if( !p || !cameraTarget || !cameraTarget.id() || closeUps )
			return;
		if( cameraMode != CAMMODE_FOLLOW )
			changeCamFollow();	// also deactivates a provider
		if( cameraMode != CAMMODE_FOLLOW )
			return;
		if( renderer )
		{
			renderer.command( "hide " + osd.getViewport().id() );
			renderer = null;
		}
		camProvider = p;
		cameraMode = CAMMODE_PROVIDER;
		// Activation restores the saved view; only a view change writes it.
		p.world = map;
		p.activate( cameraTarget, osd.getViewport(), v );
	}

	// Every other camera change starts here: the provider destroys its camera.
	void providerOff()
	{
		if( camProvider )
		{
			CameraProvider p = camProvider;
			camProvider = null;
			p.deactivate();
		}
		if( cameraMode == CAMMODE_PROVIDER )
			cameraMode = CAMMODE_NONE;
	}

	// The repair, mechanic and paint views drive Track's own cam for their
	// close-up. While one is open (closeUps > 0) V and look back are ignored
	// and provider activation and selection changes wait. Opening the first
	// one with a provider active gets the stock follow camera back; closing
	// the last one applies the current selection to the follow view.
	int	closeUps;
	int	closeUpProvider;	// a provider showed when the first close-up opened
	void closeUpBegin()
	{
		if( !closeUps++ && cameraMode == CAMMODE_PROVIDER )
		{
			changeCamFollow();	// deactivates the provider, shows cam
			closeUpProvider = 1;
		}
	}
	void closeUpEnd()
	{
		if( !closeUps || --closeUps )
			return;
		int resume = closeUpProvider;
		closeUpProvider = 0;
		if( resume && cameraMode == CAMMODE_FOLLOW )
		{
			CameraProvider p = selectedCameraProvider();
			camProviderSynced = p;
			if( p )
				providerOn( p, Cameras.view( p ) );
		}
		// Otherwise syncCameraProvider applies a selection made meanwhile.
	}

	// Back to the provider's view after a temporary camera (City's night
	// race watch), or to the stock follow camera when none is selected.
	void providerRestore()
	{
		changeCamFollow();
		CameraProvider p = selectedCameraProvider();
		camProviderSynced = p;
		if( p )
			providerOn( p, Cameras.view( p ) );
	}

	// A selection made while driving (the options) applies to the follow view.
	void syncCameraProvider()
	{
		if( closeUps )
			return;	// applied when the close-up closes
		CameraProvider p = selectedCameraProvider();
		if( p == camProviderSynced )
			return;
		camProviderSynced = p;
		if( cameraMode == CAMMODE_FOLLOW || cameraMode == CAMMODE_PROVIDER )
		{
			if( p )
			{
				if( p != camProvider )
					providerOn( p, Cameras.view( p ) );
			}
			else
				changeCamFollow();
		}
	}

	public void changeCamFree()
	{
//'freestyle' camera for debug
		makeCamExternal();
		player.controller.command( "leave " + player.car.id() );
		player.controller.command( "controllable " + cam.id() );
		player.controller.activateState( 5, 1 );
		cameraMode = CAMMODE_FREE;
	}

	public void enablePicInPic()
	{
//debug test:
//		vport2 = new Viewport( 12, 0.1, 0.1, 0.3, 0.3 );
		vport2 = new Viewport( 12, 0.99, 0.01, 0.3, 4.0/3.0, -1.0, 0.0 );
		vport2.activate( Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET );
		GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
		cameraTarget.command( "render " + vport2.id() +" "+ con.id() +" "+ cameraNum);
//		renderer = cameraTarget;
	}

	public void changeCamNone()
	{
		providerOff();	// OpenSLRR (#487)

		if (renderer)
		{
			renderer.command( "hide " + osd.getViewport().id());
			renderer = null;
		}

		if( cameraMode == CAMMODE_INTERNAL && !cameraNum )
			player.car.command( "filter 3 0" );	//running geart kifilterezzuk

/*		if (cameraMode == CAMMODE_INTERNAL)
		{
//			GameRef con = new GameRef(cameraTarget.getInfo(GII_OWNER));
			cameraTarget.command( "hide " + osd.getViewport().id());// +" "+ con.id());
		}
*/		if( cam )
		{
			cam.destroy();
			if (cam_external == cam)
				cam_external = null;
			if (renderer == cam)
				renderer = null;
			cam = null;
		}
	}

	public void refreshCamera()
	{
		if (cameraMode == CAMMODE_FOLLOW)
		{
			changeCamNone();
			changeCamFollow();
		}
		else
		if (cameraMode == CAMMODE_INTERNAL)
			changeCamInternal();
		else
		if (cameraMode == CAMMODE_TV)
			changeCamTV();
		else
		if (cameraMode == CAMMODE_CHASE)
			changeCamChase();
		else
		if (cameraMode == CAMMODE_INVCH)
			changeCamInvChase();
		else
		if (cameraMode == CAMMODE_PROVIDER && camProvider)
			providerOn( camProvider, Cameras.view( camProvider ) );	// OpenSLRR (#487): new target
	}

	public void changeCamTarget( GameRef obj )
	{
		if (cameraTarget == obj)
			return;
//		if (cameraMode == CAMMODE_INTERNAL)
		if (renderer && cameraTarget)
		if (renderer.id() == cameraTarget.id())
		{
			renderer.command( "hide " + osd.getViewport().id());
			renderer = null;
		}
		cameraTarget = obj;
		refreshCamera();
	}

	public void changeCamTarget2( GameRef obj )
	{
		if (cameraTarget2 == obj)
			return;
		if (renderer && cameraTarget2)
		if (renderer.id() == cameraTarget2.id())
		{
			renderer.command( "hide " + osd.getViewport().id());
			renderer = null;
		}
		cameraTarget2 = obj;
		if ((cameraMode == CAMMODE_CHASE)
		 || (cameraMode == CAMMODE_INVCH))
			refreshCamera();
	}

	public Trigger addTrigger( Trigger t, Vector3 pos, RenderRef marker, String handler )
	{
		trigger.addElement(t);
		addNotification( t.trigger, EVENT_TRIGGER_ON, EVENT_SAME, null, handler );
		addNotification( t.trigger, EVENT_TRIGGER_OFF, EVENT_SAME, null, handler );
		
		if( marker )
		{
			t.marker = nav.addMarker( marker, pos, 0 );
			t.nav = nav;
		}

		return t;
	}


	public Trigger addTrigger( Vector3 pos, GameRef type, RenderRef marker, String handler, float r, String alias )
	{
		return addTrigger( new Trigger( map, null, pos, r, alias ), pos, marker, handler );
	}

	public void removeTrigger( Trigger t )
	{
		if( t )
		{
			remNotification( t.trigger, EVENT_TRIGGER_ON|EVENT_TRIGGER_OFF );
			trigger.removeElement(t);
			t.finalize();
		}
	}

	//el/visszaveszi a kocsit a playertol
	public void lockCar()
	{
		if (player.car)
		{
			player.car.setParent( map );
			player.car.setMatrix( posStart, oriStart );
			player.car.command( "reset" );
			player.car.command( "reload" );	//Fuel and NOS
		}
	}

	public void releaseCar()
	{
		if (player.car)
		{
			player.car.setParent( player );
			player.car.command( "reset" );
		}
	}


	public void setMessage( String str )
	{
		messages.changeText( str );
		addTimer( 2, 0 );
		++msgtimers;
	}

	public void handleEvent( GameRef obj_ref, int event, int param )
	{
		//intet kellene visszadnia, jelezve, hogy feldolgozta-e az uzenetet!
		if( event == EVENT_TIME )
		{
			if( param == 0 )
			{
				if( !--msgtimers )
					messages.changeText( "" );	
			}
			else
			if( param == 2 )
			{
				addTimer( 1, 2 );	//one sec tick

				if( GameLogic.timeout )
				{
					if( !--GameLogic.timeout )
						GameLogic.changeActiveSection( parentState );
				}
			}
			else
			if( param == 3 )
			{
				if( cameraMode == CAMMODE_TV )
				{
//					if (player.car)
						changeCamTV();
					addTimer( 3, 3 ); //3mp-kent keresunk neki uj post
				}
			}
			else
			if( param == 4 )
			{
				if ( GameLogic.gameMode == GameLogic.GM_DEMO )
				{//random camera changes
					int	changed = 0;
					if (Math.random() > 0.3f)
					{
						int cam = 2.0*Math.random();
						cameraMode = 2+cam%3;
						changed++;
					} else
					{
						int	cam = 3.0*Math.random();
						changeCamInternal(1+cam%3);
					}

					if (Math.random() > 0.7f)
						if (cameraTarget2)
						{
							GameRef			t2 = cameraTarget2;
							
							if (renderer && cameraTarget)
							if (renderer.id() == cameraTarget.id())
							{
								renderer.command( "hide " + osd.getViewport().id());
								renderer = null;
							}
							cameraTarget2 = cameraTarget;
							cameraTarget = t2;
							changed++;
						}
					if (changed)
					{
						refreshCamera();
						addTimer (5, 4 );
					} else
					{
						addTimer (2, 4 );
					}
				}
			}
		}
	}


	public void handleEvent( GameRef obj_ref, int event, String param )
	{
		int	tok = -1;

		if( event == EVENT_CURSOR )
		{
			int	ec = param.token( ++tok ).intValue();

			int	cursor_id = param.token( ++tok ).intValue();
			if (ec == GameType.EC_RCLICK)
			{
				GameRef dest = new GameRef(param.token( ++tok ).intValue());
				int cat = dest.getInfo(GameType.GII_CATEGORY);
				if( cat == GIR_CAT_PART || cat == GIR_CAT_VEHICLE )
//					cam.command( "look " + dest.id() + " 0,0,0" );
					cam.command( "look " + dest.id() + " " + param.token( ++tok ) + "," + param.token( ++tok ) + "," + param.token( ++tok ) );
			} 
		}
	}


	public void handleMessage( Message m )
	{
		if( m.type == Message.MT_EVENT )
		{
			int	cmd=m.cmd;


			if( cmd == CMD_LOOKBACK )
			{
				// OpenSLRR (#487): the active mod camera handles look back;
				// the repair, mechanic and paint close-ups ignore it.
				if( closeUps )
				{
				}
				else
				if( cameraMode == CAMMODE_PROVIDER )
				{
					if( camProvider )
					{
						int pressed = 0;
						if( ((Event)m).flags )
							pressed = 1;
						camProvider.lookBack( pressed );
					}
				}
				else
				if(cameraMode == CAMMODE_FOLLOW)
				{
					if( ((Event)m).flags )	//press
					{
						//makeCamExternal();

						Vector3 v = new Vector3();
						v.diff( cameraTarget.getPos(), cam.getPos() );
						camDist = v.length();

						Vector3 Dist = new Vector3( 0.0, 1.5, -camDist );// distance
						Vector3 b = cameraTarget.getPos();
						float CarOri = cameraTarget.getOri().y;

						Dist.rotate( new Ypr( CarOri, 0.0, 0.0 ) ); // rotate Dist Vector
						Dist.sub( new Vector3( -b.x, -b.y, -b.z) );	// Set Pivot of rotate

						cam.command( "force 2.0 1.0 -1.0" );	//defaults are in config.java
						cam.command( "simulate 0" );

						ResourceRef bone = new ResourceRef(	cameraTarget.getInfo(GameType.GII_BONE) );
						cam.command( "move " + bone.id() + " 0,1.5,-" + camDist + " 0.001 0,-0.2, 0" );
						cam.command( "look " + bone.id() + " 0,1.3,0" );

						Dist.y += 1;
						cam.setMatrix( Dist, new Ypr(CarOri+3.14, 0, 0) );
					}
					else
					{
						changeCamFollow();

						Vector3 Dist = new Vector3( 0.0, 2.0, 0.9*camDist );// distance
						Vector3 b = cameraTarget.getPos();
						float CarOri = cameraTarget.getOri().y;

						Dist.rotate( new Ypr( CarOri, 0.0, 0.0 ) ); // rotate Dist Vector
						Dist.sub( new Vector3( -b.x, -b.y, -b.z) );	// Set Pivot of rotate

						cam.setMatrix( Dist, new Ypr(CarOri, 0, 0) );
					}
				}
				else
				if(cameraMode == CAMMODE_INTERNAL)
				{
					if( ((Event)m).flags )	//press
					{
						IntCamNumber = cameraNum; //Member current camera number

						changeCamInternal(5);
					}
					else //Relase
					{
						changeCamInternal(IntCamNumber); //Back to old camera
					}
				}
			}
			else
			if( cmd == CMD_MIRROR )
			{
				MirrorStatus += 1;
				if (MirrorStatus >= 2) MirrorStatus = 0;

				Mirror(MirrorStatus);
			}
			else
			if( cmd == CMD_MAP )
			{
				if( nav )
				{
					if( MapStatus == 1 ) // ON
					{
						osd.vp.resize( 0, 0, 1, 1 );
						nav.hide();
						enableOsd(OsdStatus);
						MapViewFrame.finalize();
						GPS_Min_Zoom = MapZ1;
						GPS_Max_Zoom = MapZ2;

						MapStatus = 0;
					}
					else // OFF
					{
						OsdStatus = osdEnabled;
						enableOsd(0);
						MapViewFrame = MirrorOsd.createRectangle( 0.63, 0.63, 0.72, 0.72, -1, new ResourceRef( MW_Mod:0x0054r ) );
						osd.vp.resize( 0.64, 0.64, 0.35, 0.35 );
						nav.ViewMap();
						MapZ1 = GPS_Min_Zoom;
						MapZ2 = GPS_Max_Zoom;

						GPS_Min_Zoom += 3;
						GPS_Max_Zoom += 4.5;

						MapStatus = 1;
					}
				}
			}
			else
			if( cmd >= CMD_SUSP_FL && cmd <= CMD_SUSP_MODE ) // All suspension buttons
			{
				if( player.car )
				{
					if( player.car.chassis )
					{
						//setMessage( cmd - 40 ); \\Text for bebuging

						if( ((Event)m).flags )	//press
							player.car.chassis.Hsuspension( 1, cmd - 40 );
						else	//release
							player.car.chassis.Hsuspension( 0, cmd - 40 );
					}
				}
			}
			else
			if( cmd == CMD_CRUISECONTROL )
			{
				int newCruise = 1-player.car.getCruiseControl();
				player.car.setCruiseControl( newCruise );
				if( newCruise )
				{
					setMessage( "Cruise control on" );


				}
				else
				{
					setMessage( "Cruise control off" );

				}
			}
			else
			if( cmd == CMD_CAMMOVE )
			{
				if( ((Event)m).flags )
				{	//press
					player.controller.user_Add( Input.AXIS_LOOK_UPDOWN,	ControlSet.MOUSE, 1,	-1.0f, 1.0f, -1.0f, 1.0f);
					player.controller.user_Add( Input.AXIS_LOOK_LEFTRIGHT,	ControlSet.MOUSE, 0,	-1.0f, 1.0f, -1.0f, 1.0f);
				}
				else
				{	//release
					player.controller.user_Del( Input.AXIS_LOOK_UPDOWN,	ControlSet.MOUSE, 1 );
					player.controller.user_Del( Input.AXIS_LOOK_LEFTRIGHT,	ControlSet.MOUSE, 0 );
				}
			} else

			if( cmd == CMD_NEXTCAM )
			{
				nextCamera();
			} else
			if( cmd == CMD_CHANGECAM_TV )
			{
//				if( cameraMode != CAMMODE_TV )
//				{
					changeCamTV();
//				}
			} else
			if( cmd == CMD_CHANGECAM_INT )
			{
				changeCamInternal();
			} else
			if( cmd == CMD_CHANGECAM_EXT )
			{
				if (cameraMode != CAMMODE_FOLLOW)
				{
					changeCamFollow();
				}
			} else
			if( cmd == CMD_CHANGECAM_CHASE )
			{
				changeCamChase();
			} else
			if( cmd == CMD_CHANGECAM_INVCH )
			{
				changeCamInvChase();
			} else
			if( cmd == CMD_CHANGECAM_FREE )
			{
				if (cameraMode != CAMMODE_FOLLOW)
					changeCamFollow();
		
				changeCamFree();
//				enablePicInPic();
			} else
			if( cmd == CMD_CHANGECAMTARGET )
			{
				if (player.car)
				{
					int old_time = System.timeWarp(-1);
					System.timeWarp(0.0); // Pause

					Vector3 CarPos = new Vector3 (player.car.getPos());
					Ypr CarYpr = new Ypr (player.car.getOri());
					player.render.destroy(); // Driver destroy
					
					if( nav )
						nav.remMarker( mPlayer );

					if (GetCarStatus == 0)
					{
						releaseCar();	
						player.carlot.lockPlayerCar();
						player.carlot.saveCar( player.carlot.curcar );
						player.carlot.flushCars();

						GetCarStatus = 1;
					}
					else
					{
						player.car.destroy();
						player.car = null;
					}

					VehicleDescriptor nee = GameLogic.getVehicleDescriptor( VehicleType.VS_DEMO );
					player.car = new Vehicle( player, nee.id, nee.colorIndex, 1+Math.random(), 1+Math.random(), 1, 1 );
					
					player.car.setParent( map );
					player.controller.command( "controllable " + player.car.id() );
					player.car.setMatrix( CarPos, new Ypr(CarYpr.y, 0.0, 0.0) );

					player.render = new RenderRef( map, player.driverID, "player" );
					player.controller.command( "renderinstance " + player.render.id() );

					player.car.setDefaultTransmission();
					player.car.setDefaultSteeringHelp();

					//lastCamPos = new Vector3( 0, 3, 6);
					//lastCamPos.add( player.car.getPos() );
					changeCamTarget(player.car);
					changeCamFollow();// cameraPos );

					map.command( "obs_add " + player.car.id() );

					player.car.command( "reload");
					System.timeWarp( old_time );
					player.car.wakeUp();

					if (nav)
						mPlayer = nav.addMarker( player );
					//Color_Number = Math.random() * GameLogic.CARCOLORS.length;
					//player.car.command( "texture " + GameLogic.CARCOLORS[Color_Number] + " 1" );	
				}
			}
			else
			if( cmd == CMD_OSDONOFF )
			{
				osdEnabled = 1-osdEnabled;
				enableOsd( osdEnabled );
				if (MirrorStatus == 1)
					Mirror( osdEnabled );
			} 
			else
			if( cmd == CMD_CAMROTATE )
			{
				if( camRotate )
				{
					cam.command( "angle 0 0" );		//0.7853 = (2*pi)/8.0
				}
				else
				{
					cam.command( "angle 0 8.0 0.7853" );		//0.7853 = (2*pi)/8.0
				}

				camRotate = 1-camRotate;
			} else
			if( cmd == CMD_QUICKREPAIR )
			{
				//GameLogic.spendTime( 1*3600 );
				//super.refresh( GameLogic.getTime() );
				//renderer.command( "hide " + osd.getViewport().id());
				//refreshCamera();

				Ypr CarYpr = new Ypr (player.car.getOri());

				if (CarYpr.r > 1.3 || CarYpr.r < -1.3 || CarYpr.p > 1.3 || CarYpr.p < -1.3)
				{
					Vector3 CarPos = new Vector3 (player.car.getPos());
					player.car.setMatrix( CarPos, new Ypr(CarYpr.y, 0.0, 0.0) );
					player.car.command( "idle");
					player.car.wakeUp();
				}

				player.car.repair();
				player.car.command( "reload" );
			}
			else
			if( cmd == CMD_SIMPAUSE )
			{
				TimeTextStatus = 1; TimeTextWait = System.currentTime()+2;

				if ( System.timeWarp(-1.0) > 0.0 )
				{
					System.timeWarp(0.0);
					cam.command( "simulate 1" );
					TimeText.changeText( "Time: Pause" );
				}
				else
				{
					cam.command( "simulate 0" );
					TimeText.changeText( "Time: Normal" );
					System.timeWarp( 1.0 );
					if( camRotate )
						cam.command( "angle 0 10.0 0.7853" );		//0.7853 = (2*pi)/8.0
				}
			}
			else
			if( cmd == CMD_SIMSPEEDINC )
			{
				TimeTextStatus = 1; TimeTextWait = System.currentTime()+2;

				float t = 2.0*System.timeWarp(-1.0);
				if( t < 8.0 )
				{
					if( t == 0.0 )
						t = 1.0/512;
					System.timeWarp( t );
				}
					
				if( t == 1.0 )
				{
					cam.command( "simulate 0" );
					TimeText.changeText( "Time: Normal" );
				}
				else
				if( t > 1.0 )
				{
					cam.command( "simulate 0" );
					TimeText.changeText( "Time: x" + System.timeWarp(-1.0) );
				}
				else
				{
					cam.command( "simulate 1" );
					TimeText.changeText( "Time: x" + System.timeWarp(-1.0) );
				}
				
				if( camRotate )
				cam.command( "angle 0 10.0 0.7853" );		//0.7853 = (2*pi)/8.0
			} 
			else
			if( cmd == CMD_SIMSPEEDDEC )
			{
				TimeTextStatus = 1; TimeTextWait = System.currentTime()+2;

				float t = 0.5*System.timeWarp(-1.0);
				if (t < 1.0/512)
					t = 0.0;
				System.timeWarp(t);
				
				if( t == 0)
				{
					TimeText.changeText( "Time: Pause" );
				}
				else
				if( t == 1.0 )
				{
					cam.command( "simulate 0" );
					TimeText.changeText( "Time: Normal" );
				}
				else
				if( t > 1.0 )
				{
					cam.command( "simulate 0" );
					TimeText.changeText( "Time: x" + System.timeWarp(-1.0) );
				}
				else
				{
					cam.command( "simulate 1" );
					TimeText.changeText( "Time: x" + System.timeWarp(-1.0) );
				}

				if( camRotate )
					cam.command( "angle 0 10.0 0.7853" );		//0.7853 = (2*pi)/8.0
			} 
			else
			if( cmd == CMD_INGAMEMENU )
			{
				if( mc_active )
					osdCommand( CMD_MECHANIC );
				else
				if( pt_active )
					osdCommand( CMD_PAINT );
				else
				{
					if( im_active )
					{
						ingameMenu.hide();

						//hatha valtozott az ingamemenuben!!
						player.car.setDefaultTransmission();
						player.car.setDefaultSteeringHelp();
						player.car.setDefaultASR();
						player.car.setDefaultABS();

						//hatha valtozott a gpsmode?
						if(	nav )
							nav.changeMode( Config.gpsMode );


						//System.timeWarp(1.0);

						if( ingameMenu.reqTrackExit )
						{
							int spec;

							if( this instanceof ROCTrack )
							{
								if( !this.testMode )
								{
									spec = 1;
								}
							}

							if( spec )
								this.giveUpRace();
							else
								GameLogic.changeActiveSection( parentState );
						}
					}
					else
					{
						//System.timeWarp(0.0);
						ingameMenu.show();
						if( cam && cam.id() )
							ingameMenu.setActiveCamera( cam );
					}

					im_active=1-im_active;
				}
			}
			else
			if( cmd == CMD_EXIT )
			{
				if( mc_active )
					osdCommand( CMD_MECHANIC );

				if( pt_active )
					osdCommand( CMD_PAINT );

				if( im_active )
					osdCommand( CMD_INGAMEMENU );

				GameLogic.changeActiveSection( parentState );
			}
			else
			if( cmd == CMD_REPAIR )
			{
				if( RepairStatus == 0 )
				{
					closeUpBegin();	// OpenSLRR (#487): the close-up uses cam
					System.timeWarp(0.0);
					enableOsd(0);
					mechanic.filterInventory( 0,0,0 );
					mechanic.show();
					cam.command( "look " + player.car.id() + " 0,0,0 0,0,0 0.2" );
					cam.command( "move " + player.car.id() + " 0,0,0 3.5" );
					cam.command( "dist 1.0 10.0");
					cam.command( "smooth 0.05 0.5");
					cam.command( "simulate 1" );
					Input.cursor.addHandler(this);	//kivancsiak vagyunk ra, mit csinal az eger
					RepairStatus = 1;
				}
				else
				if( RepairStatus == 1 )
				{
					mechanic.hide();
					painter.show();

					RepairStatus = 2;
				}
				else
				if( RepairStatus == 2)
				{
					RepairStatus = 0;

					mechanic.hide();
					painter.hide();

					player.controller.command( "camera" + cam.id() );
					System.timeWarp(1.0);
					painter.hide();
					enableOsd(1);
					closeUpEnd();	// OpenSLRR (#487)
				}
			}
			else
			if( cmd == CMD_MECHANIC )
			{
				if( mc_active )
				{
					cam.command( "dist 2.5 10.0");
					cam.command( "smooth 0.5 0.5");
					cam.command( "force 1.6 0.4 -0.4" );
					cam.command( "torque 0.05" );
					player.controller.command( "camera" + cam.id() );

					Input.cursor.remHandler(this);	//kivancsiak vagyunk ra, mit csinal az eger
					player.car.command( "start" );	//release
					mechanic.hide();
					mechanic.filterInventory( 1,1,1 );
					enableOsd(1);
					closeUpEnd();	// OpenSLRR (#487)
				}
				else
				{
					closeUpBegin();	// OpenSLRR (#487): the close-up uses cam
					enableOsd(0);
					mechanic.filterInventory( 0,0,0 );
					mechanic.show();
					cam.command( "look " + player.car.id() + " 0,0,0 0,0,0 0.2" );
					cam.command( "move " + player.car.id() + " 0,0,0 3.5" );
					cam.command( "dist 2.5 4.6");
					cam.command( "smooth 0.05 0.5");
					player.car.command( "stop" );	//grab
					Input.cursor.addHandler(this);	//kivancsiak vagyunk ra, mit csinal az eger
				}

				mc_active=1-mc_active;
			}
			else
			if( cmd == CMD_PAINT )
			{
				if( pt_active )
				{
					cam.command( "dist 2.5 10.0");
					cam.command( "smooth 0.5 0.5");
					cam.command( "force 1.6 0.4 -0.4" );
					cam.command( "torque 0.01" );
					player.controller.command( "camera" + cam.id() );
					player.car.command( "start" );	//release
					painter.hide();
					enableOsd(1);
					closeUpEnd();	// OpenSLRR (#487)
				}
				else
				{
					closeUpBegin();	// OpenSLRR (#487): the close-up uses cam
					enableOsd(0);

					painter.show();
					cam.command( "look " + player.car.id() + " 0,0,0 0,0,0 0.2" );
					cam.command( "move " + player.car.id() + " 0,0,0 3.5" );
					cam.command( "dist 2.5 4.6");
					cam.command( "smooth 0.05 0.5");
					player.car.command( "stop" );	//grab
				}

				pt_active=1-pt_active;
			}

		}
	}

	public void osdCommand( int cmd )
	{
		mp.putMessage( new Event( cmd ) );
	}

	public void enableOsd( int enable )
	{
		if( enable )
		{
			osdCounter++;
			if( osdCounter == 1 )	// 0 -> 1
			{
				player.showOsd();
				if( nav )
					nav.show();
				//osd.show(0);	//csak screenshotozashoz!!
			}
		}
		else
		{
			osdCounter--;
			if( osdCounter == 0 )	// 1 -> 0
			{
				player.hideOsd();
				if( nav )
					nav.hide();
				//osd.hide(0);	//csak screenshotozashoz!!
			}
		}
	}
}

