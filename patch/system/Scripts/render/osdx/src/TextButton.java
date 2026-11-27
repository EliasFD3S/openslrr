package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.resource.*;
import java.util.*;


//szoveged erteke lehet, az van "bele" irva
public class TextButton extends Button
{
	Label	vLabel;	//extra label for displaying the value

	public TextButton( Osd o, Style sty, float x, float y, int pri, String string, int command, String value )
	{
		super( o, sty, x, y, pri, string, command );

		//ujabb drot:
		//value label
		y-=0.5*Text.getHeight( sty.charset, osd.getViewport() );
		Text txt = osd.createText( value, sty.charset, Text.ALIGN_RIGHT, this.x+(w*0.5)/osd.vpWidth, y );
		txt.changeColor( colorUnselected );
		setVLabel( txt );
	}

	//value label
	public void setVLabel( Text txt )
	{
		vLabel = new Label( txt );
	}

	public String getVLabelText()
	{
		return vLabel.txt.text;
	}

	public void changeVLabelText( String newtxt )
	{
		vLabel.txt.changeText( newtxt );
	}

	public void changeVLabelColor( int newcolor )
	{
		vLabel.txt.changeColor( newcolor );
	}
	//

	public void focus()
	{
		super.focus();
		changeVLabelColor( Osd.COLOR_SELECTED );
	}
	public void unfocus()
	{
		super.unfocus();
		changeVLabelColor( colorUnselected );
	}

	public void press()
	{
		super.press();
		changeVLabelColor( Osd.COLOR_PRESSED );
	}
	public void release()
	{
		super.release();
		changeVLabelColor( Osd.COLOR_SELECTED );
	}
}
