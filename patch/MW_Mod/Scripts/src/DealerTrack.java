package java.game;

import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.render.*;	//Text
import java.render.osd.*;	//Text
import java.sound.*;
import java.render.osd.dialog.*; //Dialog Window

import java.game.parts.*;
import java.game.parts.enginepart.*;

public class DealerTrack extends Track
{	
	final static String MapName = "Car Dealer";
	final static String MapDescription = "\n \n \n It is default game's Car Dealer. \n \n It have some bugs, but member it was created for car shoping, not for riding...";
	final static String MapAuthor = "Authors: Invictus & Miran";
	final static ResourceRef MapImage = new ResourceRef(MW_Mod:0x0046r);
	final static float	TS_Version = 1.0;
	
	RenderRef		stuff;
	PhysicsRef		RightWall = new PhysicsRef(), LeftWall = new PhysicsRef(), FrontWall = new PhysicsRef();

	Osd				osd2;
	Text			carName, DialogText;
	int				EmptyPlace = 7, CurCar = 7, NewCar;
	Pori[]			CarPos = new Pori[7];
	Vehicle[]		Car = new Vehicle[7];
	final static int option = 0;
	final static int	CMD_CHANGE = 30;

	public DealerTrack()
	{
		posStart = new Vector3( 10.922, 0.000, -11.930 );
		oriStart = new Ypr( -2.923, 0.000, 0.000 );

		map = new GroundRef( misc.dealer:0x0001r );
	}

	public void StartMap()
	{
		option = 0;

		new Car_Dealer_Options().display();
	}

	public void enter( GameState prev_state )
	{
		float key = Math.random()*100;
		if( MainMenu.Check(key) != Math.sqrt(key)+3.75 ) Mirans_Mod_protection_error______Please_Resintal_Mirans_Mod();

		Frontend.loadingScreen.show(new ResourceRef(frontend:0x00CAr));	
		GfxEngine.flush();

		osd2 = new Osd();
		osd2.createHeader( "Car Dealer" );
		int price = player.car.getTotalPrice();
		carName=osd2.createText( "YOUR CAR  $" + price,	Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, -0.98 );
		DialogText = osd2.createText( "", Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, -0.7 );	
		osd2.show();
		
		stuff = new RenderRef( map, misc.dealer:0x000Fr, null );

		RightWall.createBox( map, 0.005, 4.0, 8.4, "Right Window Physic" );
		RightWall.setMatrix( new Vector3(22.0, 2.0, -4.1), null );

		LeftWall.createBox( map, 0.005, 4.0, 8.4, "Left Window Physic" );
		LeftWall.setMatrix( new Vector3(0.0, 2.0, -4.1), null );

		FrontWall.createBox( map, 22.0, 4.0, 0.005, "Left Window Physic" );
		FrontWall.setMatrix( new Vector3(11.0, 2.0, 0.0), null );
		
		if(option == 1) CreateCars();

		super.enter( prev_state );

		//osd2.globalHandler = this;
		osd.createHotkey( Input.RCDIK_ENTER, Input.KEY|Osd.HK_STATIC, CMD_CHANGE, this );
		
		suntype.setLight( 0x00DDD8AD, 0x004B5E6F, 0x00DDD8AD);
		GfxEngine.setGlobalEnvmap( new ResourceRef(maps.skydome:0x003Ar) );

		DialogText.changeColor(0xFFFFD600); //Yellow
		Thread.sleep( 1000 ); // Wait
		new SfxRef(DestructionTools:0x0039r).play(); // SFX
		DialogText.changeText( "Press ENTER to change car" );
		Thread.sleep( 4000 ); // Wait
		DialogText.changeText( "" );
	}

	public void CreateCars()
	{
		CarPos[0] = new Pori( new Vector3( 3.901, 0.000, -3.499 ), new Ypr( -1.963, 0.000, 0.000 ) );
		CarPos[1] = new Pori( new Vector3( 3.901, 0.000, -7.603 ), new Ypr( -1.963, 0.000, 0.000 ) );
		CarPos[2] = new Pori( new Vector3( 5.099, 0.000, -12.927 ), new Ypr( -1.876, 0.000, 0.000 ) );
		CarPos[3] = new Pori( new Vector3( 10.922, 0.000, -11.930 ), new Ypr( -2.923, 0.000, 0.000 ) );
		CarPos[4] = new Pori( new Vector3( 17.295, 0.000, -13.173 ), new Ypr( 1.981, 0.000, 0.000 ) );
		CarPos[5] = new Pori( new Vector3( 17.226, 0.000, -7.603 ), new Ypr( 1.981, 0.000, 0.000 ) );
		CarPos[6] = new Pori( new Vector3( 17.226, 0.000, -3.267 ), new Ypr( 1.981, 0.000, 0.000 ) );

		for( int i = 0; i<Car.length; i++ )
		{
			
			if( !Car[i] && Math.random() < 0.4 )
			{
				VehicleDescriptor vd = GameLogic.getVehicleDescriptor( VehicleType.VS_STOCK );
				Car[i] = new Vehicle( map, vd.id, vd.colorIndex, 1, 1, 1, 1 );
			
				Car[i].setMatrix( new Vector3(CarPos[i].pos), new Ypr(CarPos[i].ori) );
				Car[i].command( "reset" );
				Car[i].wakeUp();
				Car[i].command( "setsteer " + Math.randomRange(-0.5, 0.5) ); // SetSteer -0.5 - 0.5 (OpenSLRR #465)
			}
			else
			{
				Car[i] = null;
				EmptyPlace = i; //Save empty place for player.car
			}
		}

		if (EmptyPlace < 7)
		{
			posStart = new Vector3( CarPos[EmptyPlace].pos );
			oriStart = new Ypr( CarPos[EmptyPlace].ori );		
		}
		else // Dealer full of cars, make empty place for player
		{
			int DeleteCar = Math.randomInt(Car.length);	// OpenSLRR (#465): the last place too

			Car[DeleteCar].destroy();

			posStart = new Vector3( CarPos[DeleteCar].pos );
			oriStart = new Ypr( CarPos[DeleteCar].ori );
		}
	}

	public void osdCommand( int cmd )
	{
		if( cmd == CMD_CHANGE )
		{
			if(option == 1)
			{
				int Loop = 1;
				NewCar = CurCar;

				while( Loop == 1 )
				{
					if( NewCar >= Car.length )
						NewCar = 0;
					else
						NewCar++;
					
					if( NewCar >= Car.length )
					{
						if (player.car)
							Loop = 0;
					}
					else
					{
						if ( Car[NewCar] )
							Loop = 0;
					}
				}
		
				changeCamFollow();

				if( NewCar >= Car.length )
				{
					if (CurCar >= Car.length) //if old car = player.car
						player.controller.command( "leave " + player.car.id() );
					else
						player.controller.command( "leave " + Car[CurCar].id() );

					player.controller.command( "controllable " + player.car.id() );
					changeCamTarget(player.car);

					int price = player.car.getTotalPrice();
					carName.changeText( "YOUR CAR  $" + price );
				}
				else
				{	
					if (CurCar >= Car.length) //if old car = player.car
						player.controller.command( "leave " + player.car.id() );
					else
						player.controller.command( "leave " + Car[CurCar].id() );

					player.controller.command( "controllable " + Car[NewCar].id() );
					changeCamTarget(Car[NewCar]);
					
					int price = Car[NewCar].getTotalPrice()*1.3;
					carName.changeText( Car[NewCar].toString() + "  $" + price );
				}


				CurCar = NewCar;
			}
		}
		else
		{
			super.osdCommand( cmd );
		}
	}

	public void exit( GameState next_state )
	{
		stuff.destroy();
		RightWall.destroy();
		LeftWall.destroy();
		FrontWall.destroy();

		osd2.hide();
		osd2.finalize();

		for( int i = 0; i<Car.length; i++ )
		{	
			if(Car[i])
				Car[i].destroy();
		}

		super.exit( next_state );
	}
}

public class Car_Dealer_Options extends Dialog
{
	final static int	CMD_BACK = 15;
	final static int	CMD_OK	 = 16;
	final static int	CMD_OPT1 = 20;
	final static int	CMD_OPT2 = 21;

	Menu	m;
	Style	Menu_Center, Menu_Left;
	Text	CurrentOption;

	public Car_Dealer_Options()
	{
		super( GameLogic.player.controller, DF_MODAL|DF_DEFAULTBG|DF_HIGHPRI|DF_DARKEN, "TRACK OPTIONS", "" );
		
		Menu_Center = new Style( 0.50, 0.13, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_TEST );
		Menu_Left = new Style( 0.90, 0.13, Frontend.mediumFont, Text.ALIGN_LEFT, Osd.RRT_TEST );
		
		m = osd.createMenu( Menu_Left, -0.6, -0.5, 0, Osd.MD_VERTICAL );
		m.addItem( "1. Empty Car Dealer", CMD_OPT1, "Only your car on the track" );
		m.addItem( "2. Other cars in Car Dealer", CMD_OPT2, "Press ENTER to change car" );

		CurrentOption = osd.createText( "Chosen option: 1" , Frontend.largeFont, Text.ALIGN_CENTER, 0.0, 0.65 ); CurrentOption.changeColor(0xFFC0C0C0);// "Unselected" gray
		
		m = osd.createMenu( Menu_Center, -0.4, 0.9, 0, Osd.MD_HORIZONTAL );
		m.addItem( "OK", CMD_OK );
		
		m = osd.createMenu( Menu_Center, 0.4, 0.9, 0, Osd.MD_HORIZONTAL );
		m.addItem( "BACK", CMD_BACK );

		super.show();
	}
		
	public void osdCommand( int cmd )
	{
		if( cmd == CMD_OPT1 )
		{
			DealerTrack.option = 0;
			CurrentOption.changeText("Chosen option: 1");
		}
		else
		if( cmd == CMD_OPT2 )
		{
			DealerTrack.option = 1;
			CurrentOption.changeText("Chosen option: 2");
		}
		else
		if( cmd == CMD_BACK )
		{
			osd.hide();
			super.hide();
		}
		else
		if( cmd == CMD_OK )
		{
			osd.hide();
			super.hide();
			GameLogic.changeActiveSection( new DealerTrack() );
		}
	}
}