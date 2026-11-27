package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2): UI animations. Rectangles and texts slide in
// from the left, right, top or bottom, optionally fading in (rectangles), with
// an ease-out curve and a delay each. slideGroup moves a whole page: its texts
// and rectangles plus its gadgets' plates and slider knobs, in a cascade from
// top to bottom. Every element ends exactly where it was built; hit boxes do
// not move.
public class OsdAnimator implements Animated
{
	public final static int	FADE		= 0;
	public final static int	FROM_LEFT	= 1;
	public final static int	FROM_RIGHT	= 2;
	public final static int	FROM_TOP	= 3;
	public final static int	FROM_BOTTOM	= 4;

	Osd		osd;
	Vector	items = new Vector();

	public OsdAnimator( Osd o )
	{
		osd = o;
		osd.addAnimated( this );
	}

	// distance in OSD units (2 = the screen width or height).
	public void slide( Object target, int from, float distance, float duration, float delay, int fade )
	{
		if( !target )
			return;
		if( target instanceof Rectangle && ((Rectangle)target).fixed )
			return;
		OsdAnimation a = find( target );
		if( !a )
		{
			a = new OsdAnimation();
			a.target = target;
			if( target instanceof Rectangle )
			{
				Rectangle r = (Rectangle)target;
				a.base = new Vector3( r.pos );
				a.color = r.color;
			}
			else
			if( target instanceof Text )
			{
				a.isText = 1;
				a.base = ((Text)target).getPos();
			}
			else
				return;
			items.addElement( a );
		}
		a.dx = 0.0;
		a.dy = 0.0;
		if( from == FROM_LEFT ) a.dx = -distance;
		if( from == FROM_RIGHT ) a.dx = distance;
		if( from == FROM_TOP ) a.dy = -distance;
		if( from == FROM_BOTTOM ) a.dy = distance;
		a.fade = fade;
		if( from == FADE )
			a.fade = 1;
		a.time = 0.0;
		a.delay = delay;
		a.duration = duration;
		if( a.duration < 0.01 )
			a.duration = 0.01;
		apply( a, 0.0 );
	}

	public void fade( Object target, float duration, float delay )
	{
		slide( target, FADE, 0.0, duration, delay, 1 );
	}

	// A whole group (an Osd page): spread is the delay between its top and its
	// bottom, for a cascade.
	public void slideGroup( int hash, int from, float distance, float duration, float spread, int fade )
	{
		Group g = null;
		for( int i=osd.groups.size()-1; i>=0; i-- )
			if( ((Group)osd.groups.elementAt(i)).hashCode() == hash )
				g = osd.groups.elementAt(i);
		if( !g )
			return;
		for( int i=0; i<g.misc.size(); i++ )
		{
			Object o = g.misc.elementAt(i);
			slide( o, from, distance, duration, spread*rank( o ), fade );
		}
		for( int i=0; i<g.gadget.size(); i++ )
		{
			Gadget gd = g.gadget.elementAt(i);
			if( gd.rect )
				slide( gd.rect, from, distance, duration, spread*rank( gd.rect ), fade );
		}
	}

	// 0 at the top of the screen, 1 at the bottom.
	float rank( Object o )
	{
		float y = 0.0;
		if( o instanceof Text )
			y = ((Text)o).getPos().y;
		else
		if( o instanceof Rectangle )
			y = -((Rectangle)o).pos.y/(Osd.SCALE_FS*osd.vpHeight);
		else
			return 0.0;
		y = 0.5*(y + 1.0);
		if( y < 0.0 ) y = 0.0;
		if( y > 1.0 ) y = 1.0;
		return y;
	}

	OsdAnimation find( Object target )
	{
		for( int i=items.size()-1; i>=0; i-- )
		{
			OsdAnimation a = items.elementAt(i);
			if( a.target == target )
				return a;
		}
		return null;
	}

	public int running()
	{
		return items.size();
	}

	public void tick( float dt )
	{
		for( int i=items.size()-1; i>=0; i-- )
		{
			OsdAnimation a = items.elementAt(i);
			a.time += dt;
			float p = (a.time - a.delay)/a.duration;
			if( p < 0.0 )
				p = 0.0;
			if( p >= 1.0 )
			{
				apply( a, 1.0 );
				items.removeElementAt( i );
			}
			else
				apply( a, p );
		}
	}

	void apply( OsdAnimation a, float p )
	{
		float q = 1.0 - p;
		float e = 1.0 - q*q*q;	// ease-out cubic
		float k = 1.0 - e;
		if( a.isText )
		{
			Text t = (Text)a.target;
			t.setPos( a.base.x + a.dx*k*osd.vpWidth, a.base.y + a.dy*k*osd.vpHeight );
		}
		else
		{
			Rectangle r = (Rectangle)a.target;
			Vector3 v = new Vector3( a.base );
			v.x += a.dx*k*Osd.SCALE_FS*osd.vpAspect*osd.vpWidth;
			v.y -= a.dy*k*Osd.SCALE_FS*osd.vpHeight;
			r.setPos( v );
			// The host scales the alpha: the script never rebuilds the colour.
			if( a.fade && r.ins )
				OsdHost.fade( r.ins, e );
		}
	}
}

// One running animation.
class OsdAnimation
{
	Object	target;
	int		isText;
	Vector3	base;
	int		color;
	float	dx, dy;
	int		fade;
	float	time, delay, duration;
}
