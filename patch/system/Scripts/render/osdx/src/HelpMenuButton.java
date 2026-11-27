package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class HelpMenuButton extends Gadget
{
	final static int TITLE_UNSELECTED_COLOR		= 0xFFA0A0A0;
	final static int TITLE_UNSELECTED_COLOR2	= 0xFFAEFF00;
	final static int TITLE_SELECTED_COLOR			= 0xFFFFFFFF;

	Rectangle		m_header;
	Text				m_title;
	int					m_selected;

	public HelpMenuButton( Osd o, Style sty, float x, float y, int pri, String string, int command )
	{
		super(o, sty, x, y, pri, "", command);
		
//		m_header = osd.createRectangle(x+0.14, y-0.07, 0.5, 0.075, 0, new ResourceRef(frontend:0x0028r));
//		m_title = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, x+0.14, y-0.11 );

		m_header = osd.createRectangle(x/*+0.14*/, y, 0.5, 0.075, 0, new ResourceRef(frontend:0x0028r));

		float ty = y-(Text.getFontSize( Frontend.smallFont ))/(Config.video_y*osd.vpHeight);
		m_title = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, x/*+0.14*/, ty /*y-0.0399*/ );
		m_title.changeText(string);
		if ( !m_selected )
			m_title.changeColor(TITLE_UNSELECTED_COLOR);
		else
			m_title.changeColor(TITLE_UNSELECTED_COLOR2);
	}

	public void focus()
	{
		super.focus();

		m_title.changeColor(TITLE_SELECTED_COLOR);
	}

	public void unfocus()
	{
		super.unfocus();

		if ( !m_selected )
			m_title.changeColor(TITLE_UNSELECTED_COLOR);
		else
			m_title.changeColor(TITLE_UNSELECTED_COLOR2);
	}

	public void setTitle(String title)
	{
		m_title.changeText(title);
	}

	public void click()
	{
		sendEvent(Event.F_GAD_LCLICK);
	}

	public void setSelected(int sel)
	{
		m_selected = sel;

		if ( !m_selected )
			m_title.changeColor(TITLE_UNSELECTED_COLOR);
		else
			m_title.changeColor(TITLE_UNSELECTED_COLOR2);
	}
}
