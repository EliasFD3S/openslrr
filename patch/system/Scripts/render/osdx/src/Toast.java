package java.render.osdx;

import java.render.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): a short notification ("Preset applied")
// that hides itself after a few seconds. The plate and text are built once
// and made invisible by colour, so show() works from any page.
public class Toast extends GameType
{
	public final static int	COLOR_BG	= 0xE0202020;
	public final static int	COLOR_TEXT	= 0xFFFFFFFF;

	Rectangle	plate, edge;
	Text		text;
	int			serial;
	// Optional: the toast drops in from the top.
	public	OsdAnimator	animator;

	public Toast( Osd osd, float x, float y, float w, float h )
	{
		createNativeInstance();
		plate = osd.createRectangle( x, y, w, h, 6, 0x00000000 );
		edge = osd.createRectangle( x, y + 0.5*h - 0.003, w, 0.006, 7, 0x00000000 );
		text = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, x, y - 0.5*Text.getFontSize( Frontend.smallFont )*2.0/(Config.video_y*osd.vpHeight) );
		setEventMask( EVENT_TIME );
	}

	public void show( String message, float seconds )
	{
		plate.setColor( COLOR_BG );
		edge.setColor( 0xFFFFAE00 );
		text.changeText( message );
		text.changeColor( COLOR_TEXT );
		if( animator )
		{
			animator.slide( plate, OsdAnimator.FROM_TOP, 0.15, 0.3, 0.0, 1 );
			animator.slide( edge, OsdAnimator.FROM_TOP, 0.15, 0.3, 0.0, 1 );
			animator.slide( text, OsdAnimator.FROM_TOP, 0.15, 0.3, 0.0, 0 );
		}
		serial++;
		addTimer( seconds, serial );
	}

	public void hide()
	{
		plate.setColor( 0x00000000 );
		edge.setColor( 0x00000000 );
		text.changeText( null );
	}

	public void handleEvent( GameRef obj_ref, int event, int param )
	{
		if( param == serial )	// only the latest message hides it
			hide();
	}
}
