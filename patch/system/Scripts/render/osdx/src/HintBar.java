package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): a key legend along the bottom of a
// screen ("ESC  BACK"). add() places entries left to right.
public class HintBar
{
	Osd		osd;
	float	x, y;

	public HintBar( Osd o, float left, float y )
	{
		osd = o;
		x = left;
		this.y = y;
	}

	public void add( String key, String action )
	{
		Text k = osd.createText( key, Frontend.smallFont, Text.ALIGN_LEFT, x, y );
		k.changeColor( 0xFFFFAE00 );
		x += 0.02 + key.length()*0.018;
		Text a = osd.createText( action, Frontend.smallFont, Text.ALIGN_LEFT, x, y );
		a.changeColor( 0xFFB0B0B0 );
		x += 0.07 + action.length()*0.018;
	}
}
