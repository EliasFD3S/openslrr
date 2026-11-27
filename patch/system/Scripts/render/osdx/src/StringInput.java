package java.render.osdx;

import java.render.*;
import java.util.resource.*;
import java.util.*;
import java.io.*;

public class StringInput extends Gadget implements Runnable
{
	final static int CONTENT_UNSELECTED_COLOR		= 0xFFA0A0A0;
	final static int CONTENT_SELECTED_COLOR			= 0xFFFFFFFF;

	public	String	inputString;
	Text	inputText;
	int		max_char_length;
	float	max_length;

	String	backupString;	//in case of 'ESC'-ing
	int		changed;
	Thread	blink;

	public	int		colorSelected = Osd.COLOR_SELECTED;
	int		secured;
	int		keep;

	//hossz szamitashoz:
	ResourceRef font;
	Viewport	vp;

	public StringInput( Osd o, Style sty, float x, float y, int pri, String string, int command, String v )
	{
		this(  o, sty, x, y, pri, string, command, v, 0 );
	}

	public StringInput( Osd o, Style sty, float x, float y, int pri, String string, int command, String v, int secure )
	{
		super( o, sty, x, y, pri, string, command );

		y-=0.5*Text.getHeight( sty.charset, osd.getViewport() );
		inputText = osd.createText( null, sty.charset, sty.align, x, y );
		inputText.changeColor( CONTENT_UNSELECTED_COLOR );
		secured = secure;

		font = sty.charset;
		vp = osd.getViewport();

		setValue( v );
	}

	public void setLength(int max)
	{
		max_char_length = max;
	}

	public void setLength(float max)
	{
		max_length = max;
	}

	public void click()
	{
		beginInput();
	}

	public void setValue( String s )
	{
		inputString = s;

		inputText.changeText( convertString(inputString) );
	}

	public int key( int ascii )
	{
		if( !inputString )
			inputString = "";

		if( (max_length && Text.getWidth(inputString,font,vp) < max_length) || (max_char_length && inputString.length() < max_char_length) || ascii == 8 || ascii == 13 || ascii == 27 )//hossz limit
		{
			if( ascii == 27 )	//escape
			{
				changed = 0;
				endInput();
				sendEvent(Event.F_GAD_CHANGE);
			}
			else
			if( ascii == 13)	//enter
			{
				changed = 1;
				osd.eatUpNext = 1;
				endInput();
				sendEvent(Event.F_GAD_CHANGE);
			}
			else				//normal keys (incl backspace)
			{
				changed = 1;
				inputString = inputString.append( ascii );
				inputText.changeText( convertString(inputString) + blinkChar );
			}

			return 1;
		}

		return 0;
	}

	public void stop()
	{
		endInput();
	}

	public void beginInput()
	{
		if( !blink )
		{
			Input.flushKeys();	//ha pl hotkey inditotta, az ne jelenhessen meg

			osd.actGadget=this;
			backupString = inputString;

			if (!keep)
				inputString = null;

			blink = new Thread(this);
			blink.start();

			inputText.changeColor( CONTENT_SELECTED_COLOR );
		}
	}

	public void endInput()
	{
		osd.actGadget=null;
		blink.stop();
		blink = null;

		if( !changed )
			inputString = backupString;

		inputText.changeText( convertString(inputString) );
	}

	String blinkChar;
	public void run()
	{
		while( 1 )
		{
			blinkChar = "_";
			inputText.changeText( convertString(inputString) + blinkChar );
			Thread.sleep( 250 );
			blinkChar = "\x1bC00000000_";
			inputText.changeText( convertString(inputString) + blinkChar );
			Thread.sleep( 250 );
		}
	}


	public void focus()
	{ 
		super.focus();
		inputText.changeColor( CONTENT_SELECTED_COLOR );
	}

	public void unfocus()
	{ 
		super.unfocus();
		inputText.changeColor( CONTENT_UNSELECTED_COLOR );
	}


	String convertString(String str)
	{
		if ( !secured )
			return str;

		String		tmp;

		tmp = "";
		if (!inputString)
			return tmp;
		for ( int i=0; i<inputString.length(); i++ )
			tmp += "*";

		return tmp;
	}
}

