package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class MultiChoiceScroller /*extends Group*/ implements EventReceiver
{
	Hotkey		m_hkl, m_hkr;

	final static int CMD_SELECTED = 1000;	
	final static int CMD_SELECTED_MAX = 1099;	
	
	float			m_left, m_top;
	float			m_width, m_height;
	float			m_fontsize;
	float			m_linestep;

	int				m_levels;
	int				m_level;

	int				m_numlines;
	Button[]	m_lines;
	float[]		m_linesy;

	Osd					m_osd;

	ScrollGroup2	m_scroller;

	public MultiChoiceScroller( Osd o, Style sty, float x, float y, int pri, String string, int command, int levels, int level)
	{
		m_osd = o;

		// create gadgets
//		super( o, sty, x, y, pri, string, command );

		float			hmargin = 0.02;
		float			vmargin = 0.02;

		Style bse = new Style( sty.width - hmargin*2, 0.103, m_osd.vpAspect, Frontend.mediumFont, Text.ALIGN_LEFT, Osd.RRT_EMPTY );
		m_width = sty.width - hmargin*2;
		m_height = sty.height - vmargin*2;
		m_left = x - m_width/2;
		m_top = y - m_height/2;

		levels = 20;
		m_levels = levels;
		m_level = level;

		m_numlines = levels;
		m_numlines++;

		m_scroller = new ScrollGroup2( m_osd, m_left, m_top, m_width, m_height);


		m_fontsize = Text.getHeight(bse.charset, m_scroller.vp );
		m_linestep = m_fontsize + 0.03;

		m_scroller.setLimits(0,0, 0,m_linestep*m_numlines);
		System.log("MCS: " + m_left + " " + m_top + " " + m_width + " " + m_height + " " + m_linestep);

		m_lines = new Button[m_numlines];
		m_linesy = new float[m_numlines]; 

		// create buttons
		EventReceiver		er = m_osd.globalHandler;
		m_osd.setEventHandler( this );
		float			ly = -1.0 + m_fontsize/2;	//m_top;
		float			lx = 0.0;	//m_left;
		for ( int il=0; il<m_numlines; il++ )
		{
			m_linesy[il] = ly;
			m_lines[il] = m_osd.createButton( bse, lx, ly, "XYZW" + il, CMD_SELECTED + il);
			ly += m_linestep;	//(m_linestep * 2.0/m_height);
		}

		m_osd.addEventFlags( Event.F_GAD_FOCUS|Event.F_GAD_UNFOCUS );
		m_osd.setEventHandler( er );
		m_osd.endGroup();

/*
		m_osd.show();
		Input.cursor.enable(1);
*/
	Input.cursor.enable(1);
	setLevel( level );
//		if ( 0 == setLevel( level ) )
//			updateText();
	}

	public void focus()
	{
//		super.focus();

//		m_hkl = osd.createHotkey( Input.AXIS_MENU_LEFT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_DEC|Osd.PRIVATEEVENT );
//		m_hkr = osd.createHotkey( Input.AXIS_MENU_RIGHT, Input.VIRTUAL|Osd.HK_STATIC, Osd.CMD_MENU_INC|Osd.PRIVATEEVENT );

//		m_ccText.changeColor( Osd.COLOR_SELECTED );
	}

	public void unfocus()
	{
//		super.unfocus();

//		osd.hotkey.removeElement( m_hkl ); m_hkl.inactivate(); m_hkl=null;
//		osd.hotkey.removeElement( m_hkr ); m_hkr.inactivate(); m_hkr=null;

//		m_ccText.changeColor( colorUnselected );
	}

/*
	public void increase()
	{
		if ( 0 != setLevel( m_level + 1 ) )
			sendEvent(Event.F_GAD_CHANGE);
	}

	public void decrease()
	{
		if ( 0 != setLevel( m_level - 1 ) )
			sendEvent(Event.F_GAD_CHANGE);
	}
*/
	public int setLevel( int v )
	{
		if ( v < 0 )
			v = 0;
		else
			if ( v >= m_levels )
				v = m_levels-1;

		if ( m_level == v )
			return 0;

		m_level = v;
//		updateText();

		return 1;
	}

	public int getLevel()
	{
		return m_level;
	}

	public void setLevels(int levels)
	{
		m_levels = levels;
		setLevel(m_level);
//		updateText();
	}

/*
	void updateText()
	{
		int			value = m_minimum + m_level * m_step;
		m_ccText.changeText( "" + value );
	}
*/

	public void	handleEvent( Event e )
	{
		switch( e.flags )
		{
			case (Event.F_GAD_FOCUS):
				if ( (CMD_SELECTED <= e.cmd)
				&&		(e.cmd <= CMD_SELECTED_MAX) )
				{
					float			firsty = -m_scroller.getScroll().y;
					float			gady = m_lines[e.cmd-CMD_SELECTED].y;	//m_scroller.convertTextCoordinates(0.0, m_lines[e.cmd-CMD_SELECTED].y, 0).y;

//					firsty /= (m_height/2);
//					gady /= (m_height/2);

					System.log("firsty: " + firsty + "gady: " + gady);
					if ( (gady - m_fontsize/2) < -1.0 )
					{
						m_scroller.vScroll(m_linestep * 3);
//						m_scroller.vScroll(-2);
						System.log("-scroll " + m_linestep + m_scroller.getScroll().y);
					}

					if ( (gady + m_fontsize/2) > 1.0 )
					{
						m_scroller.vScroll(m_linestep * -3);
//						m_scroller.vScroll(2);
						System.log("+scroll " + m_linestep + m_scroller.getScroll().y);
					}
				
					m_scroller.activate();
					m_scroller.show();
				}
				break;
			case (Event.F_GAD_UNFOCUS):
				if (e.cmd <= CMD_SELECTED_MAX )
				{
				}
				break;
		}
	}
}	
