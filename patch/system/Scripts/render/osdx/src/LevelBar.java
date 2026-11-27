package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2): a smooth level bar for a live value (pedal,
// steering). The fill is one rectangle resized in place (OsdHost.resize)
// and eased towards the value every frame; a bright cap marks its end.
// A centred bar fills from the middle (-1..1).
public class LevelBar implements Animated
{
	public final static int	COLOR_TRACK	= 0x38FFFFFF;
	public final static int	COLOR_FILL	= 0xFFFFAE00;
	public final static int	COLOR_CAP	= 0xFFFFFFFF;
	public final static float	SPEED = 14.0;	// easing rate, 1/s

	Osd			osd;
	Group		group;
	Rectangle	track, fill, cap;
	Vector3		trackBase;
	float		x, y, w, h;
	int			centred;
	float		applied = -9.0;
	public	float	target, shown;

	public LevelBar( Osd o, float x, float y, float width, float height, int centredBar )
	{
		osd = o;
		group = osd.groups.lastElement();
		this.x = x;
		this.y = y;
		w = width;
		h = height;
		centred = centredBar;
		track = osd.createRectangle( x, y, width, height, 2, COLOR_TRACK );
		trackBase = new Vector3( track.pos );
		if( centred )
			osd.createRectangle( x, y, 0.004, height*1.8, 3, 0x90FFFFFF );
		fill = osd.createRectangle( x, y, width, height, 3, COLOR_FILL );
		cap = osd.createRectangle( x, y, 0.005, height*1.7, 4, COLOR_CAP );
		// The bar places these itself, also while an OsdAnimator moves the track.
		fill.fixed = 1;
		cap.fixed = 1;
		osd.addAnimated( this );
		apply( 0.0 );
	}

	// Shows or hides the whole bar (track, fill and cap).
	public void setVisible( int v )
	{
		float f = 0.0;
		if( v )
			f = 1.0;
		OsdHost.fade( track.ins, f );
		OsdHost.fade( fill.ins, f );
		OsdHost.fade( cap.ins, f );
	}

	// 0..1, or -1..1 for a centred bar.
	public void setValue( float v )
	{
		if( centred )
		{
			if( v < -1.0 ) v = -1.0;
			if( v > 1.0 ) v = 1.0;
		}
		else
		{
			if( v < 0.0 ) v = 0.0;
			if( v > 1.0 ) v = 1.0;
		}
		target = v;
	}

	public void tick( float dt )
	{
		float k = dt*SPEED;
		if( k > 1.0 )
			k = 1.0;
		shown = shown + (target - shown)*k;
		apply( shown );
	}

	void apply( float v )
	{
		// Follow the track when an animation slides it.
		float ox = track.pos.x - trackBase.x;
		float oy = track.pos.y - trackBase.y;
		float d = v - applied;
		if( d < 0.0 ) d = -d;
		if( d < 0.0005 && ox == 0.0 && oy == 0.0 )
			return;
		applied = v;

		float lo, hi, end;
		if( centred )
		{
			end = x + v*w*0.5;
			if( v >= 0.0 )
			{
				lo = x;
				hi = end;
			}
			else
			{
				lo = end;
				hi = x;
			}
		}
		else
		{
			lo = x - 0.5*w;
			hi = lo + v*w;
			end = hi;
		}

		Vector3 p = group.convertTextCoordinates( 0.5*(lo + hi), y, 3 );
		p.x += ox; p.y += oy;
		fill.setPos( p );
		OsdHost.resize( fill.ins, 0.5*(hi - lo)*osd.vpWidth, 0.5*h*osd.vpHeight );

		Vector3 c = group.convertTextCoordinates( end, y, 4 );
		c.x += ox; c.y += oy;
		cap.setPos( c );
		if( hi - lo < 0.002 )
			cap.setColor( 0x00000000 );
		else
			cap.setColor( COLOR_CAP );
	}
}
