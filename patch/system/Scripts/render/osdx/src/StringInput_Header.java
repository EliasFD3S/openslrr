package java.render.osdx;

import java.render.*;
import java.util.resource.*;
import java.util.*;
import java.io.*;

class StringInput_Header extends StringInput
{
	final static int SIF_NOHEADERBACK				= 1024;

	final static int TITLE_UNSELECTED_COLOR		= 0xFFFFAE00;
	final static int TITLE_SELECTED_COLOR			= 0xFFFFAE00;

	Rectangle		m_header;
	Text				m_title;

	int					m_flags;

	public StringInput_Header(Osd o, Style sty, float x, float y, int pri, String string, int command, String v, int secure, int flags)	
	{
		super(o, sty, x, y, pri, null, command, v, secure);

		m_flags = flags;

		// szoveg kozepe
		float text_y = y - 0.5*Text.getHeight(Frontend.smallFont, osd.getViewport() );
		float header_x = 0;//.14;

		if ( !(m_flags & SIF_NOHEADERBACK) )
			m_header = osd.createRectangle(x+header_x, y-0.1, 0.5, 0.075, 0, new ResourceRef(frontend:0x0028r));
		text_y -= 0.1;

		m_title = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, x+header_x, text_y );
		m_title.changeText(string);
		m_title.changeColor(TITLE_UNSELECTED_COLOR);
	}

	public void focus()
	{
		super.focus();

		m_title.changeColor(TITLE_SELECTED_COLOR);
	}

	public void unfocus()
	{
		super.unfocus();

		m_title.changeColor(TITLE_UNSELECTED_COLOR);
	}

	public void setTitle(String title)
	{
		m_title.changeText(title);
	}
}
