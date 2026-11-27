package java.render.osdx.dialog;

import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.io.*;
import java.util.resource.*;


public class MenuDialog extends Dialog
{
	String		info;
	String[]	labels;
	ScrollGroup	scroller;

	public MenuDialog( Controller ctrl, int myFlags, String myTitle, String info, String[] myLabels )
	{
		super( ctrl, myFlags|DF_DARKEN, myTitle, null );
		labels = myLabels;
		this.info = info;
	}

	public void show()
	{
		float	y = -0.5;

		if( info )
		{
			osd.createText( info, Frontend.mediumFont, Text.ALIGN_CENTER, 0.0, y );
			y+=0.2;
		}

		Style bs = new Style( 0.25, 0.12, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_TEST );
		osd.createButton( bs, 0.0, 0.9, "$637|CANCEL", 0 );

		Style sbu = new Style( 0.1, 0.1, 1.0, null, Text.ALIGN_CENTER, new ResourceRef(Osd.RID_ARROWUP) );
		Style sbd = new Style( 0.1, 0.1, 1.0, null, Text.ALIGN_CENTER, new ResourceRef(Osd.RID_ARROWDN) );
		osd.createButton( sbu, -0.5, 0.9,  1025, "$638|Scroll up" );
		osd.createButton( sbd,  0.5, 0.9,  1026, "$639|Scroll down" );

		scroller = new ScrollGroup( osd, -0.5, -0.5, 1.0, 1.0 );
		scroller.setLimits( 0, 0,  0, 6 );

		bs = new Style( 0.45, 0.10, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_TEST );
		Menu m = osd.createMenu( bs, 0.0, -0.8, 0 );

		for ( int i=0; i<labels.length; i++ )
			m.addItem( labels[i], i+1 );

		super.show();
	}

	public void	handleEvent( Event e )
	{
		if( e.cmd < labels.length+1 )
		{
			result=e.cmd;
			notify();
		}
		else
		if( e.cmd == 1025 )
		{
			scroller.vScroll(2.0);
		}
		else
		if( e.cmd == 1026 )
		{
			scroller.vScroll(-2.0);
		}
	}

}
