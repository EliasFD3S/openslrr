package java.render.osdx;

import java.render.*;
import java.util.resource.*;

public class StartLamp
{
	final static ResourceRef	startlamptex_holder = new ResourceRef(frontend:0x00A2r);
	final static ResourceRef	startlamptex_lamp = new ResourceRef(frontend:0x012Br);

	Osd						m_osd;
	float					m_x, m_y;
	float					m_width, m_height;
	int						m_priority;


	float					m_column_gap;
	float					m_column_width;
	float					m_column_height;
	float					m_lamp_size_x;
	float					m_lamp_size_y;
	float[]				m_lamp_offset_y = new float[3];

	int						m_fademaxcounter;	// hany animate step alatt fade-el at
	int						m_fadecounter;		

	// 0 - [---] kikapcsolt
	// 1 - [X--]
	// 2 - [XX-]
	// 3 - [XXX]
	// 4 - [OOO]
	Group					m_group;
	int						m_groupid;

	int						m_state;
	int						m_laststate;
	int[]					m_colors = new int[5*3];

	// visuals
	Rectangle							m_rect_holder_left;
	Rectangle							m_rect_holder_right;
	Rectangle[]						m_rect_lamp_left = new Rectangle[3];
	Rectangle[]						m_rect_lamp_right = new Rectangle[3];

	public StartLamp(Osd o, float x, float y, float w, float h, int pri)
	{
		// default fekete mindegyik
		m_osd = o;
		m_x = x;
		m_y = y;
		m_width = w;
		m_height = h;
		m_priority = pri;

		m_laststate = 0;
		m_fademaxcounter = 8;

		setState(0);

		// state 0 - [---] kikapcsolt
		int			colblack	= 0xFF404040; 
		int			colred		= 0xFFFF0000; 
		int			colgreen	= 0xFF00FF00; 

		m_colors[0*3+0] = colblack;
		m_colors[0*3+1] = colblack;
		m_colors[0*3+2] = colblack;

		// state 1 - [X--]
		m_colors[1*3+0] = colred;
		m_colors[1*3+1] = colblack;
		m_colors[1*3+2] = colblack;

		// state 2 - [XX-]
		m_colors[2*3+0] = colred;
		m_colors[2*3+1] = colred;
		m_colors[2*3+2] = colblack;

		// state 3 - [XXX]
		m_colors[3*3+0] = colred;
		m_colors[3*3+1] = colred;
		m_colors[3*3+2] = colred;

		// state 4 - [XXO]
		m_colors[4*3+0] = colgreen;
		m_colors[4*3+1] = colgreen;
		m_colors[4*3+2] = colgreen;


		m_column_gap = 4.0/640.0;
		m_column_width = (m_width-m_column_gap)/2.0;
		m_column_height = m_height;
		m_lamp_size_x = m_column_width*0.52;
		m_lamp_size_y = m_column_height*0.26;
		m_lamp_offset_y[0] = 0.195 * m_column_height;
		m_lamp_offset_y[1] = 0.5 * m_column_height;
		m_lamp_offset_y[2] = 0.815 * m_column_height;

		m_group = m_osd.groups.lastElement();
		create(m_x, m_y);
		m_group.misc.addElement( this );
		m_groupid = m_osd.endGroup();

		setPos(m_x, m_y);

		setState(0);

		for ( int i=0; i<3; i++ )
		{
			m_rect_lamp_left[i].setColor(m_colors[m_state*3+i]);
			m_rect_lamp_right[i].setColor(m_colors[m_state*3+i]);
		}
	}

	public void destroy()
	{
		if ( m_rect_holder_left )
		{
			m_rect_holder_left.finalize();
			m_osd.deleteRectangle(m_rect_holder_left);
		}

		int				il;
		for ( il=0; il<3; il++ ) 
			if ( m_rect_lamp_left[il] )
			{
				m_rect_lamp_left[il].finalize();
				m_osd.deleteRectangle(m_rect_lamp_left[il]);
			}

		if ( m_rect_holder_right )
		{
			m_rect_holder_right.finalize();
			m_osd.deleteRectangle(m_rect_holder_right);
		}

		for ( il=0; il<3; il++ ) 
			if ( m_rect_lamp_right[il] )
			{
				m_rect_lamp_right[il].finalize();
				m_osd.deleteRectangle(m_rect_lamp_right[il]);
			}

//		m_group.finalize();
		m_osd.remGroup(m_groupid);
	}

	public void show()
	{
	}

	public void hide()
	{
	}

	public void setState(int state)
	{
		m_laststate = m_state;
		m_state = state;

		m_fadecounter = m_fademaxcounter;
	}

	public void animate()
	{
		if ( m_state != m_laststate )
		{
			// setcolor
			float			f = ((float)(m_fademaxcounter-m_fadecounter))/m_fademaxcounter;
			for ( int i=0; i<3; i++ )
			{
				float			r1, g1, b1, a1;
				float			r2, g2, b2, a2;
				int				r, g, b, a;

				a1 = (float)((m_colors[m_laststate*3+i]>>24)&0xFF);
				r1 = (float)((m_colors[m_laststate*3+i]>>16)&0xFF);
				g1 = (float)((m_colors[m_laststate*3+i]>>8)&0xFF);
				b1 = (float)((m_colors[m_laststate*3+i])&0xFF);

				a2 = (float)((m_colors[m_state*3+i]>>24)&0xFF);
				r2 = (float)((m_colors[m_state*3+i]>>16)&0xFF);
				g2 = (float)((m_colors[m_state*3+i]>>8)&0xFF);
				b2 = (float)((m_colors[m_state*3+i])&0xFF);

				a = (((int)((1.0-f)*a1 + f*a2))<<24)&0xFF000000;
				r = (((int)((1.0-f)*r1 + f*r2))<<16)&0x00FF0000;
				g = (((int)((1.0-f)*g1 + f*g2))<<8)&0x0000FF00;
				b = (((int)((1.0-f)*b1 + f*b2)))&0xFF;

				m_rect_lamp_left[i].setColor(a|r|g|b);
				m_rect_lamp_right[i].setColor(a|r|g|b);
			}
			m_fadecounter--;

			if ( m_fadecounter < 0 )
				m_laststate = m_state;
		}
	}

	public void create(float px, float py)
	{
		int				il;
		float			x, y;
		float			center, left, top;

		left = px - m_width/2.0;
		top  = py - m_height/2.0;
		x = left + m_column_width/2.0;
		y = top + m_column_height/2.0;
		center = x + m_column_width*0.025;

		m_rect_holder_left = m_osd.createRectangle(x, y, m_column_width, m_column_height, m_priority, startlamptex_holder);
		for ( il=0; il<3; il++ ) 
			m_rect_lamp_left[il] = m_osd.createRectangle(center, top + m_lamp_offset_y[il], m_lamp_size_x, m_lamp_size_y, m_priority+1, startlamptex_lamp, 0xFFFFFFFF);

		x = left + m_column_width + m_column_gap + m_column_width/2.0;
		y = top + m_column_height/2.0;
		center = x + m_column_width*0.025;

		m_rect_holder_right = m_osd.createRectangle(x, y, m_column_width, m_column_height, m_priority, startlamptex_holder);
		for ( il=0; il<3; il++ ) 
			m_rect_lamp_right[il] = m_osd.createRectangle(center, top + m_lamp_offset_y[il], m_lamp_size_x, m_lamp_size_y, m_priority+1, startlamptex_lamp, 0xFFFFFFFF);
	}

	public void setPos(float px, float py)
	{
		int				il;
		float			x, y;
		float			center, left, top;

		left = px - m_width/2.0;
		top  = py - m_height/2.0;

		// left
		{
			x = left + m_column_width/2.0;
			y = top + m_column_height/2.0;
			center = x + m_column_width*0.025;

			Vector3 pos;
			pos  = m_group.convertTextCoordinates( x, y, m_priority );
			m_rect_holder_left.setPos(pos);

			for ( il=0; il<3; il++ )
			{
				pos = m_group.convertTextCoordinates( center, top + m_lamp_offset_y[il], m_priority+1 );
				m_rect_lamp_left[il].setPos(pos);
			}
		}
		
		// right
		{
			x = left + m_column_width + m_column_gap + m_column_width/2.0;
			y = top + m_column_height/2.0;
			center = x + m_column_width*0.025;

			Vector3 pos;
			pos  = m_group.convertTextCoordinates( x, y, m_priority );
			m_rect_holder_right.setPos(pos);

			for ( il=0; il<3; il++ )
			{
				pos = m_group.convertTextCoordinates( center, top + m_lamp_offset_y[il], m_priority+1 );
				m_rect_lamp_right[il].setPos(pos);
			}
		}
	}
}
