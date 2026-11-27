package java.render.osdx;

import java.render.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): a small accent title with a thin rule
// under it, to split a page into sections.
public class SectionHeader
{
	public final static int	COLOR = 0xFFFFAE00;

	public	Text		text;
	public	Rectangle	rule;

	public SectionHeader( Osd osd, String title, float x, float y, float width )
	{
		text = osd.createText( title, Frontend.smallFont, Text.ALIGN_LEFT, x, y );
		text.changeColor( COLOR );
		float h = 2.0*Text.getFontSize( Frontend.smallFont )/(Config.video_y*osd.vpHeight);
		rule = osd.createRectangle( x + 0.5*width, y + h + 0.012, width, 0.004, 1, 0x80FFAE00 );
	}
}
