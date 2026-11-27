package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;

public class MultiChoiceNumericArrow extends MultiChoiceNumeric implements EventReceiver
{
	final static int MCA_EVENT_DEC = 1;
	final static int MCA_EVENT_INC = 2;

	Button			m_leftarrow;
	Button			m_rightarrow;

	public MultiChoiceNumericArrow( Osd o, Style sty, Style LBsty, Style RBsty, float x, float y, int pri, String string, int command, int min, int levels, int step, int v, int flags)
	{
		super(o, sty, x, y, pri, string, command, min, levels, step, v, flags);

		float		w = sty.width*sty.aspect*sty.aspect/osd.vpAspect/osd.vpWidth;
		float		lw = LBsty.width*LBsty.aspect*LBsty.aspect/osd.vpAspect;
		float		rw = RBsty.width*RBsty.aspect*RBsty.aspect/osd.vpAspect;

		m_leftarrow = osd.createButton( LBsty, this.x - w/2 , y+0.010 , MCA_EVENT_DEC, null ).changeEventHandler(this);
		m_rightarrow = osd.createButton( RBsty, this.x + w/2 + rw , y+0.010, MCA_EVENT_INC, null ).changeEventHandler(this);
		m_leftarrow.nokeyfocus = 1;
		m_rightarrow.nokeyfocus = 1;
		setLevel(v);
	}

	public void handleEvent( Event e )
	{
		switch( e.flags )
		{
			case (Event.F_GAD_FOCUS):
			case (Event.F_GAD_UNFOCUS):
				break;
			default:
				switch ( e.cmd )
				{
					case (MCA_EVENT_DEC):
						decrease();
						break;
					case (MCA_EVENT_INC):
						increase();
						break;
				}
				break;
		}
	}

	public void click() 
	{
	}

	public void customHL(ResourceRef LBHL, ResourceRef RBHL)
	{
		m_leftarrow.customHL(LBHL);
		m_rightarrow.customHL(RBHL);
	}

	public int setLevel( int v )
	{
		int			ret = super.setLevel(v);

		if ( m_leftarrow && m_rightarrow )
		{
			if ( !disabled )
			{
				m_leftarrow.enable();
				m_rightarrow.enable();
			}
			if ( !(m_flags & MCAF_LOOP) )
			{
				if ( m_level <= 0 )
					m_leftarrow.disable();
				if ( (m_level+1) >= m_levels )
					m_rightarrow.disable();
			}
		}
	
		return ret;
	}

	public Gadget disable()
	{
		super.disable();
		m_leftarrow.disable();
		m_rightarrow.disable();

		return this;
	}

	public void enable()
	{
		super.enable();
		setLevel(m_level);
	}

}
