package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class MultiChoiceArrow_Header extends MultiChoiceArrow
{
	public	final static int MCAF_NOHEADERBACK				= 1024;

	public	final static int TITLE_UNSELECTED_COLOR		= 0xFFFFAE00;
	public	final static int TITLE_SELECTED_COLOR			= 0xFFFFAE00;

	Rectangle		m_header;
	Text				m_title;

	public MultiChoiceArrow_Header( Osd o, Style sty, Style LBsty, Style RBsty, float x, float y, int pri, String string, int command, int v, Vector chs, int flags )
	{
		super(o, sty, LBsty, RBsty, x, y, pri, "", command, v, chs, flags );
		
		// szoveg kozepe
		float text_y = y - 0.5*Text.getHeight(Frontend.smallFont, osd.getViewport() );
		text_y -= 0.072;

		if ( !(m_flags & MCAF_NOHEADERBACK) )
			m_header = osd.createRectangle(x+0.14, y-0.07, 0.5, 0.075, 0, new ResourceRef(frontend:0x0028r));

		m_title = osd.createText( null, Frontend.smallFont, Text.ALIGN_CENTER, x+0.14, text_y-0.005 /*y-0.10*/ );
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
