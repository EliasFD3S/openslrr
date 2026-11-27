package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class MultiChoice extends Gadget
{
	public	final static int MCAF_LOOP = 1;

	public	final static int CONTENT_UNSELECTED_COLOR		= 0xFFA0A0A0;
	public	final static int CONTENT_SELECTED_COLOR			= 0xFFFFFFFF;

	Hotkey		hkl, hkr;

	Vector		choices = new Vector();

	public	int		value;
	Text	ccText;
	int		m_flags;

	public	int		color_Selected = CONTENT_SELECTED_COLOR;
	public	int		color_Unselected = CONTENT_UNSELECTED_COLOR;

	public MultiChoice( Osd o, Style sty, float px, float py, int pri, String string, int command, int v, Vector chs, int flags, int cs, int cu )
	{
		super( o, sty, px, py, pri, string, command );
		color_Selected = cs;
		color_Unselected = cu;

		choices = chs;
		m_flags = flags;

		py-=0.5*Text.getHeight( sty.charset, osd.getViewport() );

		float w = sty.width*sty.aspect*sty.aspect/osd.vpAspect;

		int align = sty.align;

		if( this instanceof MultiChoiceArrow )
			align = Text.ALIGN_CENTER;

		switch( align )
		{
		case (Text.ALIGN_LEFT):
			px = x-w*0.5;
			break;
		case (Text.ALIGN_RIGHT):
			px = x+w*0.5;
			break;
		default:
			px=x;
		}
		
		ccText = osd.createText( null, sty.charset, align, px, py );
		ccText.changeColor( color_Unselected );
		
		setValue( v );
	
	}

	public MultiChoice( Osd o, Style sty, float px, float py, int pri, String string, int command, int v, Vector chs, int flags )
	{
		this( o, sty, px, py, pri, string, command, v, chs, flags, CONTENT_SELECTED_COLOR, CONTENT_UNSELECTED_COLOR );
	}

	public void reset()
	{
		ccText.changeColor( color_Unselected );
	}

	public void focus()
	{
		super.focus();

		hkl = osd.createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_DEC|Osd.PRIVATEEVENT );
		hkr = osd.createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_INC|Osd.PRIVATEEVENT );

		ccText.changeColor( color_Selected );
	}
	public void unfocus()
	{
		super.unfocus();

		osd.hotkey.removeElement( hkl ); hkl.inactivate(); hkl=null;
		osd.hotkey.removeElement( hkr ); hkr.inactivate(); hkr=null;

		ccText.changeColor( color_Unselected );
	}

	public void increase()
	{
		setValue( value+1 );
		sendEvent(Event.F_GAD_CHANGE);
	}

	public void decrease()
	{
		setValue( value-1 );
		sendEvent(Event.F_GAD_CHANGE);
	}

	public void click()
	{
		int cmp = value;

		setValue( value+1 );

		sendEvent(Event.F_GAD_LCLICK);
	}

	public void setValue( int v )
	{
		if( choices && (choices.size() > 0) )
		{
			if ( m_flags & MCAF_LOOP )
			{
				if( v >= choices.size() )
					value = 0;
 				else
				if( v < 0 )
					value = choices.size()-1;
				else
					value = v;
			} else
			{
				if( v >= choices.size() )
					value = choices.size()-1;
 				else
				if( v < 0 )
					value = 0;
				else
					value = v;
			}
			ccText.changeText( (String)(choices.elementAt(value)) );
		} else
			ccText.changeText("");
	}

	public String getChoiceText()
	{
		if( ccText )
			return ccText.text;

		return null;
	}

	public void changeChoiceText( String newtxt )
	{
		if( ccText )
			ccText.changeText( newtxt );
	}

	public void setChoices( Vector chs )
	{
		choices = chs;
	}
}
