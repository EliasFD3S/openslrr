package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class Ledz extends Gadget
{
	static ResourceRef	texOn = new ResourceRef( frontend:0x012Br );
	static ResourceRef	texOff= new ResourceRef( frontend:0x00A2r );
	static ResourceRef	texDsb= new ResourceRef( frontend:0x00A2r );

	Hotkey		hkl, hkr;

	Rectangle[]	led;
	Vector3[]		l_end_pos;
	Vector3[]		l_start_pos;

	public	int		value;
	int		offset;
	int		noff;	//nem lehet csupa sotet allasa (0=1 vilagit)
	int		lim;	//a max valaszthato ertek

	public Ledz( Osd o, Style sty, float px, float py, int pri, String string, int command, int v, int max )
	{
		super( o, sty, px, py, pri, string, command );

		//
		{
			Group g = osd.groups.lastElement();

			w = sty.width*sty.aspect*sty.aspect/osd.vpAspect;

			switch( sty.align )
			{
			case (Text.ALIGN_LEFT):
				x = px+0.5*w;
				break;
			case (Text.ALIGN_RIGHT):
				x = px-0.5*w;
				break;
			default:
				x=px;
			}

			y = py;

			this.pri = pri;

			Vector3 pos = g.convertTextCoordinates( x-0.5*w +0.5*sty.height*0.7  , y, pri );

			RectangleTemplate rt = new RectangleTemplate( sty.rt.height, sty.rt.height, Osd.RRT_NONE );

			if( max<0)
			{
				max=-max;
				max++;
				noff=1;
			}

			led = new Rectangle[max-1];
			l_end_pos = new Vector3[max-1];
			l_start_pos = new Vector3[max-1];
			for( int i=0; i<led.length; i++ )
			{
				led[i] =  new Rectangle( g, new Vector3(pos), rt );
				l_end_pos[i] = new Vector3(pos);
				l_start_pos[i] = new Vector3(pos);

				if ( l_end_pos[i].x < 0.0 )
					l_start_pos[i].x = l_end_pos[i].x - 2.0;
				else
					l_start_pos[i].x = l_end_pos[i].x + 2.0;

				l_start_pos[i].y = l_end_pos[i].y;
				l_start_pos[i].z = l_end_pos[i].z;

				pos.x+=sty.rt.height*0.9;
			}
		}
		//

		setValue( v );
	}

	public Ledz limit( int level )
	{
		lim = level;
		setValue( value );
		return this;
	}

	public void focus()
	{
		super.focus();

		hkl = osd.createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_DEC|Osd.PRIVATEEVENT );
		hkr = osd.createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_INC|Osd.PRIVATEEVENT );
	}
	public void unfocus()
	{
		super.unfocus();

		osd.hotkey.removeElement( hkl ); hkl.inactivate(); hkl=null;
		osd.hotkey.removeElement( hkr ); hkr.inactivate(); hkr=null;
	}

	public void increase()
	{
		setValue( value+1 );
		sendEvent(Event.F_GAD_CHANGE);
	}

	public void decrease()
	{
		setValue( value-1 );
		sendEvent(Event.F_GAD_CHANGE);
	}

	public void click()
	{
		int cmp = value;

		setValue( value+1 );

		if( cmp == value )
		{
			setValue( offset );
		}
		sendEvent(Event.F_GAD_LCLICK);
	}

	public void setValue( int v )
	{
		if( v > led.length+offset-noff )
			value = led.length+offset-noff;
 		else
		if( v < offset )
			value = offset;
		else
			value = v;


		if( lim && value > lim )
			value = lim;

		if ( value < 0 )
			value = 0;

		for( int i=0; i<led.length; i++ )
			led[i].changeTexture( 
				( (lim && i>=lim+noff) ?
					(texDsb) :
					( (i>=value-offset+noff) ? 
						(texOff) : 
						(texOn) 
					) 
				)
			);
	}

	public void animate(float dt)
	{
		if ( animate_running == 0 )
			return;

		super.animate(dt);

		float				time = ((float)(animate_length-animate_timer))/animate_length;

//		time = Math.sqrt(Math.sqrt(time));

		for( int i=0; i<led.length; i++ )
		{
			float			x = time*l_end_pos[i].x + (1.0-time)*l_start_pos[i].x;
			float			y = time*l_end_pos[i].y + (1.0-time)*l_start_pos[i].y;
			float			z = time*l_end_pos[i].z + (1.0-time)*l_start_pos[i].z;

			Vector3		p = new Vector3(x, y, z);
			led[i].setPos(p);
			led[i].setPos(p);
		}
	}
}
