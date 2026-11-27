package java.render.osdx.dialog;

import java.render.osdx.*;
import java.render.osdx.Osd;
import java.render.*;
import java.io.*;
import java.sound.*;
import java.util.*;
import java.util.resource.*;


public class Dialog extends GameType implements EventReceiver
{
	public	final static ResourceRef RRT_DIALOGBG	= new ResourceRef( frontend:0x000Er );
	
	final static float DEF_SIZE		= 0.6;
	final static float DEF_ASPECT	= 4.0/3.0;
	final static float DEF_BODYRATIO= 0.70;

	public	final static int DF_MODAL		= 0x00000001;
	public	final static int DF_FULLSCREEN	= 0x00000002;	//otherwise defaults to 60%
	public	final static int DF_DEFAULTBG	= 0x00000004;	//uses a default backgroung image
	public	final static int DF_FREEZE		= 0x00000008;	//stop physics
	public	final static int DF_LEAVEPOINTER= 0x00000010;	//do not alter the pointer's state (otherwise it'll be turned on)
	public	final static int DF_LOWPRI		= 0x00000020;	//ha tobb dalogus lehet kint, ezzel lehet megadni a sorrendet
	public	final static int DF_HIGHPRI		= 0x00000040;	//ha tobb dalogus lehet kint, ezzel lehet megadni a sorrendet
	public	final static int DF_WIDE		= 0x00000080;	//szeles, nem fullscreen dialogus
	public	final static int DF_DARKEN		= 0x00000100;	//tesz a hatterbe egy sotetitett alfas lapot
	public	final static int DF_SILENT		= 0x00010000;	//a show() nem jatssza le a default hangot
	public	final static int DF_LOADINGPRI	= 0x00000400;	//loadingscreen: 'topmost' priority

	public	final static int SIF_NOEMPTY	= 0x00001000;	//StringRequester - no empty strings allowed
	public	final static int FRF_SAVE		= 0x00001000;	//FileRequester - save mode (owerwrite question)

	int				priority = 22;	//low: 19/20, normal: 21/22; highr: 23/24
	public	Osd				osd;
	public	int				flags;
	String			title;
	String			buttonTexts;
	float			timewarp;

	int				result;
	int				escapeCmd = -1;

	Controller		controller;
	ControlSetState	css;

	public	int				prevPointerState;

	Osd				backShieldOsd;

	float			bodyRatio = DEF_BODYRATIO;
	
	public	int				shown;

	private Dialog()
	{
		System.exit( "private constructor access (Dialog)" );
	}

	public Dialog( Controller ctrl, int myFlags, String myTitle, String myButtons )
	{
		this( ctrl, myFlags, myTitle, myButtons, DEF_SIZE, DEF_ASPECT, 0.5, 0.5, -0.5, -0.5 );
	}

	public Dialog( Controller ctrl, int myFlags, String myTitle, String myButtons, float size, float aspect )
	{
		this( ctrl, myFlags, myTitle, myButtons, size, aspect, 0.5, 0.5, -0.5, -0.5 );
	}

	public Dialog( Controller ctrl, int myFlags, String myTitle, String myButtons, float size, float aspect, float cx, float cy, float refx, float refy )
	{
		createNativeInstance();

		controller = ctrl;
		flags=myFlags;
		title=myTitle;
		buttonTexts=myButtons;

		if (flags&DF_LOADINGPRI)
			priority = 100;

		if( flags&DF_HIGHPRI )
			priority+=2;
		else
		if( flags&DF_LOWPRI )
			priority-=2;

		if( flags&DF_FULLSCREEN )
			osd = new Osd( 1.0, 0.0, priority );
		else
			if( flags&DF_WIDE )
				osd = new Osd( 0.9, 6.0/3.0, priority );
			else
				osd = new Osd( size, aspect, cx, cy, refx, refy, priority );

		if( flags&DF_DEFAULTBG )
		{
			//osd.createBG( RRT_DIALOGBG );
			osd.createWindowBG();
		}

		if( title )
			osd.createText( title, Frontend.mediumFont, Text.ALIGN_CENTER,	0.0, 2*((1-bodyRatio)/4)-1.0 ).changeColor( 0xFFFFae00 );

		osd.setEventHandler( this );
	}

	public void show()
	{
		show(Osd.FADE_OFF, 0.25);
	}

	public void show(int fade, float time)
	{
//		System.profileStartTimer("java_Dialog_show");
//		System.profileStartTimer("java_Dialog_show_1");
//		fade = Osd.FADE_OFF;

		shown=1;

		//FLAG: DF_FREEZE
		if( flags&DF_FREEZE )
			timewarp = System.timeWarp(0.0);

		Osd	shieldHolderOsd;
		if( flags&DF_MODAL )
		{
			backShieldOsd = new Osd( 1.0, 0.0, priority-1 );
			backShieldOsd.iLevel = Osd.IL_NONE;
//			backShieldOsd.show(fade,time);
			backShieldOsd.show(Osd.FADE_OFF,0.0);

			shieldHolderOsd = backShieldOsd;
			osd.flags|=Osd.OF_MODAL;
		}
		else
		{
			shieldHolderOsd = osd;
		}

//		System.profileStopTimer("java_Dialog_show_1");
//		System.profileStartTimer("java_Dialog_show_2");

		ResourceRef bgButtonImage;

		if( flags&DF_DARKEN )
			bgButtonImage = Osd.RRT_WHITEALPHA;

		Style shieldButtonStyle = new Style( 2.0, 2.0, shieldHolderOsd.vpAspect, Frontend.mediumFont, Text.ALIGN_CENTER, bgButtonImage );
		shieldHolderOsd.createButton( shieldButtonStyle, 0.0, 0.0, -2, " ", 9999999 ).nofocus = 1;


//		System.profileStopTimer("java_Dialog_show_2");
//		System.profileStartTimer("java_Dialog_show_3");

		//SET UP BUTTONS
		float x,y;

		StringTokenizer tok = new StringTokenizer( buttonTexts, ";" );
		int buttons = tok.countTokens();

		if( buttons )
		{
//			Style buttonStyle = new Style( 0.3, 0.1, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_TEST );
			
			Style LbuttonStyle = new Style( 0.35, 0.09, osd.vpAspect, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef(frontend:0x00D8r) );
			LbuttonStyle.customHL( new ResourceRef(frontend:0x012Br) );
			Style RbuttonStyle = new Style( 0.35, 0.09, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef(frontend:0x00D8r) );
			RbuttonStyle.customHL( new ResourceRef(frontend:0x012Br) );

			//float bw = 0.3 * (RbuttonStyle.aspect*RbuttonStyle.aspect/osd.vpAspect);
			float bw = 0.34 / osd.vpWidth; // *LbuttonStyle.aspect*LbuttonStyle.aspect/osd.vpAspect; // * osd.vpAspect * osd.vpAspect;

			//button = new Button[buttons];
			//float step = - bw + 0.1); //2.0/(buttons+1);

			//y = 2*(bodyRatio+((1-bodyRatio)/osd.vpAspect))-1.0;
			y = 1.0 - 0.09 / osd.vpHeight;
			
			x = 0.0 - (buttons-1) * bw/2.0;
			for( int i=0; i<buttons; i++ )
			{
//				/*button[i] =*/ osd.createButton( buttonStyle, x, y, tok.nextToken(), i );
		
				if ( i%2 == 0 )
				{
					if (i>=2)
						x+=0.03;

					osd.createButton( LbuttonStyle, x, y, tok.nextToken(), i );
				}
				else
					osd.createButton( RbuttonStyle, x, y, tok.nextToken(), i );
				
				x+=bw;
			}

			if( escapeCmd < 0 )
				escapeCmd = osd.defSelection;

			osd.orientation = 1;
			osd.createHotkey( Input.AXIS_CANCEL, Input.VIRTUAL, escapeCmd );
		}

//		System.profileStopTimer("java_Dialog_show_3");
//		System.profileStartTimer("java_Dialog_show_4");

		if( !flags&DF_SILENT )
			new SfxRef( Frontend.SFX_QUESTION ).play(); 

//		if( flags&DF_MODAL )

		osd.show(Osd.FADE_OFF,time);
//D		if (backShieldOsd && (shieldHolderOsd != backShieldOsd))
//D			backShieldOsd.show(fade,time);

//		else
//			osd.show(FADE_OFF,0.0);

//		System.profileStopTimer("java_Dialog_show_4");
//		System.profileStartTimer("java_Dialog_show_5");

		if( controller )
		{
			css=controller.reset( new ControlSetState(ControlSet.MENUSET) );
		}

		if( !(flags&DF_LEAVEPOINTER) )
			prevPointerState = Input.cursor.enable(1);


		setEventMask( EVENT_CURSOR|EVENT_COMMAND );
//		System.profileStopTimer("java_Dialog_show_5");
//		System.profileStopTimer("java_Dialog_show");
	}

	public void hide()
	{
		shown=0;

		clearEventMask( EVENT_ANY );

		if( !(flags&DF_LEAVEPOINTER) )
			Input.cursor.enable(prevPointerState);

		if( controller )
			controller.reset( css );
		
		osd.hide();

		if( backShieldOsd )
		{
			backShieldOsd.hide();
			backShieldOsd = null;
		}

		if( flags&DF_FREEZE )
			System.timeWarp(timewarp);
	}

	public void	handleEvent( Event e )
	{
		result=e.cmd;
		notify();
	}

	public int display()
	{
		if( !shown )
			show();	//show(), de csak ha meg nem volt!

		wait();
		hide();
		return result;
	}
		
}

