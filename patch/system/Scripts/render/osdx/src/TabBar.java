package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

// OpenSLRR addition (OSD v2 demo): a row of tabs. Tab i is a Button whose
// command is baseCmd+i; select(i) moves the accent bar under the active tab
// and dims the others. The screen shows the matching page group.
public class TabBar
{
	public final static int	COLOR_ACTIVE	= 0xFFFFAE00;
	public final static int	COLOR_IDLE		= 0x40FFFFFF;
	public final static int	COLOR_TAB		= 0x60000000;

	Osd			osd;
	Vector		buttons = new Vector();
	Vector		marks = new Vector();
	public	int	baseCmd;
	public	int	active = -1;

	public TabBar( Osd o, float x, float y, float tabWidth, float tabHeight, String[] names, int cmd )
	{
		osd = o;
		baseCmd = cmd;

		// Style widths are 4:3 units; on screen a tab is this wide.
		float tw = tabWidth*(4.0/3.0)/osd.vpAspect;
		float left = x - 0.5*tw*names.length;
		// A coloured Style would tint the label too (Gadget.customCU): the
		// tab plate is a separate rectangle.
		Style st = new Style( tabWidth, tabHeight, Frontend.mediumFont, Text.ALIGN_CENTER, null );

		for( int i=0; i<names.length; i++ )
		{
			float cx = left + (i+0.5)*tw;
			osd.createRectangle( cx, y, tw*0.96, tabHeight, 0, COLOR_TAB );
			buttons.addElement( osd.createButton( st, cx, y, names[i], cmd+i ) );
			marks.addElement( osd.createRectangle( cx, y + 0.5*tabHeight + 0.006, tw*0.92, 0.008, 2, COLOR_IDLE ) );
		}
	}

	public void select( int i )
	{
		active = i;
		// if/else, not ?: (the ternary as an argument always took its
		// second branch in the VM; see docs/ui/osd-v2.md)
		for( int j=0; j<marks.size(); j++ )
		{
			Rectangle r = (Rectangle)marks.elementAt(j);
			if( j == i )
				r.setColor( COLOR_ACTIVE );
			else
				r.setColor( COLOR_IDLE );
		}
	}

	// The tab index of a command, or -1.
	public int indexOf( int cmd )
	{
		int i = cmd - baseCmd;
		if( i >= 0 && i < buttons.size() )
			return i;
		return -1;
	}

	public int size()
	{
		return buttons.size();
	}
}
