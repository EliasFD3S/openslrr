package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): an OFF/ON switch. It is a MultiChoice
// (mouse click, Enter and left/right all work) with a status lamp that is
// green when on and grey when off.
public class Toggle extends MultiChoice
{
	public final static int	COLOR_ON	= 0xFF3CC85A;
	public final static int	COLOR_OFF	= 0xFF464646;

	Rectangle	lamp;

	public Toggle( Osd o, Style sty, float px, float py, String string, int command, int v )
	{
		super( o, sty, px, py, 0, string, command, v, Toggle.onOff(), MCAF_LOOP );

		// A small lamp right of the value (Menu rows are ALIGN_RIGHT: the
		// value ends at the box's right edge).
		float h = 0.022;
		lamp = osd.createRectangle( x + 0.5*w + 0.03, y, h*0.75*1.5, h, 3, COLOR_OFF );
		setValue( value );
	}

	public static Vector onOff()
	{
		Vector v = new Vector();
		v.addElement( "OFF" );
		v.addElement( "ON" );
		return v;
	}

	public void setValue( int v )
	{
		super.setValue( v );
		if( lamp )
		{
			if( value )
				lamp.setColor( COLOR_ON );
			else
				lamp.setColor( COLOR_OFF );
		}
	}
}
