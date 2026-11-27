package java.render.osdx.dialog;

import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.io.*;

public class TextDialog extends Dialog
{
	String	text;
	TextBox	textbox;

	public TextDialog( Controller ctrl, int myFlags, String myTitle, String myButtons, String mybody )
	{
		super( ctrl, myFlags, myTitle, myButtons );
		text=mybody;
	}

	public TextDialog( Controller ctrl, int myFlags, String myTitle, String myButtons, String mybody, float size, float aspect )
	{
		super( ctrl, myFlags, myTitle, myButtons, size, aspect );
		text=mybody;
	}

	public TextDialog( Controller ctrl, int myFlags, String myTitle, String myButtons, String mybody, float size, float aspect, float cx, float cy, float refx, float refy )
	{
		super( ctrl, myFlags, myTitle, myButtons, size, aspect, cx, cy, refx, refy );
		text=mybody;
	}

	public void show()
	{
		if( text )
		{
			float x = -0.85;
			float y = -0.7;
			float width = 1.7;

			textbox = osd.createTextBox( text, Frontend.mediumFont, Text.ALIGN_CENTER, x, y, width );
			textbox.changeColor( 0xFFFFFFFF );
		}
		super.show();
	}

	public void changeText(String txt)
	{
		text = txt;
		if (textbox)
			textbox.changeText(text);
	}
}
