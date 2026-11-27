package java.render.osdx.dialog;

import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.io.*;


public class StringRequesterDialog extends TextDialog
{
	String input;

	public StringRequesterDialog( Controller ctrl, int myflags, String mytitle, String init )
	{
		super( ctrl, myflags, mytitle, "$644|OK"+";"+"$645|Cancel", null );

		input = init;
	}

	public void show()
	{
		Style sty = new Style( 0.75, 0.12, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_TEST );

		osd.createStringInput( sty, 0.0, 0.0, null, 12345, input );

		super.show();
	}

	public void	handleEvent( Event e )
	{

		if( e.cmd==12345 )
		{
			input = ((StringInput)e.gadget).inputString;

			System.log( (flags&SIF_NOEMPTY) && input.length() == 0 );
			if( !((flags&SIF_NOEMPTY) && input.length() == 0) )
			{
				if( !osd.actGadget )		//ha most veszti el a fokuszt
					handleEvent( new Event( 0 ) );	//OK generated
			}
		}
		else
		{
			if( e.cmd==0 )	//OK de-generated?
			{
				if( ((flags&SIF_NOEMPTY) && input.length() == 0) )
				{
					return;	//nem hagyjuk az ureset!
				}
			}

			super.handleEvent( e );
		}
	}
}

