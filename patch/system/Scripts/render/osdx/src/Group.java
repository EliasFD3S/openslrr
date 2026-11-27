package java.render.osdx;

import java.render.*;
import java.io.*;
import java.sound.*;
import java.util.*;
import java.util.resource.*;


public class Group extends Dummy
{
	Osd	osd;
	int	active;

	public	Vector	hotkey = new Vector();	//for group hotkeys only
	public	Vector	gadget = new Vector();
	public	Vector	misc = new Vector();	//text, textbox, rectangle, renderobject store

	int		hasFocusedGadget;
	int		timerCnt;

	public final static int HIDEKILL = 0x00000001;	//ha bezarodik, meg is szunik
	public final static int AUTOHIDE = 0x00000002;	//ha osszes eleme elveszti a fokuszt, magatol becsukodik

	int		flags;

	public Group( Osd o )
	{
		super( o/*, WORLDTREEROOT*/ );	//a group resource nodejat az osd node alatt hozzuk letre
		osd = o;

		activate();
	}

	public void finalize()
	{
		while( !hotkey.isEmpty() )
		{
			Hotkey hk =  hotkey.elementAt( 0 );
			hotkey.removeElementAt( 0 );
			hk.inactivate();
		}

		super.finalize();
	}

	public Vector3 convertTextCoordinates( float x, float y, int pri )
	{
		return osd.convertTextCoordinates( x, y, pri );
	}

	public void activate()
	{
		if( !active )
		{
			active = 1;

			clearFlags( WORLDTREELEAF );

			if( osd.iLevel >= Osd.IL_TIPS )
			{
				//time a sajat timer miatt kell!
				setEventMask( EVENT_CURSOR|EVENT_TIME );

				for( int i=0; i<hotkey.size(); i++ )
				{
					Hotkey hk =  hotkey.elementAt( i );
					hk.activate();
				}
			}
		}
	}

	public void deactivate()
	{
		if( active )
		{
			active = 0;

			setFlags( WORLDTREELEAF );

			if( osd.iLevel >= Osd.IL_TIPS )
			{
				clearEventMask( EVENT_CURSOR|EVENT_TIME );

				if( osd.pointedGadget && findGadget(osd.pointedGadget.phy.id()) )
					osd.unFocusGadget();

				for( int i=0; i<hotkey.size(); i++ )
				{
					Hotkey hk =  hotkey.elementAt( i );
					hk.inactivate();
				}
			}

			if( flags & HIDEKILL )	//csak addig el, amig el nem hideolodik
				osd.remGroup( hashCode() );
		}
	}

	public Gadget findGadget( int physicsId )
	{
		int i=gadget.size();
		Gadget g;

		while( i-- )
		{
			g = gadget.elementAt(i);
			if( g.phy && g.phy.id() == physicsId )	//pl a progressbarnak nincs phy-je
			{
				return g;
			}
		}

		return null;
	}

	public Gadget findGadgetEx( int physicsId )
	{
		GameRef parent;
		Object	instance;
		if( (parent = new ResourceRef( physicsId ).getParent()) && (instance = parent.getScriptInstance()) && instance instanceof Group )
		{
			return ((Group)instance).findGadget( physicsId );
		}

		return null;
	}

	//megszuri az inputot (lenyomom, kozben megnyilik egy modal, felengedem, click()! hiba)
	public int checkModality()
	{
		//igazabol az aktualis es a legutolso kozt mindet vegig kellene nezni, van-e egyetlen modal is!
		return osd.isFocused() || (Frontend.inputQueue.lastElement() && !((Osd)Frontend.inputQueue.lastElement()).flags&Osd.OF_MODAL) ;
	}

	public void handleEvent(GameRef obj_ref, int event, String param)
	{
		int	ec = param.token(0).intValue();

		osd.lastUserActivityTimeStamp = System.currentTime();

		//button click kezeles:
		if (ec == EC_LDOWN)
		{
			int	physicsId = param.token(3).intValue();

			Gadget g = findGadget( physicsId );
	
			if( g == osd.pointedGadget || g.parent == osd.pointedGadget)//pl:versenynaptar: egerrel raallunk, kormannyal melleallunk, egerclick!@$@#$
			{
				if( g.parent )
					g = g.parent;

				if( g )
					if( !g.nofocus )
						if( !g.disabled )
						{
							Gadget ag = osd.actGadget;
							if( g != ag )	//ha ua, hagyjuk mukodni
							{
								if( ag )	//StringInput, etc lezarasa!
								{
									ag.stop();
									if( osd.actGadget )
										System.exit( "JVM/Frontend: actGadget left open" );

									ag.sendEvent(Event.F_GAD_LCLICK);
								}

								g.press();

								if( g.noleaverelease )
									osd.clickLock = 1;

								osd.actDown = g;

								new SfxRef( Frontend.SFX_MENU_SELECT ).play(); 
							}
						}
						else
							new SfxRef( Frontend.SFX_WARNING ).play(); 

			}
		}
		else
		if (ec == EC_LUP)
		{
			int	physicsId = param.token(3).intValue();

			Gadget g = findGadget( physicsId );
	
			if( g == osd.pointedGadget || g.parent == osd.pointedGadget)//pl:versenynaptar: egerrel raallunk, kormannyal melleallunk, egerclick!@$@#$
			{
				if( g.parent )
					g = g.parent;

				if( g )
				{
					g.release();

					osd.clickLock = 0;
					osd.actDown = null;
				}
			}
		}
		else
		if (ec == EC_LCLICK)
		{
			int	physicsId = param.token(2).intValue();

			Gadget g = findGadget( physicsId );

			if( g == osd.pointedGadget || g.parent == osd.pointedGadget)//pl:versenynaptar: egerrel raallunk, kormannyal melleallunk, egerclick!@$@#$
			{
				if( g.parent )
					g = g.parent;

				if( g && !g.disabled )
				{
					if( checkModality() )
					{
						g.click();	//CLICK->Press
					}
				}
			}
		}
		else
		if (ec == EC_LDROP)
		{
			int	physicsId = param.token(2).intValue();
			int	physicsIdDragged = param.token(4).intValue();

			Gadget g = findGadget( physicsId );

			if( g.parent )
				g = g.parent;

			if( g && !g.disabled )
			{
				if( physicsId == physicsIdDragged )	//onmagat "dropoltak" ra
				{
					if( checkModality() )
					{
						g.click();	//CLICK->Press
					}
				}
				else
				{
					Gadget dg = findGadgetEx( physicsIdDragged );
		
					if( g.dropEnabled )
					{
						if( dg )
						{
							g.drop( dg );
						}
						else
						{
							int gameInstanceId = param.token(3).intValue();
							g.drop( new GameRef( gameInstanceId ) );
						}
					}
				}
			}
		}
		else
		if( !osd.clickLock )	//lasd noleaverelease
		{
			//button mouse_over hilite kezeles
			if( ec == EC_HOVER )
			{
				int	moved = param.token(4).intValue();

				//if( moved )	//felig oldja meg a problemat, mert legkozelebb ha megmozdul, mar nem kuld meg egy hovert..
							//mozdulast pedig nem figyelunk, tul sokat lassitana! :(

				if( !osd.controlMode || moved )
				{
					int	physicsId = param.token(3).intValue();

					Gadget g = findGadget( physicsId );

					if( g )
					{
						if( g.parent )
							g=g.parent;

						hasFocusedGadget=1;

						if( g!=osd.pointedGadget && !g.nofocus && (!g.disabled || g.highliteMethod == Osd.HL_BACKLIGHT) )
						{
							osd.unFocusGadget();

							osd.focusGadget( this, g );
							osd.hoverEffect();
							new SfxRef( Frontend.SFX_MENU_MOVE ).play();

							if( osd.actDown == g )
							{
								g.press();
							}
						}


						osd.controlMode = 0;	//billentyus kontroll OFF!
					}
				}
			}
			else
			if( ec == EC_LEAVE )
			{
				int	physicsId = param.token(3).intValue();

				Gadget g = findGadget( physicsId );

				if( g )
				{
					if( g.parent )
						g=g.parent;

					if( g==osd.pointedGadget )
					{
						g.release();

						osd.unFocusGadget();
						osd.controlMode = 0;	//billentyus kontroll OFF!
					}

					hasFocusedGadget=0;

					if( flags & AUTOHIDE )	//unfocus all elements -> hide
					{
						addTimer( 0, 0 );
						timerCnt++;
					}
				}
			}
		}
	}

	public void handleEvent(GameRef obj_ref, int event, int param)
	{
		if( !--timerCnt )
			if( !hasFocusedGadget )
			{
				deactivate();
			}
	}

	//osd show/hide eseten hivodik
	public void show(){ /*activate();*/ }	//tobbet ront mint hasznal(jo: mainmenu felbontasvaltas, rossz: minde group elojon show-ra)
	public void hide(){ /*deactivate();*/ }

	public void animate(float dt)
	{
		for( int i=gadget.size()-1; i>=0; i-- )
			((Gadget)gadget.elementAt(i)).animate(dt);
	}
}
