package java.game;

import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.render.*;	//Text
import java.render.osd.*;	//Text
import java.sound.*;
import java.render.osd.dialog.*; //Dialog Window

public class TestTrack extends Track
{
	final static String MapName = "Test Track";
	final static String MapDescription = "\n It is default game's Test Track with Miran's Destruction Tools 1.0 \n \n \n There You can test Your car acceleration, handling etc, take a part in The Weight Challenge, get some damge using Elevator or simply: take a ride.";
	final static String MapAuthor = "Authors: Invictus & Miran";
	final static ResourceRef MapImage = new ResourceRef(MW_Mod:0x0032r);
	final static float	TS_Version = 1.0;

	final static int MPH_60 = 771.604938271604938271604938271605;
	final static int MPH_124 = 3086.41975308641975308641975308642;

	Vector3 posExit = new Vector3( 189.2, -3.8, -1417.0 );
//	Vector3 posExit = new Vector3( -200, 0.0, 90);
//	Vector3 posExit = new Vector3( -233, 0, -30);
	Ypr		oriExit = new Ypr( 2.88, 0.05, 0.0 );
	Vector3 WCH_Start = new Vector3( 251.5, 1.5, -558);
	Vector3 Elevator_Pos = new Vector3( -233, -0.8, -12);
	Vector3 PolicePos = new Vector3( 14, 0, -1128);

	Text	messageTextA;
	Text	messageTextB;
	Text	messageText0;
	Text	messageText1;
	Text	messageText2;

	int		testStatus = 0;
	int		triggerIndex = 0;
	int		acceleration = 0;
	int		acceleration120 = 0;
	float	testStartTime = 0.0f;

	Marker	mPolice;

	Round	actRound = null;
	Round[]	round = new Round[2];
	int		rounds = 2;

// Miran's:
	int			WeightSFXbuffor, WeightSFXspeed, WeightSFXstatus, ElevatorRotate, KeyCheckStat, KeyCheck, OptionE, Survived, LightsStatus, Lights, PrimeWin, LastLevel, SFX_E_Loop_ID, WCH_Level, Level = 0, WeightBornStatus, SurvivedGoal, Weight_Created, Player_Dialog_Text_Status;
	RenderRef	WMarker, WMarkerF, Elevator_Platform, Elevator_Arm, Light1, Light2, Light3, Light4;
	PhysicsRef	PlatformB = new PhysicsRef(), Platform = new PhysicsRef(), Arm = new PhysicsRef();
	float		Frequency, CalcRotSpeed, RotateGoal, RotateSpeed, CalcSpeed, TimX, LightsTimer, ToSlowTime, ToSlow, PDTS_time, PDTS_time_start, Weight_Time, X = 0.01, ElevatorStatus = 0, ElevatorSpeed, Weight_Rotate, Weight_Dist;
	Text		TimeInfo, SpeedInfo, PosInfo, YprInfo, Level_Info, Weight_Text, Weight_Frequency_Text, Weight_Survived_Text, Weight_Level_Text, Weight_Distance_Text, Player_Dialog_Text;
	Rectangle	Level_Info_Frame, Info_Strip;

	Vector3		WeightSFXpos = new Vector3(0,0,0), MarkerVector = new Vector3 (WCH_Start), ElevatorPlatformVectorSub = new Vector3 (Elevator_Pos),ElevatorPlatformVector = new Vector3 (Elevator_Pos), ElevatorArmVector = new Vector3 (Elevator_Pos), PlatformBPhysVector = new Vector3(Elevator_Pos), PlatformPhysVector = new Vector3(Elevator_Pos), ArmPhysVector = new Vector3(Elevator_Pos), ElevatorMarkerVector = new Vector3(Elevator_Pos), Light1Pos = new Vector3(Elevator_Pos), Light2Pos = new Vector3(Elevator_Pos), Light3Pos = new Vector3(Elevator_Pos), Light4Pos = new Vector3(Elevator_Pos), CarVector;
	Ypr			CarYpr = new Ypr ( 0.0, 0.0, 0.0), ElevatorPlatformYpr = new Ypr ( 0.0, 0.0, 0.0), ElevatorPhysYpr = new Ypr ( 0.0, 0.0, 0.0), ElevatorPhysBYpr = new Ypr ( 0.0, 0.0, 0.0);
	Pori		PlatformPhysPori, PlatformBPhysPori;
	
	final static int  Weight_Name, WeightSFXname;
	final static RenderRef	GMarker;
	
	Vehicle[]	Weight = new Vehicle[5];
	Vehicle		prime;
	Bot[]	raceBot = new Bot[2];

	Vector3[]	EPPhysPos = new Vector3[18];
	PhysicsRef	EPPhys1 = new PhysicsRef(), EPPhys2 = new PhysicsRef(), EPPhys3 = new PhysicsRef(), EPPhys4 = new PhysicsRef(), EPPhys5 = new PhysicsRef(), EPPhys6 = new PhysicsRef(), EPPhys7 = new PhysicsRef(), EPPhys8 = new PhysicsRef(), EPPhys9 = new PhysicsRef(), EPPhys10 = new PhysicsRef(), EPPhys11 = new PhysicsRef(), EPPhys12 = new PhysicsRef(), EPPhys13 = new PhysicsRef(), EPPhys14 = new PhysicsRef(), EPPhys15 = new PhysicsRef(), EPPhys16 = new PhysicsRef(), EPPhys17 = new PhysicsRef(), EPPhys18 = new PhysicsRef();

	public TestTrack()
	{
		map = new GroundRef( maps.test_track:0x00000001r );
		nav = new Navigator( -5.317-2.64, -15.996, 2.64, maps.test_track.smallmap:0x00000001r, maps.test_track.smallmap:0x00000002r, maps.test_track.smallmap:0x00000005r, 6, 8, 10 );	
	}

	public void StartMap()
	{
		GameLogic.changeActiveSection( new TestTrack() );
	}

	public void enter( GameState prev_state )
	{
		Frontend.loadingScreen.show();
		GfxEngine.flush();

		posStart=posExit;
		oriStart=oriExit;

		PrimeWin = 0;

		// 1/4 miles
		round[0] = new Round( this, "Go straight ahead", "to time the accelearation of your car!" );
		round[0].startdir( Math.PI );
		round[0].point( 0.000, 0.000, -1461.785, 1, 10 );
		round[0].point( 0.000, 0.000, -1059.785, 1, 20 );	//quartermile
		round[0].point( 0.000, 0.000, 148.215, 1, 20 );		//mile

		round[1] = new Round( this, "Test lap time!", "GO!" );
		round[1].startdir( Math.PI );
		round[1].point( -450.598, 0.000, -524.816, 1 );
		round[1].point( -450.598, 0.000, -223.142, 1 );
		round[1].point( -444.736, 1.011, 72.270, 1 );
		round[1].point( -27.535, 1.425, 450.475, 1 );
		round[1].point( 444.689, 1.011, 72.560, 1 );
		round[1].point( 450.598, 0.000, -223.142, 1 );
		round[1].point( 450.598, 0.000, -544.816, 1 );
		round[1].point( 450.598, 0.000, -893.995, 1 );
		round[1].point( 446.293, 0.965, -1124.731, 1 );
		round[1].point( -40.000, 1.425, -1513.328, 1 );	
		round[1].point( -446.000, 1.425, -1124.000, 1 );
		round[1].point( -450.598, 0.000, -893.996, 1 );
		round[1].loop();

		actRound = null;

		super.enter( prev_state );

		messageTextA = osd.createText( "", Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.8 );
		messageTextB = osd.createText( "", Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.7 );
		messageText0 = osd.createText( "", Frontend.largeFont, Text.ALIGN_RIGHT, 0.0, -0.7 );
		messageText1 = osd.createText( "", Frontend.largeFont, Text.ALIGN_LEFT, 0.0, -0.7 );
		messageText2 = osd.createText( "", Frontend.largeFont, Text.ALIGN_CENTER, 0.0, -0.6 );

		PosInfo = osd.createText( "", Frontend.smallFont, Text.ALIGN_LEFT, -0.85, -0.9);	PosInfo.changeColor(0xFF82FF00);
		YprInfo = osd.createText( "", Frontend.smallFont, Text.ALIGN_RIGHT, 0.85, -0.9);	YprInfo.changeColor(0xFF82FF00);
		SpeedInfo = osd.createText( "", Frontend.smallFont, Text.ALIGN_LEFT, -0.85, -0.8);	SpeedInfo.changeColor(0xFF82FF00);
		TimeInfo = osd.createText( "", Frontend.smallFont, Text.ALIGN_RIGHT, 0.85, -0.8);	TimeInfo.changeColor(0xFF82FF00);
		
		Level_Info = osd.createText( "", Frontend.smallFont, Text.ALIGN_LEFT, 0.815, -0.84);
		Player_Dialog_Text = osd.createText( "", new ResourceRef( Text.RID_SLII24 ), Text.ALIGN_CENTER, 0.0, -0.7);

		Weight_Text = osd.createText( "", Frontend.smallFont, Text.ALIGN_LEFT, -0.95, -0.98);
		Weight_Level_Text = osd.createText( "", Frontend.smallFont, Text.ALIGN_CENTER, -0.38, -0.98);
		Weight_Survived_Text = osd.createText( "", Frontend.smallFont, Text.ALIGN_CENTER, -0.06, -0.98);
		Weight_Frequency_Text = osd.createText( "", Frontend.smallFont, Text.ALIGN_CENTER, 0.31, -0.98);
		Weight_Distance_Text = osd.createText( "", Frontend.smallFont, Text.ALIGN_RIGHT, 0.95, -0.98);

		if (player.club == 2)
		{
			GMarker	= new RenderRef( frontend:0x0073r );
		}
		else
		if (player.club == 1)
		{
			GMarker	= new RenderRef( frontend:0x005Fr );
		}
		else
			GMarker	= new RenderRef( frontend:0x005br );

		addTrigger( new Vector3(PolicePos), null, null, "Police_Trigger", 15.0, "trigger" );
		addTrigger( new Vector3(WCH_Start), null, new RenderRef(DestructionTools:0x0022r), "Weight_Challenge_Trigger", 1.5, "trigger" );
		addTrigger( new Vector3(Elevator_Pos), null, new RenderRef(DestructionTools:0x0086r), "Elevator_Trigger", 2.0, "trigger" );
		addTrigger( new Vector3(193, -4.5, -1427), null, GMarker, "Exit_Trigger", 3.0, "trigger" );
		
		WMarker = new RenderRef( map, DestructionTools:0x0024r, "3D marker" );
		WMarker.setMatrix( new Vector3 (MarkerVector), null );
		WMarkerF = new RenderRef( map, DestructionTools:0x0026r, "3D marker frame" );
//		Animation, but I can't export animate meshes ;/
		/*Animation WMarker_Anim = new Animation( WMarker, new ResourceRef(DestructionTools:0x0025r) );
		WMarker_Anim.setSpeed( 10.0 );
		WMarker_Anim.loopPlay();*/
		
		Elevator_Arm = new RenderRef( map, DestructionTools:0x0033r, "Elevator's arm" );
		Elevator_Arm.setMatrix( new Vector3 (ElevatorArmVector), null );
		Arm.createBox( map, 1.135, 72.847, 1.135, "Arm physic" );
		ArmPhysVector.y -= 36.549;
		Arm.setMatrix(new Vector3(ArmPhysVector), null );	
		
		Elevator_Platform = new RenderRef( map, DestructionTools:0x0031r, "Elevator's platform" );//31
		Elevator_Platform.setMatrix( new Vector3 (ElevatorPlatformVector), null );
		Platform.createBox( map,  3.986, 0.01/*0.429*/, 9.850,  "Platform Top physic" );
		Platform.createBox( map,  3.986, 0.02, 9.850,  "Platform Top physic" );
		PlatformPhysVector.y += 1.0;//0.75;
		Platform.setMatrix(new Vector3(PlatformPhysVector), null );

		PlatformB.createBox( map,  3.986, 0.01, 9.850,  "Platform Bottom physic" );
		PlatformB.createBox( map,  3.986, 0.02, 9.850,  "Platform Bottom physic layer 2" );
		PlatformBPhysVector.y += 0.40;
		PlatformB.setMatrix(new Vector3(PlatformBPhysVector), null );

		EPPhys1.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys2.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys3.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys4.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys5.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys6.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys7.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys8.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys9.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys10.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys11.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys12.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys13.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys14.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys15.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys16.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys17.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );
		EPPhys18.createBox( map,  0.2, 0.01, 9.850,  "Platform Rotate physic" );

		EPPhys1.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys2.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys3.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys4.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys5.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys6.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys7.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys8.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys9.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys10.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys11.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys12.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys13.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys14.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys15.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys16.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys17.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
		EPPhys18.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track

		new SfxRef( DestructionTools:0x0081r ).cache(); //def weight hit

// Lights on elevator positions config:
		Light1Pos.x += 1.92;	Light1Pos.y += 1;	Light1Pos.z += 4.92;
		Light2Pos.x += 1.92;	Light2Pos.y += 1;	Light2Pos.z -= 4.92;
		Light3Pos.x -= 1.92;	Light3Pos.y += 1;	Light3Pos.z += 4.92;
		Light4Pos.x -= 1.92;	Light4Pos.y += 1;	Light4Pos.z -= 4.92;
		

		//Player_Dialog_Text.changeText( " •|ü|ç|è|ô|ò|ñ|í|ë|ã|â|à|á|Ü|ê|å|É|Å||ö|õ|ú|ù|°|¢|£|§|¶|ß |99| " );

		Weight[0] = null;
		Weight[1] = null;
		Weight[2] = null;
		Weight[3] = null;
		Weight[4] = null;

	// Police car
		raceBot[0] = new Bot( Math.random()*59, 4, 1, 2.0, 2.0, 20.0);

		GameRef policeCar = new GameRef( map, cars.Misc.Police:0x0006r, "0,0,0"+","+"0,0,0," + 1, "Police Bot" );
		raceBot[0].createCar( map, new Vehicle(policeCar) );
	
		RenderRef policeman = new RenderRef( map, humans:0x0018r, "driver" );
		raceBot[0].car.command( "corpse 0 " + policeman.id() );

		raceBot[0].brain.queueEvent( null, EVENT_COMMAND, "AI_params 50.0 50.35" ); // bardziej kozaczy :D

		raceBot[0].car.setMatrix(new Vector3(PolicePos), new Ypr( 1.5, 0, 0 ));
		raceBot[0].car.wakeUp();
		
		changeCamTarget2(raceBot[0].car);
		if(nav) mPolice = nav.addMarker( new RenderRef(MW_Mod:0x0101r), raceBot[0].car );
		
		Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
		Thread.sleep( 1000 ); // Wait
		new SfxRef(DestructionTools:0x0039r).play(); // SFX
		Player_Dialog_Text.changeText( "Miran's Destruction Tools - (C) Miran 2007" );
		Thread.sleep( 4000 ); // Wait
		new SfxRef(DestructionTools:0x0039r).play(); // SFX
		Player_Dialog_Text.changeText( "Special for annivesary of:" );
		Thread.sleep( 4000 ); // Wait
		new SfxRef(DestructionTools:0x0039r).play(); // SFX
		Player_Dialog_Text.changeText( "www.Redline-Racers.info" );
		Thread.sleep( 5000 ); // Wait
		Player_Dialog_Text.changeText( "Have a fun - Miran" );
		Player_Dialog_Text_Status = 2;
		PDTS_time = 3.0;

		/*//Weight[0] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
		Weight[0] = new Vehicle( map, player.car.getInfo(GII_OWNER), 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
		Weight[0].setMatrix( new Vector3( 0, 20, -180), null );
		Thread.sleep( 8000 ); // Wait
		Weight[0].wakeUp();*/
	}

	public void exit( GameState next_state )
	{
        if( !(next_state instanceof RaceSetup) )
			removeAllTimers();

		if( round[0] )
		{
			round[0].destroy();
			round[0] = null;
		}
		if( round[1] )
		{
			round[1].destroy();
			round[1] = null;
		}

		nav.remMarker( mPolice );	mPolice=null;

		if (PrimeWin == 1)
		{
			player.carlot.addCar( player.car );
			player.carlot.saveCar( player.carlot.curcar );
			player.carlot.flushCars();
			player.car = prime;
			PrimeWin = 0;
		}

		super.exit( next_state );
	}

	public void Police_Trigger( GameRef obj_ref, int event, String param )
	{
		if( param.token(0).intValue() == player.car.id() )
		{
			raceBot[0].followCar( player.car, 0.001 );
			raceBot[0].pressHorn();
		}
	}
	
	public void Elevator_Trigger( GameRef obj_ref, int event, String param )
	{
		if( event == EVENT_TRIGGER_ON && WeightBornStatus == 0)  // only when car go-in triger
		{
			if ( ElevatorStatus == 0 ) // Elevator is down position
			{
				Level_Info.changeText( "Level: " + Level );
				Level_Info_Frame = osd.createRectangle( 1.35, -0.82, 1.2, 0.22, -1, new ResourceRef(frontend:0x0024r) );
				new SfxRef(DestructionTools:0x0034r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Beep Beep
				Thread.sleep( 2500 *TimX ); // Wait 2.5 second
				ElevatorSpeed = 0.01; // Set speed, move up (+)
				ElevatorStatus = 1;
				player.car.wakeUp();
				Lights = 1;// Turn ON lights
				new SfxRef(DestructionTools:0x0035r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				Thread.sleep( 300 *TimX ); // Wait 0.3 second
				SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move
				//Elevator_Move(); //Turn ON elevator
			}
			else
			if ( ElevatorStatus == 1 && ElevatorRotate == 0 || ElevatorStatus == 2 & ElevatorRotate == 0 ) // Elevator is moving up or is on top
			{
				Player_Dialog_Text.changeText( "Watch Your head!" );
				Player_Dialog_Text_Status = 2;
				PDTS_time = 4.0;
				ElevatorStatus = 3; // moving down
				ElevatorSpeed = -0.05; // Set speed, move down (-)

				new SfxRef( DestructionTools:0x0036r ).stop( SFX_E_Loop_ID );  //end move sfx
				new SfxRef( DestructionTools:0x0087r ).stop( SFX_E_Loop_ID );  //end move sfx

				new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				new SfxRef(DestructionTools:0x0037r).play(Elevator_Pos, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move loop
				
				//Elevator_Move(); //Turn ON elevator
			}
		}
	}

	public void Weight_Challenge_Trigger( GameRef obj_ref, int event, String param )
	{
		if( event == EVENT_TRIGGER_ON )  // only when car go-in triger
		{
			if( player.car.getSpeedSquare() < 250.0)// low car speed 
			{
				if (WeightBornStatus == 0 && PrimeWin == 0 && ElevatorStatus == 0 && ElevatorRotate == 0)
				{
					player.car.command( "brake" );
					cam.command( "angle 0 3.0 0.4853" );
					new SfxRef(Frontend:0x007Er).play(); // SFX	

					if( 0 == new YesNoDialog( player.controller, Dialog.DF_LOWPRI|Dialog.DF_MODAL|Dialog.DF_DEFAULTBG, "The Weight Challenge", " \n You wanna take this challenge? \n \n YOUR CAR PAROBABLE BEEN SMASHED! \n \n Is one rule: Your minimum speed is 40 kmh (25 mph) \n \n Have a fun - Miran").display() )
					{
						player.car.command( "start" );
						cam.command( "angle 0 0.0 0.0" );
						Weight_Start();
					}
					else
					{
						cam.command( "angle 0 0.0 0.0" );
						player.car.command( "start" );
					}
				}
			}
		}
	}

	public void Exit_Trigger( GameRef obj_ref, int event, String param )
	{
		if( event == EVENT_TRIGGER_ON )  // only when car go-in triger
		{
			player.car.command( "stop" );
			if( 0 == new YesNoDialog( player.controller, Dialog.DF_LOWPRI|Dialog.DF_MODAL|Dialog.DF_DEFAULTBG, "QUESTION", "Go back to the garage?").display() )
			{
				player.car.command( "start" );
				GameLogic.changeActiveSection( GameLogic.garage );
			}
			else
				player.car.command( "start" );
		}
	}

		public void Weight_Start()
	{// Turn OFF active rounds
		if( round[0] )
		{
			round[0].destroy();
			round[0] = null;
		}
		if( round[1] )
		{
			round[1].destroy();
			round[1] = null;
		}
	//--------------------------
		Sound.changeMusicSet( Sound.MUSIC_SET_RACE );
		
		Info_Strip = osd.createRectangle( 0.0, -0.95, 2.0, 0.1, -1, new ResourceRef(frontend:0x0028r) );// Bacground strip

		WCH_Level = 1; //Level 1
		Survived = 0;
		Weight_Time = 4; // 4 seconds
		Weight_Dist = 9.0; // 9 meters
		SurvivedGoal = 5; //5 Level 2
		LastLevel = 0;//*/

// Debug fast win:
		/*Survived = 273;
		Weight_Time = 2;
		Weight_Dist = 0.0;
		SurvivedGoal = 275;
		LastLevel = 1;//*/

		Weight_Text.changeText( "The Weight Challenge:" );
		Weight_Level_Text.changeText( "Level: " + WCH_Level );
		Weight_Survived_Text.changeText( "Survived: " + Survived );
		Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
		Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );
		Thread.sleep( 1000 *TimX ); // Wait 0.3 second
		Player_Dialog_Text.changeText( "Go, Run, Escape... and Don't Stop!" );
		new SfxRef(DestructionTools:0x0039r).play(); // SFX

		if (Weight_Created == 0)
		{
			Weight[0] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
			Weight[1] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
			Weight[2] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
			Weight[3] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
			Weight[4] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
			Weight_Created = 1;
		}
		Weight[0].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
		Weight[1].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
		Weight[2].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
		Weight[3].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
		Weight[4].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track

		Thread.sleep( 5000 *TimX ); // Wait 5 second

		Player_Dialog_Text.changeText( "Watch Your head!" );
		Player_Dialog_Text_Status = 2;
		PDTS_time = 3;

		WeightBornStatus = 1;
		WeightSFXstatus = 1;
		WeightSFXname = Weight_Name;
		Create_Weight();
	}
		
	public void Create_Weight()
	{
		if (WeightBornStatus == 1)
		{
			Vector3 Dist = new Vector3( 0.0, 20.0, -1.8 * (Weight_Dist + Math.sqrt(player.car.getSpeedSquare())) );// Clac distance
			Vector3 b = player.car.getPos();
			Vector3 c = new Vector3( -b.x, -b.y, -b.z); //Negateive of b

			Ypr d = player.car.getOri();
			Ypr rot = new Ypr( d.y, 0.0, 0.0 );

			Dist.rotate( rot ); // rotate Dist Vector
			Dist.sub( c );	// Set Pivot of rotate

			Weight[Weight_Name].setMatrix( Dist, new Ypr(0.0 /*Math.random()*3f*/, 0, 0) );
			Weight[Weight_Name].wakeUp();
		
			if (WeightBornStatus == 1)// Check again is status changed when sleept
			{
				if (Weight_Name >= 4) //Reset, possible names 0-4
				{
					Weight_Name = 0;
				}
				else
					Weight_Name += 1;

				Survived += 1; // Survived info
				Weight_Survived_Text.changeText( "Survived: " + Survived );
				
				Frequency = System.simTime() + Weight_Time; // Set time to next weight
			}
		}
	}

	public void Elevator_Top()
	{	
		if (ElevatorStatus == 0)
		{
			Player_Dialog_Text.changeText( "Normal Motion - Time: x1.0" );
			Player_Dialog_Text_Status = 2;
			PDTS_time = 4;
			System.timeWarp(1.0);
			new SfxRef(DestructionTools:0x0091r).play(ElevatorPlatformVector, 0.0, 1.0, 1.0, 0); // SFX Spring

			Thread.sleep( 7000 ); //Wait 5s
			if (cameraMode = CAMMODE_FOLLOW)
			{
				changeCamTV();
				changeCamFollow();
			}
			ElevatorStatus = 3; // moving down
			ElevatorSpeed = -0.05; // Set speed, move down (-)

			new SfxRef( DestructionTools:0x0036r ).stop( SFX_E_Loop_ID );  //end move sfx
			new SfxRef( DestructionTools:0x0087r ).stop( SFX_E_Loop_ID );  //end move sfx

			new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
			SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move loop
		}
		else
		if (ElevatorStatus == 2)
		{	
			CarVector = player.car.getPos();
			if (CarVector.y > 40)
			{	
				Thread.sleep( 5000 ); //Wait 5s
				KeyCheck = 1;
				KeyCheckStat = player.car.getCruiseControl();

				Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
				Player_Dialog_Text.changeText("Drawing");
				new SfxRef(DestructionTools:0x0039r).play( 0.6, 1.0, 0); // SFX
				
				Thread.sleep( 1500 ); //Wait
				Player_Dialog_Text.changeText(". Drawing .");
				new SfxRef(DestructionTools:0x0039r).play( 0.7, 1.0, 0); // SFX
				
				Thread.sleep( 1400 ); //Wait
				Player_Dialog_Text.changeText(". . Drawing . .");
				new SfxRef(DestructionTools:0x0039r).play( 0.8, 1.0, 0); // SFX

				Thread.sleep( 1300 ); //Wait
				Player_Dialog_Text.changeText(". . . Drawing . . .");
				new SfxRef(DestructionTools:0x0039r).play( 0.9, 1.0, 0); // SFX

				Thread.sleep( 1200 ); //Wait
				OptionE = KeyCheck - 2;

				if (OptionE >= 1 && OptionE <= 6) // if 1-6
				{
					Player_Dialog_Text.changeText("Chosen option: " + OptionE );
				}
				else
				{
					OptionE = 1+Math.random()*6;
					Player_Dialog_Text.changeText("Drawed option: " + OptionE );
				}
				Player_Dialog_Text_Status = 2;
				PDTS_time = 3.0;
				KeyCheck = 0;// Turn OFF Checker
				//Elevator_Top(); //Loop Drawing for debuging
				Thread.sleep( 3000 ); //Wait
			}
			else
			{
				OptionE = 3;
				Player_Dialog_Text.changeText("Option " + OptionE + " activated!");
				Player_Dialog_Text_Status = 2;
				PDTS_time = 3.0;
				Thread.sleep( 3500 ); //Wait
			}
			
			if (OptionE == 1)
			{ // Rotate right
				RotateSpeed = 0.002;
				RotateGoal = 0.8;
				RotateStart(1);
			}
			else
			if (OptionE == 2)
			{ // Rotate left
				RotateSpeed = -0.002;
				RotateGoal = -0.8;
				RotateStart(2);
			}
			else
			if (OptionE == 3)
			{
				if (Weight_Created == 0)
				{
					Weight[0] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
					Weight[1] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
					Weight[2] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
					Weight[3] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
					Weight[4] = new Vehicle( map, DestructionTools:0x0015r, 6, 2.0, 0.0, 1.0, 1.0 );// Weight Create
					Weight_Created = 1;

					Weight[0].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
					Weight[1].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
					Weight[2].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
					Weight[3].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
					Weight[4].setMatrix( new Vector3(0.0, -1500.0, 0.0), null );//now place it under track
				}

				Weight[Weight_Name].setMatrix( new Vector3 (ElevatorPlatformVector.x, ElevatorPlatformVector.y + 15/*19.2*/, ElevatorPlatformVector.z), new Ypr(Math.random()*3, 0, 0) );
				Weight[Weight_Name].wakeUp();
				WeightSFXname = Weight_Name;
				WeightSFXstatus = 1;

				if (Weight_Name >= 4) //Reset, possible names 0-4
				{
					Weight_Name = 0;
				}
				else
					Weight_Name += 1;

				OptionE = Math.random()*10;
				Thread.sleep( 5000 * TimX ); //Wait
				WeightSFXstatus = 0;

				if (OptionE < 5)
				{ // Rotate left
					RotateSpeed = -0.002;
					RotateGoal = -0.8;
					RotateStart(2);
				}
				else
				{ // Rotate right
					RotateSpeed = 0.002;
					RotateGoal = 0.8;
					RotateStart(1);
				}
				OptionE = 0;
			}
			else
			if (OptionE == 4)
			{
				if (cameraMode != CAMMODE_FOLLOW)
				changeCamFollow();
				cam.command( "move " + map.id() + " 0,0,0 0.0" );
				cam.command( "look " + player.car.id() + " 0,0,0 0,0,0" );
				cam.setMatrix( new Vector3(ElevatorPlatformVector.x + 6, ElevatorPlatformVector.y + 1.5, ElevatorPlatformVector.z + 6), new Ypr(1.0, 0, 0)); 
				cam.command( "simulate 1" );

				ElevatorSpeed = -0.03; // Set speed, move down (-)
				ElevatorStatus = 4; // moving down and stop at 68.5 meter
				player.car.wakeUp();
				new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				new SfxRef(DestructionTools:0x0037r).play(Elevator_Pos, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move	
			}
			else
			if (OptionE == 5)
			{
				Player_Dialog_Text.changeColor(0xFFFFFFFF); //White
				Player_Dialog_Text.changeText("You have only one way to down... Jump");
				Player_Dialog_Text_Status = 2;
				PDTS_time = 3.0;
			}
			else // OptionE 6, or something error
			{ // Elevator back to ground
				ElevatorSpeed = -0.02; // Set speed, move down (-)
				ElevatorStatus = 3; // moving down
				player.car.wakeUp();
				new SfxRef(DestructionTools:0x0035r).play(Elevator_Pos, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start	
				Thread.sleep( 300 *TimX ); // Wait 0.3 second
				new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
				SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move	
			}	
		}
	}
	
	public void RotateStart( int mode )
	{
		Platform.setMatrix( new Vector3(0.0, -1000, 0.0), null); //Place it under track
		PlatformB.setMatrix( new Vector3(0.0, -1000, 0.0), null); //Place it under track
		
		ElevatorRotate = mode;
		player.car.wakeUp();
		new SfxRef(DestructionTools:0x0088r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
		SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0087r ).play( ElevatorPlatformVector, 0.0, System.timeWarp(-1) - 0.4, 1.0, 0 );  //sfx move
	}

	public void RotateEnd()
	{
		new SfxRef(DestructionTools:0x0089r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
		new SfxRef( DestructionTools:0x0087r ).stop( SFX_E_Loop_ID );  //end move sfx

		if (ElevatorPlatformYpr.r > 0.1) //Platform is rotated right
		{
			Thread.sleep( 3000 *TimX ); // Wait 5s
			RotateSpeed = -0.003; //Rotate left
			RotateGoal = 0.0;
			ElevatorRotate = 2; //Rotate left
			new SfxRef(DestructionTools:0x0088r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
			SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0087r ).play( ElevatorPlatformVector, 0.0, System.timeWarp(-1) - 0.4, 1.0, 0 );  //sfx move
		}
		else
		if (ElevatorPlatformYpr.r < -0.1) //Platform is rotated left
		{
			Thread.sleep( 3000 *TimX ); // Wait 5s
			RotateSpeed = 0.003; //Rotate right
			RotateGoal = 0.0;
			ElevatorRotate = 1; //Rotate right
			new SfxRef(DestructionTools:0x0088r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
			SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0087r ).play( ElevatorPlatformVector, 0.0, System.timeWarp(-1) - 0.4, 1.0, 0 );  //sfx move
		}
		else // Platform is no rotated
		{ // Elevator back to ground
			EPPhys1.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys2.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys3.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys4.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys5.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys6.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys7.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys8.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys9.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys10.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys11.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys12.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys13.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys14.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys15.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys16.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys17.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track
			EPPhys18.setMatrix(new Vector3(0.0, -1000, 0.0), null ); // Place it under track

			PlatformPhysVector = new Vector3(ElevatorPlatformVector.x, ElevatorPlatformVector.y + 1, ElevatorPlatformVector.z); // Set simple phys
			PlatformBPhysVector = new Vector3(ElevatorPlatformVector.x, ElevatorPlatformVector.y + 0.4, ElevatorPlatformVector.z); // Set simple phys

			Platform.setMatrix( new Vector3(PlatformPhysVector), null);
			PlatformB.setMatrix( new Vector3(PlatformBPhysVector), null);

			Thread.sleep( 3000 *TimX ); // Wait 3s
			ElevatorSpeed = -0.02; // Set speed, move down (-)
			ElevatorStatus = 3; // moving down
			player.car.wakeUp();
			new SfxRef(DestructionTools:0x0035r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
			new SfxRef(DestructionTools:0x0037r).play(Elevator_Pos, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Start
			Thread.sleep( 300 *TimX ); // Wait 0.3 second
			SFX_E_Loop_ID = new SfxRef( DestructionTools:0x0036r ).play( Elevator_Pos, 0.0, System.timeWarp(-1) - 0.5, 1.0, 0 );  //sfx move
		}
	}

	/*public void Elevator_Move()
	{
		if (ElevatorStatus == 1 || ElevatorStatus == 3)
		{
			ElevatorPlatformVector.y += ElevatorSpeed;
			PlatformPhysVector.y += ElevatorSpeed;
			PlatformBPhysVector.y += ElevatorSpeed;
			ElevatorArmVector.y += ElevatorSpeed;
			ArmPhysVector.y += ElevatorSpeed;
			Light1Pos.y += ElevatorSpeed;
			Light2Pos.y += ElevatorSpeed;
			Light3Pos.y += ElevatorSpeed;
			Light4Pos.y += ElevatorSpeed;

			Elevator_Platform.setMatrix( new Vector3 (ElevatorPlatformVector), null );
			Platform.setMatrix(new Vector3(PlatformPhysVector), null );
			PlatformB.setMatrix(new Vector3(PlatformBPhysVector), null );
			Elevator_Arm.setMatrix( new Vector3 (ElevatorArmVector), null );
			Arm.setMatrix(new Vector3(ArmPhysVector), null );
					
			if (Light1 && Light4)
			{
				Light1.setMatrix(Light1Pos, null);
				Light2.setMatrix(Light2Pos, null);
				Light3.setMatrix(Light3Pos, null);
				Light4.setMatrix(Light4Pos, null);
			}

			Level = ElevatorPlatformVector.y/2.3;
			Level_Info.changeText( "Level: " + Level );

			if (ElevatorStatus == 1) //Move Up
			{	
				if (ElevatorPlatformVector.y > 0.5 && ElevatorPlatformVector.y < 0.6)// Heghts 0.5-0.6
					ElevatorSpeed = 0.015;
				else
				if (ElevatorPlatformVector.y > 1 && ElevatorPlatformVector.y < 1.1)// Heghts 1.0-1.1
					ElevatorSpeed = 0.02;
				else
				if (ElevatorPlatformVector.y > 1.5 && ElevatorPlatformVector.y < 1.6)// Heghts 1.5-1.6
					ElevatorSpeed = 0.03;
				else
				if (ElevatorPlatformVector.y > 2 && ElevatorPlatformVector.y < 2.1)// Heghts 2.0-2.1
					ElevatorSpeed = 0.04;
				else
				if (ElevatorPlatformVector.y > 2.5 && ElevatorPlatformVector.y < 2.6)// Heghts 2.5-2.6
					ElevatorSpeed = 0.05;
				else
				if (ElevatorPlatformVector.y > 70) //Max height
				{
					ElevatorStatus = 2;//Top Stop
					new SfxRef(DestructionTools:0x037r).play(ElevatorPlatformVector, 0.0, 1.0, 1.0, 0); // SFX Stop
					new SfxRef( DestructionTools:0x036r ).stop( SFX_E_Loop_ID );  //end move sfx

					Thread.sleep( 4000 *TimX ); // Wait 3s
					ElevatorStatus = 3;
					ElevatorSpeed = -0.02;
				}

			}
			else
			if (ElevatorStatus == 3 ) // Move Down
			{	
				if (ElevatorPlatformVector.y > 69.5 && ElevatorPlatformVector.y < 69.6)// Heghts 69.5-69.6
					ElevatorSpeed = -0.03;
				else
				if (ElevatorPlatformVector.y > 69.0 && ElevatorPlatformVector.y < 69.1)// Heghts 69.0-69.1
					ElevatorSpeed = -0.04;
				else
				if (ElevatorPlatformVector.y > 68.5 && ElevatorPlatformVector.y < 68.6)// Heghts 68.5-68.6
					ElevatorSpeed = -0.05;
				else
			// Slowing down
				if (ElevatorPlatformVector.y > 3.0 && ElevatorPlatformVector.y < 3.1)// Heghts 3.0-3.1
					ElevatorSpeed = -0.04;
				else
				if (ElevatorPlatformVector.y > 2.5 && ElevatorPlatformVector.y < 2.6)// Heghts 2.5-2.6
					ElevatorSpeed = -0.03;
				else
				if (ElevatorPlatformVector.y > 2.0 && ElevatorPlatformVector.y < 2.1)// Heghts 2.0-2.1
					ElevatorSpeed = -0.02;
				else
				if (ElevatorPlatformVector.y > 1.5 && ElevatorPlatformVector.y < 1.6)// Heghts 1.5-1.6
					ElevatorSpeed = -0.01;
				else
				if (ElevatorPlatformVector.y <= Elevator_Pos.y) //Down positionm, Min height
				{
				ElevatorStatus = 0; // Turn OFF
				new SfxRef(DestructionTools:0x037r).play(ElevatorPlatformVector, 0.0, 1.0, 1.0, 0); // SFX Stop
				new SfxRef( DestructionTools:0x036r ).stop( SFX_E_Loop_ID );  //end move sfx
				Level_Info.changeText( "" );
				Level_Info_Frame.finalize();
				Thread.sleep( 4000 *TimX ); // Wait 4s
				Lights = 0; // Turn OFF lights
				}

			}

			Thread.sleep( 40 *TimX ); // Wait 0.04s = 1/25s = 25 FPS... in theory
			Elevator_Move(); // And Loop
		}
	}*/

	public void event_handlerTrigger( GameRef obj_ref, int event, String param )
	{
		int	id = param.token(0).intValue();
		if (id == player.car.id())
		{
			if( event == EVENT_TRIGGER_ON )
			{
				if( !activeTrigger )
				{
					activeTrigger=obj_ref.id();
				}
			}
			else
			{
				activeTrigger=0;
			}
		}
	}

	public void handleEvent( GameRef obj_ref, int event, int param )
	{
		super.handleEvent( obj_ref, event, param );

		if( event == EVENT_TIME )
		{
			if( param == 2 )	//one sec tick
			{
				if( activeTrigger )
				{
					if( player.car.getSpeedSquare() < 0.25 )	//slow speed?
					{	
						for( int i = 0; i < rounds; i++ )
						{
							if( round[i] )
							{
								if( activeTrigger == round[i].trigger.elementAt(0).trigger.id() )
								{
									Ypr ypr = player.car.getOri();

									// OpenSLRR (#465): the heading difference in [-PI, PI].
									float diff = Math.wrapAngle(ypr.y-round[i].startOri.y);

									if( diff < 0.5f && diff > -0.5f)
									{
										actRound = round[i];
										testStatus = 0;
										triggerIndex = 1;
										if( actRound.type == 0 )
										{
											acceleration = 1;
											acceleration120 = 1;
										}
										else
										{
											acceleration = 0;
											acceleration120 = 0;
										}
									}
								}
							}
						}
					}
				}
				else
				{
				}
			}
		}
	}

	public void animate()
	{
		super.animate();

	// Cuirse Control KeyChecker for Elevator Option Chose
		if (KeyCheck > 0)
		{
			if(player.car.getCruiseControl() == 1)
			{
				if (KeyCheckStat == 1)
				{
					KeyCheck += 1;
					KeyCheckStat = 0;
				}
			}
			else
			{
				if (KeyCheckStat == 0)
				{
					KeyCheck += 1;
					KeyCheckStat = 1;
				}
			}
		}
	// Animate Weight challenge 3d marker on the track:
		if (MarkerVector.y > WCH_Start.y+0.1) //Max marker up position
			X = -0.005; // set down move speed
		else
		if (MarkerVector.y < WCH_Start.y-0.1) // Max marker down position
			X = 0.005; // set up move speed

		MarkerVector.y += X;
		WMarker.setMatrix( new Vector3 (MarkerVector), null );

		Ypr	MarkerFYpr = new Ypr ((System.currentTime()*3), 0.0, 0.0);
		WMarkerF.setMatrix( new Vector3 (WCH_Start), new Ypr (MarkerFYpr) );

	// Calc slow motion for timmers:
		CalcSpeed = ElevatorSpeed * System.timeWarp(-1); //For elevator moves
		CalcRotSpeed = RotateSpeed * System.timeWarp(-1); //For elevator moves
		// And for Thread.Sleep:
		if( System.timeWarp(-1) == 0) // division by zero is impossible
			TimX = 0.007;
		else
			TimX = 1/System.timeWarp(-1);
	
	// Moder's help texts:
		/*if (player.car)
		{
			CarVector = player.car.getPos();
			CarYpr = player.car.getOri();
	
			PosInfo.changeText( "Car Position: X: " + CarVector.x + ", Y: " + CarVector.y + ", Z: " + CarVector.z );
			YprInfo.changeText( "Car Rotation: Y: " + CarYpr.y + ", P: " + CarYpr.p + ", R: " + CarYpr.r );
			SpeedInfo.changeText( "Car Speed Square: " + player.car.getSpeedSquare() + " , Cuirse Control: " + player.car.getCruiseControl() );//+ ", Player.car.id: " + player.id() );
			TimeInfo.changeText( "System.SimTime: " + System.simTime() + ", System.timeWarp: " + System.timeWarp(-1) );
		}//*/

	// Show Dialog Messages:
		if (Player_Dialog_Text_Status == 2)
		{
			new SfxRef(DestructionTools:0x0039r).play(); // SFX
			PDTS_time_start = System.simTime();
			Player_Dialog_Text_Status = 1;
		}
		else
		if (Player_Dialog_Text_Status == 1)
		{
			if (System.simTime() - PDTS_time_start >= PDTS_time)
			{
				Player_Dialog_Text.changeText( "" );//clean
				Player_Dialog_Text.changeColor(0xFFFFFFFF); //Reset color (White)
				Player_Dialog_Text_Status = 0;
			}
		}	
		
		// Elevator's lights blinkink:
		if (LightsTimer <= System.simTime())
		{
			if (Light1 && Light4)
			{
				Light1.destroy();	Light1 = null;
				Light2.destroy();	Light2 = null;
				Light3.destroy();	Light3 = null;
				Light4.destroy();	Light4 = null;
				player.car.wakeUp();
			}

			if (LightsStatus == 0 && Lights == 1)
			{
				Light1 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light2 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light3 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light4 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				LightsStatus = 1;
				LightsTimer = System.simTime() + 0.05;
			}
			else
			if (LightsStatus == 1)
			{
				Light1 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light2 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light3 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light4 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				LightsStatus = 2;
				LightsTimer = System.simTime() + 0.05;
			}
			else
			if (LightsStatus == 2)
			{
				Light1 = new RenderRef( map, DestructionTools:0x0084r, "Light" );
				Light2 = new RenderRef( map, DestructionTools:0x0084r, "Light" );
				Light3 = new RenderRef( map, DestructionTools:0x0084r, "Light" );
				Light4 = new RenderRef( map, DestructionTools:0x0084r, "Light" );
				LightsStatus = 3;
				LightsTimer = System.simTime() + 1.5;
			}
			else
			if (LightsStatus == 3)
			{
				Light1 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light2 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light3 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				Light4 = new RenderRef( map, DestructionTools:0x0083r, "Light" );
				LightsStatus = 4;
				LightsTimer = System.simTime() + 0.05;
			}
			else
			if (LightsStatus == 4)
			{
				Light1 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light2 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light3 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				Light4 = new RenderRef( map, DestructionTools:0x0082r, "Light" );
				LightsStatus = 5;
				LightsTimer = System.simTime() + 0.05;
			}
			else	
			if (LightsStatus == 5)
			{
				LightsStatus = 0;
				LightsTimer = System.simTime() + 1.5;
			}

			if (ElevatorStatus == 0 && Light1 && Light4 && ElevatorRotate == 0 || ElevatorStatus == 2 && Light1 && Light4 && ElevatorRotate == 0 )
			{
				Light1.setMatrix(Light1Pos, null);
				Light2.setMatrix(Light2Pos, null);
				Light3.setMatrix(Light3Pos, null);
				Light4.setMatrix(Light4Pos, null);
			}
		}

	// Weight SFX player:
		if (WeightSFXstatus == 1)
		{
			WeightSFXspeed = Weight[WeightSFXname].getSpeedSquare();

			if (WeightSFXbuffor == 0)
			{
				if (WeightSFXspeed > 35)
				{
					WeightSFXbuffor = 1;
				}
			}
			else
			{
				if (WeightSFXspeed < 30)
				{
					WeightSFXpos = Weight[WeightSFXname].getPos();
					new SfxRef(DestructionTools:0x0081r).play(new Vector3(WeightSFXpos), 0.0, System.timeWarp(-1), 1.0, 0); // SFX
					WeightSFXbuffor = 0;
										
					if (WeightSFXname >= 4) //Reset, possible names 0-4
					{
						WeightSFXname = 0;
					}
					else
						WeightSFXname += 1;
				}
				
			}
		}	

	// Elevator section:
		if (ElevatorStatus == 2 && ElevatorRotate > 0)
		{
			ElevatorPlatformYpr.r += CalcRotSpeed;
			
			ElevatorPlatformVectorSub = new Vector3( -ElevatorPlatformVector.x, -ElevatorPlatformVector.y, -ElevatorPlatformVector.z );// Negative vector
	
			Light1Pos = new Vector3( 1.92, 1, 4.92 ); // Reset: 0,0,0 + Lights position on the platform
			Light2Pos = new Vector3( 1.92, 1, -4.92 ); // Reset: 0,0,0 + Lights position on the platform
			Light3Pos = new Vector3( -1.92, 1, 4.92 ); // Reset: 0,0,0 + Lights position on the platform
			Light4Pos = new Vector3( -1.92, 1, -4.92 ); // Reset: 0,0,0 + Lights position on the platform

			EPPhysPos[0] = new Vector3( 1.82, 1.0, 0.0);
			EPPhysPos[1] = new Vector3( 1.62, 1.0, 0.0);
			EPPhysPos[2] = new Vector3( 1.41, 1.0, 0.0);
			EPPhysPos[3] = new Vector3( 1.21, 1.0, 0.0);
			EPPhysPos[4] = new Vector3( 1.0, 1.0, 0.0);
			EPPhysPos[5] = new Vector3( 0.8, 1.0, 0.0);
			EPPhysPos[6] = new Vector3( 0.6, 1.0, 0.0);
			EPPhysPos[7] = new Vector3( 0.4, 1.0, 0.0);
			EPPhysPos[8] = new Vector3( 0.2, 1.0, 0.0);
			EPPhysPos[9] = new Vector3( -0.2, 1.0, 0.0);
			EPPhysPos[10] = new Vector3( -0.4, 1.0, 0.0);
			EPPhysPos[11] = new Vector3( -0.6, 1.0, 0.0);
			EPPhysPos[12] = new Vector3( -0.8, 1.0, 0.0);
			EPPhysPos[13] = new Vector3( -1.0, 1.0, 0.0);
			EPPhysPos[14] = new Vector3( -1.21, 1.0, 0.0);
			EPPhysPos[15] = new Vector3( -1.41, 1.0, 0.0);
			EPPhysPos[16] = new Vector3( -1.62, 1.0, 0.0);
			EPPhysPos[17] = new Vector3( -1.82, 1.0, 0.0);

			EPPhysPos[0].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[1].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[2].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[3].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[4].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[5].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[6].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[7].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[8].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[9].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[10].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[11].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[12].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[13].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[14].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[15].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[16].rotate( ElevatorPlatformYpr ); // rotate vector
			EPPhysPos[17].rotate( ElevatorPlatformYpr ); // rotate vector

			EPPhysPos[0].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[1].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[2].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[3].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[4].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[5].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[6].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[7].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[8].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[9].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[10].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[11].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[12].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[13].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[14].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[15].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[16].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			EPPhysPos[17].sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)

			EPPhys1.setMatrix( new Vector3(EPPhysPos[0]), null);
			EPPhys2.setMatrix( new Vector3(EPPhysPos[1]), null);
			EPPhys3.setMatrix( new Vector3(EPPhysPos[2]), null);
			EPPhys4.setMatrix( new Vector3(EPPhysPos[3]), null);
			EPPhys5.setMatrix( new Vector3(EPPhysPos[4]), null);
			EPPhys6.setMatrix( new Vector3(EPPhysPos[5]), null);
			EPPhys7.setMatrix( new Vector3(EPPhysPos[6]), null);
			EPPhys8.setMatrix( new Vector3(EPPhysPos[7]), null);
			EPPhys9.setMatrix( new Vector3(EPPhysPos[8]), null);
			EPPhys10.setMatrix( new Vector3(EPPhysPos[9]), null);
			EPPhys11.setMatrix( new Vector3(EPPhysPos[10]), null);
			EPPhys12.setMatrix( new Vector3(EPPhysPos[11]), null);
			EPPhys13.setMatrix( new Vector3(EPPhysPos[12]), null);
			EPPhys14.setMatrix( new Vector3(EPPhysPos[13]), null);
			EPPhys15.setMatrix( new Vector3(EPPhysPos[14]), null);
			EPPhys16.setMatrix( new Vector3(EPPhysPos[15]), null);
			EPPhys17.setMatrix( new Vector3(EPPhysPos[16]), null);
			EPPhys18.setMatrix( new Vector3(EPPhysPos[17]), null);
	
			Light1Pos.rotate( ElevatorPlatformYpr ); // rotate vector
			Light2Pos.rotate( ElevatorPlatformYpr ); // rotate vector
			Light3Pos.rotate( ElevatorPlatformYpr ); // rotate vector
			Light4Pos.rotate( ElevatorPlatformYpr ); // rotate vector

			Light1Pos.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			Light2Pos.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
			Light3Pos.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot) 
			Light4Pos.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot) 

			// For the one phys rotate method
			/*	PlatformPhysVector = new Vector3( 0.0, 1.0, 0.0 ); // Reset: 0,0,0 + Phys position on the platform
				PlatformBPhysVector = new Vector3( 0.0, 0.4, 0.0 ); // Reset: 0,0,0 + Phys position on the platform

				PlatformPhysVector.rotate( ElevatorPlatformYpr ); // rotate vector
				PlatformBPhysVector.rotate( ElevatorPlatformYpr ); // rotate vector

				PlatformPhysVector.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)
				PlatformBPhysVector.sub( ElevatorPlatformVectorSub ); // Set pos of vectors group (pivot)

				Platform.setMatrix( new Pori( new Vector3(PlatformPhysVector), new Ypr( 0.0, 0.0, ElevatorPlatformYpr.r)) );
				PlatformB.setMatrix( new Pori( new Vector3(PlatformBPhysVector), new Ypr( 0.0, 0.0, ElevatorPlatformYpr.r)) );
			*/

			if (Light1 && Light4)
			{
				Light1.setMatrix(Light1Pos, null);
				Light2.setMatrix(Light2Pos, null);
				Light3.setMatrix(Light3Pos, null);
				Light4.setMatrix(Light4Pos, null);
			}

			Elevator_Platform.setMatrix( new Vector3(ElevatorPlatformVector), new Ypr(ElevatorPlatformYpr) );

			if (ElevatorRotate == 1 && ElevatorPlatformYpr.r >= RotateGoal) //Rotate right
			{
				ElevatorRotate = 0;
				RotateEnd();
			}
			else
			if (ElevatorRotate == 2 && ElevatorPlatformYpr.r <= RotateGoal) // Rotate Left
			{
				ElevatorRotate = 0;
				RotateEnd();
			}
		}
		else
		if (ElevatorStatus == 1 || ElevatorStatus == 3 || ElevatorStatus == 4 || ElevatorStatus == 5)
		{
			ElevatorPlatformVector.y += CalcSpeed;
			PlatformPhysVector.y += CalcSpeed;
			PlatformBPhysVector.y += CalcSpeed;
			ElevatorArmVector.y += CalcSpeed;
			ArmPhysVector.y += CalcSpeed;
			Light1Pos.y += CalcSpeed;
			Light2Pos.y += CalcSpeed;
			Light3Pos.y += CalcSpeed;
			Light4Pos.y += CalcSpeed;

			Elevator_Platform.setMatrix( new Vector3 (ElevatorPlatformVector), null );
			Platform.setMatrix(new Vector3(PlatformPhysVector), null );
			PlatformB.setMatrix(new Vector3(PlatformBPhysVector), null );
			Elevator_Arm.setMatrix( new Vector3 (ElevatorArmVector), null );
			Arm.setMatrix(new Vector3(ArmPhysVector), null );
					
			if (Light1 && Light4)
			{
				Light1.setMatrix(Light1Pos, null);
				Light2.setMatrix(Light2Pos, null);
				Light3.setMatrix(Light3Pos, null);
				Light4.setMatrix(Light4Pos, null);
			}

			Level = ElevatorPlatformVector.y/2.3;
			Level_Info.changeText( "Level: " + Level );

			if (ElevatorStatus == 1) //Move Up
			{	
				if (ElevatorPlatformVector.y > -0.6 && ElevatorPlatformVector.y < -0.5)
					ElevatorSpeed = 0.015;	
				else
				if (ElevatorPlatformVector.y > -0.4 && ElevatorPlatformVector.y < -0.3)
					ElevatorSpeed = 0.02;
				else
				if (ElevatorPlatformVector.y > 0.0 && ElevatorPlatformVector.y < 0.1)
					ElevatorSpeed = 0.03;
				else
				if (ElevatorPlatformVector.y > 0.8 && ElevatorPlatformVector.y < 0.9)
					ElevatorSpeed = 0.04;
				else
				if (ElevatorPlatformVector.y > 1.6 && ElevatorPlatformVector.y < 1.7)
					ElevatorSpeed = 0.05;
				else
				if (ElevatorPlatformVector.y > 70) //Max height
				{
					ElevatorStatus = 2;//Top Stop
					new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
					new SfxRef( DestructionTools:0x0036r ).stop( SFX_E_Loop_ID );  //end move sfx

					Elevator_Top();
				}

			}
			else
			if (ElevatorStatus == 3 ) // Move Down
			{	
				if (ElevatorPlatformVector.y > 69.7 && ElevatorPlatformVector.y < 69.8)// Heghts 69.5-69.6
					ElevatorSpeed = -0.03;
				else
				if (ElevatorPlatformVector.y > 69.3 && ElevatorPlatformVector.y < 69.4)// Heghts 69.0-69.1
					ElevatorSpeed = -0.04;
				else
				if (ElevatorPlatformVector.y > 68.5 && ElevatorPlatformVector.y < 68.6)// Heghts 68.5-68.6
					ElevatorSpeed = -0.05;
				else
			// Slowing down
				if (ElevatorPlatformVector.y > 3.0 && ElevatorPlatformVector.y < 3.1)// Heghts 3.0-3.1
					ElevatorSpeed = -0.04;
				else
				if (ElevatorPlatformVector.y > 2.5 && ElevatorPlatformVector.y < 2.6)// Heghts 2.5-2.6
					ElevatorSpeed = -0.03;
				else
				if (ElevatorPlatformVector.y > 2.0 && ElevatorPlatformVector.y < 2.1)// Heghts 2.0-2.1
					ElevatorSpeed = -0.02;
				else
				if (ElevatorPlatformVector.y > 1.5 && ElevatorPlatformVector.y < 1.6)// Heghts 1.5-1.6
					ElevatorSpeed = -0.01;
				else
				if (ElevatorPlatformVector.y <= Elevator_Pos.y) //Down positionm, Min height
				{
					ElevatorStatus = 0; // Turn OFF
					new SfxRef(DestructionTools:0x0090r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
					new SfxRef( DestructionTools:0x0036r ).stop( SFX_E_Loop_ID );  //end move sfx
					Level_Info.changeText( "" );
					Level_Info_Frame.finalize();
					Thread.sleep( 4000 *TimX ); // Wait 4s
					Lights = 0; // Turn OFF lights
					player.car.wakeUp();
				}
			}
			else
			if (ElevatorStatus == 4) // Move Down
			{
				if (ElevatorPlatformVector.y <= 64.0) //Stop at 64.0 meters
				{
					CarVector = player.car.getPos();
					if (CarVector.y > 40)
					{
						ElevatorStatus = 0; // Turn OFF
						new SfxRef(DestructionTools:0x0037r).play(ElevatorPlatformVector, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
						new SfxRef(DestructionTools:0x0037r).play(Elevator_Pos, 0.0, System.timeWarp(-1), 1.0, 0); // SFX Stop
						new SfxRef( DestructionTools:0x0036r ).stop( SFX_E_Loop_ID );  //end move sfx
						Thread.sleep( 1000 ); // Wait
						Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
						Player_Dialog_Text.changeText( "Slow Motion - Time: x0.015" );
						Player_Dialog_Text_Status = 2;
						PDTS_time = 3;
						System.timeWarp(0.015);
						Thread.sleep( 1000 ); // Wait
						ElevatorSpeed = 0.5;
						ElevatorStatus = 5;
						player.car.wakeUp();
					}
					else
					{
						ElevatorSpeed = -0.07;
						ElevatorStatus = 3;
					}
				}
			}
			else
			if (ElevatorStatus == 5) // Top spring
			{	
				if (ElevatorPlatformVector.y > 65.0 && ElevatorPlatformVector.y < 65.1)
					ElevatorSpeed = 1.0;
				else
				if (ElevatorPlatformVector.y > 65.5 && ElevatorPlatformVector.y < 65.6)
					ElevatorSpeed = 1.4;
				else
				if (ElevatorPlatformVector.y > 66.0 && ElevatorPlatformVector.y < 66.1)
					ElevatorSpeed = 2.3;
				else
				if (ElevatorPlatformVector.y > 67.0 && ElevatorPlatformVector.y < 67.1)
					ElevatorSpeed = 3.2;
				else
				if (ElevatorPlatformVector.y > 68.0 && ElevatorPlatformVector.y < 68.1)
					ElevatorSpeed = 4.1;
				else
				if (ElevatorPlatformVector.y > 69.0 && ElevatorPlatformVector.y < 69.1)
					ElevatorSpeed = 5.0;
				else
				if (ElevatorPlatformVector.y >= 70.0) //Top stop
				{
					ElevatorStatus = 0;
					Elevator_Top();
				}
			}
		}
		else
	// The Weight challenge section:
		if (WeightBornStatus == 1)
		{
			if (Frequency <= System.simTime())
			{
				Create_Weight();
			}

			if (Survived == SurvivedGoal)
			{
				WCH_Level += 1;
				
				if (Survived >= 175)
				{
					if (LastLevel == 0)
					{
						SurvivedGoal += 6;
						LastLevel = 1;

						if (Weight_Time >= 1.3) //Minimum frequency
							Weight_Time -= 0.3; // - 0.3 second

						Weight_Level_Text.changeText( "Level: " + WCH_Level );
						Weight_Survived_Text.changeText( "Survived: " + Survived );
						Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
						Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );

						Player_Dialog_Text.changeColor(0xFFFF0000); //Red
						Player_Dialog_Text.changeText( "You survived to the Last Level ( " + WCH_Level + " ) !" );
						new SfxRef(DestructionTools:0x0039r).play(); // SFX
					}
					else
					if (LastLevel == 1)
					{
						SurvivedGoal += 1;
						Weight_Survived_Text.changeText( "Survived: " + Survived );

						Player_Dialog_Text.changeColor(0xFFFF0000); //Red
						Player_Dialog_Text.changeText( "Only " + (275 - Survived) + " Weights left!");
						
						if (275 - Survived <= 0)
						{
							WeightBornStatus = 0; // stop chelenge

							enableOsd( 0 );
							if (cameraMode != CAMMODE_FOLLOW)
								changeCamFollow();

							Thread.sleep( 1500 *TimX ); //Wait
							Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
							Player_Dialog_Text.changeText( "Well Done! You finish The Weight Challenge!" );
							new SfxRef(Sound:0x0022r).play(); // SFX
							
							Thread.sleep( 2000 *TimX ); //Wait
							player.car.command( "brake" );
							cam.command( "angle 0 5.0 0.4853" );
							new SfxRef(Sound:0x0022r).play(); // SFX
							
							Thread.sleep( 10000 ); //Wait 10 seconds
							
							Weight_Text.changeText( "" );
							Weight_Level_Text.changeText( "" );
							Weight_Survived_Text.changeText( "" );
							Weight_Frequency_Text.changeText( "" );
							Weight_Distance_Text.changeText( "" );
							Info_Strip.finalize();
							
							Vector3 CamPos = player.car.getPos();
							cam.command( "move " + player.car.id() + " 10,20,0 3.0" );
							cam.command( "look " + map.id() + " " + CamPos.toString() + " 0,0,0" );

							Sound.changeMusicSet( Sound.MUSIC_SET_NONE );
							Player_Dialog_Text.changeText( "But don't think Your car can't be float!" );
							new SfxRef(DestructionTools:0x0039r).play(); // SFX

							Thread.sleep( 2000 ); //Wait

							CamPos.y += 20;
							Weight[Weight_Name].setMatrix( CamPos, new Ypr(Math.random()*3f, 0.0, 0.0) );
							Weight[Weight_Name].wakeUp();
							WeightSFXstatus = 1;
							
							if (Weight_Name >= 4) //Reset, possible names 0-4
							{
								Weight_Name = 0;
							}
							else
								Weight_Name += 1;

							Thread.sleep( 1200  *TimX); //Wait
							Vector3 CamVec = cam.getPos();
							cam.command( "move " + map.id() + " " + CamVec.toString() + " 0.0" );
							CamPos.y -= 20;
							cam.command( "look " + map.id() + " " + CamPos.toString() + " 0,0,0" );
							player.car.setMatrix( new Vector3( 101.3, 0.1, 153.2 ), new Ypr( 0.4, 0.0, 0.0) );

							if( nav )
								nav.remMarker( mPlayer );
							
							Thread.sleep( 2000 ); //Wait
							WeightSFXstatus = 0;
							player.car.wakeUp();
							player.car.command( "reset" );
							player.car.command( "setsteer -0.7" );
							PrimeWin = 1;
							prime = Vehicle.load( "DestructionTools/Scripts/Muscle", map );
							prime.setMatrix( new Vector3( 171.7, 0.1, 129.2 ), new Ypr( 2.1, 0.0, 0.0) );
							prime.wakeUp();
							prime.command( "reload" );
							prime.command( "setsteer -0.7" );

							Thread.sleep( 3000 ); //Wait
							Sound.changeMusicSet( Sound.MUSIC_SET_DRIVING );
							Player_Dialog_Text.changeText( "OK... it's only JOKE" );
							new SfxRef(DestructionTools:0x0039r).play(); // SFX
							
							//cam.command( "move " + map.id() + " 80.0, 6.0, 148.0 0.0" );
							cam.command( "look " + player.car.id() + " 0,0,0 0,0,0" );
							cam.setMatrix( new Vector3(80, 6, 148), new Ypr(-1.7, 0.0, 0.0) );
							cam.command( "zoom 15 0.5" );
							
							Thread.sleep( 7000 ); //Wait
							player.car.command( "start" );
							Player_Dialog_Text.changeText( "You win this Prime DHL 500 !!" );
							Player_Dialog_Text_Status = 2;
							PDTS_time = 6;
							new SfxRef(Sound:0x0022r).play(); // SFX
							enableOsd( 1 );
							player.controller.command( "leave " + player.car.id() );
							player.car.setParent( map );
							prime.setParent( player );
							prime.setParent( map );
							player.controller.command( "controllable " + prime.id() );

							changeCamTarget(prime);
							changeCamFollow();// cameraPos );

							map.command( "obs_add " + prime.id() );

							if (nav)
								mPlayer = nav.addMarker( player );
						}
					}

				}
				else
				if (Survived >= 115)
				{
					SurvivedGoal += 30; // Level 8-9

						if (Weight_Time >= 1.3) //Minimum frequency
							Weight_Time -= 0.3; // - 0.3 second

					Weight_Level_Text.changeText( "Level: " + WCH_Level );
					Weight_Survived_Text.changeText( "Survived: " + Survived );
					Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
					Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );

					Player_Dialog_Text.changeColor(0xFFFF8200); //Yellow-Red
					Player_Dialog_Text.changeText( "You advanced to Level " + WCH_Level + " !" );
					Player_Dialog_Text_Status = 2;
					PDTS_time = 6;
				}
				else
				if (Survived >= 40)
				{
					SurvivedGoal += 25; // Level 5-7

					if (Weight_Time >= 1.3) //Minimum frequency
						Weight_Time -= 0.3; // - 0.3 second

					Weight_Level_Text.changeText( "Level: " + WCH_Level );
					Weight_Survived_Text.changeText( "Survived: " + Survived );
					Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
					Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );

					Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
					Player_Dialog_Text.changeText( "You advanced to Level " + WCH_Level + " !" );
					Player_Dialog_Text_Status = 2;
					PDTS_time = 6;
				}
				else
				if (Survived == 25) // Level 4
				{
					SurvivedGoal += 15;

					if (Weight_Dist > 0) // Minimum initial distance
					Weight_Dist -= 3.0;

					if (Weight_Time >= 1.3) //Minimum frequency
						Weight_Time -= 0.3; // - 0.3 second

					Weight_Level_Text.changeText( "Level: " + WCH_Level );
					Weight_Survived_Text.changeText( "Survived: " + Survived );
					Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
					Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );

					Player_Dialog_Text.changeColor(0xFF82FF00); //Green-Yellow
					Player_Dialog_Text.changeText( "Level " + WCH_Level + " ! End of fun! Now Initial Distance is 0 m" );
					Player_Dialog_Text_Status = 2;
					PDTS_time = 8;
				}
				else
				{
					SurvivedGoal += 10; // Levels 2-3

					if (Weight_Time >= 1.3) //Minimum frequency
						Weight_Time -= 0.3; // - 0.3 second
					
					if (Weight_Dist > 0) // Minimum initial distance
						Weight_Dist -= 3.0; 

					Weight_Level_Text.changeText( "Level: " + WCH_Level );
					Weight_Survived_Text.changeText( "Survived: " + Survived );
					Weight_Frequency_Text.changeText( Float.toString(Weight_Time, "Frequency: %1.1f s") );
					Weight_Distance_Text.changeText( Float.toString(Weight_Dist, "Initial distance: %1.1f m") );
					
					Player_Dialog_Text.changeColor(0xFF00FF00); //Green
					Player_Dialog_Text.changeText( "You advanced to Level " + WCH_Level + " !" );
					Player_Dialog_Text_Status = 2;
					PDTS_time = 6;
				}
			}
			
			if (WeightBornStatus == 1)// check again
			{
				if (player.car.getSpeedSquare() < 126.5) // 40 km\h
				{	
					if (ToSlow == 0)
					{
						Player_Dialog_Text.changeColor(0xFFFF0000); //Red
						Player_Dialog_Text.changeText( "More speed!!" );
						Player_Dialog_Text_Status = 2;
						PDTS_time = 1;
						ToSlow = 1;
						ToSlowTime = System.simTime() + 3; //time + 3 seconds
					}
					else
					{
						Player_Dialog_Text.changeColor(0xFFFF0000); //Red
						Player_Dialog_Text.changeText( "More speed!!" );
						PDTS_time_start = System.simTime();
						Player_Dialog_Text_Status = 1;
						PDTS_time = 1;

						if (ToSlowTime <= System.simTime()) //Time up!
						{
							String problem = player.car.isDriveable();
							if( problem )
							{
								Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
								Player_Dialog_Text.changeText( "End of Challenge. You busted!" );
							}
							else
							{
								Player_Dialog_Text.changeColor(0xFFFFD600); //Yellow
								Player_Dialog_Text.changeText( "End of Challenge. To slow!" );
							}

							ToSlow = 0;
							Player_Dialog_Text_Status = 2;
							PDTS_time = 7;
							WeightBornStatus = 0;
							WeightSFXstatus = 0;
							Sound.changeMusicSet( Sound.MUSIC_SET_DRIVING );
							Thread.sleep( 7000 ); //Wait
							Weight_Text.changeText( "" );
							Weight_Level_Text.changeText( "" );
							Weight_Survived_Text.changeText( "" );
							Weight_Frequency_Text.changeText( "" );
							Weight_Distance_Text.changeText( "" );
							Info_Strip.finalize();
						}
					}

				}
				else
				if (player.car.getSpeedSquare() < 193.6) // 50 km\h
				{
					if (Player_Dialog_Text_Status == 0)
					{
						Player_Dialog_Text.changeColor(0xFFFFFF00); //Yellow
						Player_Dialog_Text.changeText( "More speed!" );
						Player_Dialog_Text_Status = 2;
						PDTS_time = 1;
						ToSlow = 0;
					}
					else
					if (Player_Dialog_Text_Status  > 0)
					{
						Player_Dialog_Text.changeColor(0xFFFFFF00); //Yellow
						Player_Dialog_Text.changeText( "More speed!" );
						PDTS_time_start = System.simTime();
						Player_Dialog_Text_Status = 1;
						PDTS_time = 1;
						ToSlow = 0;
					}
				}
			}
		}
		else
	// Default Tests section:
		if( actRound )
		{
			float speedSquare = player.car.getSpeedSquare();

			if( player.car.maxTestTrackSpeedSq < speedSquare )
				player.car.maxTestTrackSpeedSq = speedSquare;

			if( testStatus == 0 )
			{
				messageTextA.changeText( actRound.messageA );
				messageTextB.changeText( actRound.messageB );
				messageText0.changeText( "" );
				messageText1.changeText( "" );
				messageText2.changeText( "" );
				testStatus = 1;
			}
			if( testStatus == 1 )
			{
				if( speedSquare > 0.25 )
				{
					messageTextA.changeText( "" );
					messageTextB.changeText( "" );
					messageText0.changeText( "Time: " );
					testStartTime = System.simTime();
					testStatus = 2;
					Sound.changeMusicSet( Sound.MUSIC_SET_RACE );
				}
			}
			if( testStatus == 2 )
			{
				float	time = System.simTime() - testStartTime;
				messageText1.changeText( String.timeToString( time, String.TCF_NOHOURS ) + " s" );
				if( acceleration )
				{
					if( speedSquare >= MPH_60 )
					{
						messageText2.changeText( "Acceleration 0-100 KPH (0-62.1 MPH): " + String.timeToString( time, String.TCF_NOHOURS ) + " s" );
						acceleration = 0;
						if( player.car.bestTestTrackAcc > time || player.car.bestTestTrackAcc < 0.10 )
							player.car.bestTestTrackAcc = time;
					}
				}
				if( acceleration120 )
				{
					if( speedSquare >= MPH_124 )
					{
						messageText2.changeText( "Acceleration 0-200 KPH (0-124.2 MPH): " + String.timeToString( time, String.TCF_NOHOURS ) + " s" );
						acceleration120 = 0;
						if( player.car.bestTestTrackAcc120 > time || player.car.bestTestTrackAcc120 < 0.10 )
							player.car.bestTestTrackAcc120 = time;
					}
				}
				if( activeTrigger )
				{
					Trigger tr = actRound.trigger.elementAt( triggerIndex );
					if( activeTrigger == tr.trigger.id() )
					{
						if( triggerIndex == 1 ) // 402 m //
						{
							if( actRound.type == 0 )
							{
								messageText2.changeText( "1/4 mile time: " + String.timeToString( time, String.TCF_NOHOURS ) + " s" );
								if( player.car.bestTestTrackTime2 > time || player.car.bestTestTrackTime2 < 0.10 )
								{
									player.car.bestTestTrackTime2_speedSq = speedSquare;
									player.car.bestTestTrackTime2 = time;
								}
							}
						}
						triggerIndex++;
						if( triggerIndex == actRound.trigger.size() )
						{
							if( actRound.type == 0 )
							{
								messageText2.changeText( "1 mile time: " + String.timeToString( time, String.TCF_NOHOURS ) + " s" );
								if( player.car.bestTestTrackTime0 > time || player.car.bestTestTrackTime0 < 0.10 )
								{
									player.car.bestTestTrackTime0_speedSq = speedSquare;
									player.car.bestTestTrackTime0 = time;
								}
							}
							else
							{
								if( player.car.bestTestTrackTime1 > time || player.car.bestTestTrackTime1 < 0.10 )
									player.car.bestTestTrackTime1 = time;
							}
							actRound = null;
							Sound.changeMusicSet( Sound.MUSIC_SET_DRIVING );
						}
					}
				}
			}
		}
	}
}


