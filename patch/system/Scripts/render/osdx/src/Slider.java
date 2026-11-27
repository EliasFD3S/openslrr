package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.resource.*;
import java.util.*;

public class Slider extends Gadget
{
	Hotkey		hkl, hkr;

	float		x0, x1;
	public	float		value;

	float		a=0.0, b=1.0;	//range bounds
	int			ticks;			//0 - unlimited

	Label	vLabel;	//extra label for displayeing the value

	String	vFmt;
	float	scale;

	Button	button;

	public Slider( Osd o, Style sty, Style ksty, float x, float y, int pri, String string, int command, float v )
	{
		super( o, sty, x, y, pri, string, command );

		float kwidth = ksty.width*ksty.aspect*ksty.aspect/osd.vpAspect;

		x0 = this.x-0.5*(w-kwidth)/osd.vpWidth;
		x1 = this.x+0.5*(w-kwidth)/osd.vpWidth;

		//csuszka knob:
		button = osd.createButton( ksty, x, y, pri+1, null, command );	//itt nem lehet string, mert megjelenik!
		button.parent=this;	//a button a slidernek kuldi uzeneteit!

		button.nofocus=1;
		noleaverelease=1;

		//ujabb drot:
		//value label
		y-=0.5*Text.getHeight( sty.charset, osd.getViewport() );
		Text txt = osd.createText( null, sty.charset, Text.ALIGN_LEFT, this.x+(w*0.52)/osd.vpWidth, y );
		txt.changeColor( colorUnselected );
		setVLabel( txt );

		setValue( v );
	}

	//value label
	public void setVLabel( Text txt )
	{
		vLabel = new Label( txt );
	}

	public String getVLabelText()
	{
		if( vLabel )
			return vLabel.txt.text;

		return null;
	}

	public void changeVLabelText( String newtxt )
	{
		if( vLabel )
			vLabel.txt.changeText( newtxt );
	}

	public void changeVLabelColor( int newcolor )
	{
		if( vLabel )
			vLabel.txt.changeColor( newcolor );
	}
	//

	//ha engedelyezzuk folyamatosan kiirja a slider erteket
	public void printValue( String fmt )
	{
		vFmt = fmt;
		scale = 1.0;
		changeVLabelText( Float.toString( value, vFmt ) );
	}

	public void printValue( String fmt, float mul )
	{
		vFmt = fmt;
		scale = mul;
		changeVLabelText( Float.toString( value * mul, vFmt ) );
	}

	public void setRange( float a, float b )
	{
		this.a = a;
		this.b = b;
	}

	public void setTicks( int t )
	{
		ticks=t;
	}

	public void focus()
	{
		super.focus();

		//mindegy, hogy statikus vagy nem, ugyis csak a slider focuszaltsaganak ideeig tart
		//es igy konnyebb eltavolitani is!
		hkl = osd.createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_DEC|Osd.PRIVATEEVENT );
		hkr = osd.createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_INC|Osd.PRIVATEEVENT );
	}
	public void unfocus()
	{
		super.unfocus();

		osd.hotkey.removeElement( hkl ); hkl.inactivate(); hkl=null;
		osd.hotkey.removeElement( hkr ); hkr.inactivate(); hkr=null;
	}

	public void press()
	{ 
		if( !pressed )
		{
			sendEvent( new Event( cmd, Event.F_GAD_DRAGBEGIN ) );

			super.press();
		}
	}

	public void release()
	{ 
		if( pressed )
		{
			sendEvent( new Event( cmd, Event.F_GAD_DRAGEND ) );

			super.release();
		}
	}

	public void click( Gadget g )
	{
		Vector3 cv = Input.cursor.getPos();
		float val = a+(b-a)*osd.mapClickPosH( cv, x0, x1 );

		if( val != value )
		{
			setValue( val );
			sendEvent(Event.F_GAD_LCLICK);
		}
	}

	public void increase()
	{
		float original = value;

		if( ticks > 1)
			setValue( value+(b-a)/ticks );
		else
			setValue( value+(b-a)*0.1 );

		if( value != original )
		{
			sendEvent(Event.F_GAD_CHANGE);
		}
	}

	public void decrease()
	{
		float original = value;

		if( ticks > 1 )
			setValue( value-(b-a)/ticks );
		else
			setValue( value-(b-a)*0.1 );

		if( value != original )
		{
			sendEvent(Event.F_GAD_CHANGE);
		}
	}


	public void setValue( float v )
	{
		if( a < b )
		{
			if( v > b )
				value = b;
			else
			if( v < a )
				value = a;
			else
				value = v;
		}
		else
		{
			if( v > a )
				value = a;
			else
			if( v < b )
				value = b;
			else
				value = v;
		}
		
		if( ticks > 1)
		{
			float tickRange = (b-a)/(ticks-1);
			int	castint = (value-a)/tickRange;
			float diff = (value-a) - tickRange*castint;
			if( diff > 0.0 )
			{
				if( diff < tickRange/2.0 )
					value = a+tickRange*castint;
				else
					value = a+tickRange*(castint+1);
			}
		}

		float dx = x0+((value-a)/(b-a))*(x1-x0);

		Vector3 pos = osd.convertTextCoordinates( dx, y, 1 );
		button.rect.setPos( pos );
		button.rect.setPos( pos );
		button.phy.setMatrix( pos, null );

		if( vFmt )
			changeVLabelText( Float.toString( value * scale, vFmt ) );
	}
}
