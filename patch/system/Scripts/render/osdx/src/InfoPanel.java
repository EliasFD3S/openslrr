package java.render.osdx;

import java.render.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): a framed panel that explains the gadget
// under the pointer or the keyboard focus. It is the Osd's tooltip box, so
// Osd.focusGadget fills it with Gadget.tooltip; setText writes it directly.
public class InfoPanel
{
	public final static int	COLOR_BG	= 0xB0000000;
	public final static int	COLOR_EDGE	= 0xFFFFAE00;

	Osd			osd;
	public	Text		title;
	public	TextBox		body;
	String		idle;

	public InfoPanel( Osd o, String heading, String idleText, float x, float y, float w, float h )
	{
		osd = o;
		idle = idleText;
		osd.createRectangle( x, y, w, h, 1, COLOR_BG );
		osd.createRectangle( x - 0.5*w + 0.003, y, 0.006, h, 2, COLOR_EDGE );
		title = osd.createText( heading, Frontend.smallFont, Text.ALIGN_LEFT, x - 0.5*w + 0.025, y - 0.5*h + 0.02 );
		title.changeColor( COLOR_EDGE );
		body = osd.createTextBox( idleText, Frontend.smallFont, Text.ALIGN_LEFT, x - 0.5*w + 0.025, y - 0.5*h + 0.085, w - 0.05 );
		osd.setTooltipBox( body );
	}

	public void setText( String t )
	{
		if( t )
			body.changeText( t );
		else
			body.changeText( idle );
	}
}
