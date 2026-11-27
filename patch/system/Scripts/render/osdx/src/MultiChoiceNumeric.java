package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class MultiChoiceNumeric extends Gadget
{
	final static int MCAF_LOOP = 1;

	final static int CONTENT_UNSELECTED_COLOR		= 0xFFA0A0A0;
	final static int CONTENT_SELECTED_COLOR			= 0xFFFFFFFF;

	Hotkey		m_hkl, m_hkr;

	int				m_minimum, m_levels, m_step;
	int				m_level;
	int				m_flags;

	Text			m_ccText;

	public MultiChoiceNumeric( Osd o, Style sty, float x, float y, int pri, String string, int command, int min, int levels, int step, int v, int flags)
	{
		super( o, sty, x, y, pri, string, command);

		m_minimum = min;
		m_step = step;
		m_levels = levels;
		m_level = 0;
		m_flags = flags;

		y-=0.5*Text.getHeight( sty.charset, osd.getViewport() );
		m_ccText = osd.createText( null, sty.charset, Text.ALIGN_CENTER, this.x, y );
		m_ccText.changeColor( CONTENT_UNSELECTED_COLOR );

		if ( 0 == setLevel( v ) )
			updateText();
	}

	public void focus()
	{
		super.focus();

		m_hkl = osd.createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_DEC|Osd.PRIVATEEVENT );
		m_hkr = osd.createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_INC|Osd.PRIVATEEVENT );

		m_ccText.changeColor( CONTENT_SELECTED_COLOR );
	}

	public void unfocus()
	{
		super.unfocus();

		osd.hotkey.removeElement( m_hkl ); m_hkl.inactivate(); m_hkl=null;
		osd.hotkey.removeElement( m_hkr ); m_hkr.inactivate(); m_hkr=null;

		m_ccText.changeColor( CONTENT_UNSELECTED_COLOR );
	}

	public void increase()
	{
		if ( 0 != setLevel( m_level + 1 ) )
		{
			sendEvent(Event.F_GAD_CHANGE);
		}
	}

	public void decrease()
	{
		if ( 0 != setLevel( m_level - 1 ) )
		{
			sendEvent(Event.F_GAD_CHANGE);
		}
	}

	public void click()
	{
/*
		int cmp = value;

		setValue( value+1 );

		if( cmp == value )
		{
			setValue( offset );
		}
		sendEvent(Event.F_GAD_LCLICK);
*/
	}

	public int setLevel( int v )
	{
		if ( m_flags & MCAF_LOOP )
		{
			if ( v < 0 )
				v = m_levels-1;
			else
				if ( v >= m_levels )
					v = 0;
		} else
		{
			if ( v < 0 )
				v = 0;
			else
				if ( v >= m_levels )
					v = m_levels-1;

			if ( m_level == v )
				return 0;
		}
		m_level = v;
		updateText();

		return 1;
	}

	public int getLevel()
	{
		return m_level;
	}

	public void setParams(int min, int levels, int step)
	{
		m_minimum = min;
		m_levels = levels;
		m_step = step;

		setLevel(m_level);
		updateText();
	}

	public void setMinimum(int min)
	{
		m_minimum = min;
		setLevel(m_level);
		updateText();
	}

	public void setLevels(int levels)
	{
		m_levels = levels;
		setLevel(m_level);
		updateText();
	}

	public void setStep(int step)
	{
		m_step = step;
		setLevel(m_level);
		updateText();
	}

	void updateText()
	{
		int			value = m_minimum + m_level * m_step;
		m_ccText.changeText( "" + value );
	}

}
