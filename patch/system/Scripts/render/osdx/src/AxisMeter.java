package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2): a segmented level bar for a live value, such
// as a pedal or the steering. Segments are tinted, never rebuilt, so it can
// be updated every frame. A centred meter lights from the middle (-1..1).
public class AxisMeter
{
	public final static int	COLOR_ON	= 0xFFFFAE00;
	public final static int	COLOR_OFF	= 0x30FFFFFF;
	public final static int	COLOR_CENTRE = 0x90FFFFFF;

	Vector		segs = new Vector();
	int			centred;
	int			shownLo = -1, shownHi = -1;
	public	float	value;

	public AxisMeter( Osd osd, float x, float y, float width, float height, int segments, int centredMeter )
	{
		centred = centredMeter;
		float step = width / segments;
		for( int i=0; i<segments; i++ )
			segs.addElement( osd.createRectangle( x - 0.5*width + (i+0.5)*step, y, step*0.78, height, 3, COLOR_OFF ) );
		if( centred )
			osd.createRectangle( x, y, 0.004, height*1.6, 4, COLOR_CENTRE );
		setValue( 0.0 );
	}

	// Hidden: every segment transparent until the next setVisible(1).
	int		hidden;

	public void setVisible( int v )
	{
		if( v )
		{
			if( hidden )
			{
				hidden = 0;
				shownLo = -2;	// repaint on the next setValue
				setValue( value );
			}
			return;
		}
		hidden = 1;
		for( int i=0; i<segs.size(); i++ )
			((Rectangle)segs.elementAt(i)).setColor( 0x00000000 );
	}

	// 0..1, or -1..1 for a centred meter.
	public void setValue( float v )
	{
		value = v;
		int n = segs.size();
		int lo, hi;
		if( centred )
		{
			if( v < -1.0 ) v = -1.0;
			if( v > 1.0 ) v = 1.0;
			int mid = n/2;
			int k = v*mid;
			if( k >= 0 )
			{
				lo = mid;
				hi = mid + k;
			}
			else
			{
				lo = mid + k;
				hi = mid;
			}
		}
		else
		{
			if( v < 0.0 ) v = 0.0;
			if( v > 1.0 ) v = 1.0;
			lo = 0;
			hi = v*n + 0.5;
		}
		if( hidden || (lo == shownLo && hi == shownHi) )
			return;
		shownLo = lo;
		shownHi = hi;
		for( int i=0; i<n; i++ )
		{
			Rectangle r = (Rectangle)segs.elementAt(i);
			if( i >= lo && i < hi )
				r.setColor( COLOR_ON );
			else
				r.setColor( COLOR_OFF );
		}
	}
}
