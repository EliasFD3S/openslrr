package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

public class Gadget
{
	final static ResourceRef	HIGHLIGHT_NORMAL	= new ResourceRef(frontend:0x012Br);
	final static ResourceRef	HIGHLIGHT_DISABLED	= Osd.RRT_NONE/*new ResourceRef(frontend:0x0004r)*/;
	final static float			HIGHLIGHT_NORMAL_SCALE		= 1.0;
	final static float			HIGHLIGHT_DISABLED_SCALE	= 1.0;
	final static float			HIGHLIGHT_ACTIVE_SCALE		= 1.0;
	

	// creation params
	Style				m_style;
	float				m_px;
	float				m_py;

	float				m_offset_y;
	Group				m_group;

	Osd				osd;
	PhysicsRef		phy;	//a fizikai (es input objektum)
	public	Rectangle		rect; //latvany objektum

	float			x, y, w, h;

	int				pri;

	Label			label;

	public	int				cmd;
	EventReceiver	handler;
	String			tooltip;

	public	int				focused;
	public	int				disabled;
	public	int				pressed;

	int				dropEnabled;
	int				noleaverelease;	//leave-re nem kuld release-t!
	public	int				nofocus;
	public	int				nokeyfocus;

	public	int				norectcolormodify;	//nem fogja modositani disablenel a gadget szinet

	public	int				highliteMethod = Osd.HL_LABELCOLOR;
	Rectangle		hr;
	ResourceRef		highliteTexture = HIGHLIGHT_NORMAL;
	int				colorUnselected = Osd.COLOR_UNSELECTED;

	//for the button->slider contact so far
	Gadget			parent;


	//// animate params
	float			animate_timer;		// seconds
	float			animate_length;		// seconds
	int				animate_running;
	//// animate params

	Vector3		animate_end_pos;
	Vector3		animate_start_pos;

	public Gadget( Osd o, Style sty, float px, float py, int pri, String string, int command, float offsety )
	{
		m_style = sty;
		m_px = px;
		m_py = py;

		osd = o;
		cmd = command;
		handler = osd.globalHandler;

		m_offset_y = offsety; //-0.007;
		py += m_offset_y;

		m_group = osd.groups.lastElement();

//		calcPos(osd, sty, m_px, m_py);

		w = sty.width*sty.aspect*sty.aspect/osd.vpAspect;
		float lw = sty.labelwidth*sty.aspect*sty.aspect/osd.vpAspect;

		switch( sty.align )
		{
		case (Text.ALIGN_LEFT):
			x = px+0.5*w;
			break;
		case (Text.ALIGN_RIGHT):
			x = px-0.5*w;
			break;
		default:
			x=px;
		}

		y = py;

		this.pri = pri;

		Vector3 pos = m_group.convertTextCoordinates( x, y, pri );

		if( sty.rt )
		{
			//if( sty.background.id() == frontend:0x000Er )
			//{//nem pecs, pecsség!
			//	osd.createRectangle( x, y, sty.width*sty.aspect, sty.height, pri, Osd.RRT_WHITEALPHA, 0xEE333333);
			//}
			//else
			{
			rect =  new Rectangle( m_group, pos, sty.rt );
			if( sty.usecolor )
				customCU( sty.color );
			}
		}

		phy = new PhysicsRef();
		phy.createBox( m_group, sty.rWidth, sty.rHeight, 0.001, string );	//string used here just for debugging
		phy.setMatrix( pos, null );

		if( string )	//create label if exists
		{
			//-------x
			float tx = x;

			int	p1, p2;
			if( this instanceof Button && !(this instanceof TextButton))	//patch - buttonnal marad a gomb kozepen a text!
			{
				p1=sty.la1;
				p2=sty.la2;
				sty.la1 = sty.la2 = Text.ALIGN_CENTER;
			}

			switch( sty.la1 )
			{
			case (Text.ALIGN_LEFT):
				tx-=w;
				break;
			case (Text.ALIGN_RIGHT):
				tx+=w;
				break;
			}

			switch( sty.la2 )
			{
			case (Text.ALIGN_LEFT):
				tx-=lw*0.45;
				break;
			case (Text.ALIGN_RIGHT):
				tx+=lw*0.45;
				break;
			}

			//-------y
			//a font def fogoja a teteje, nekunk a kozepe kell
			float ty = y-0.5*Text.getHeight( sty.charset, osd.getViewport() );

			Text txt = osd.createText( string, sty.charset, sty.la2, tx, ty );
			txt.changeColor( colorUnselected );

			setLabel( txt );

			if( this instanceof Button && !(this instanceof TextButton))
			{
				sty.la1=p1;
				sty.la2=p2;
			}
		}

		animate_length = 0.5;
		animate_timer = animate_length;

		animate_running = osd.gadgetIntro;


		animate_start_pos = new Vector3();
		animate_end_pos = new Vector3();

		// The CRC engine also required label.txt.renderinst.id(); the OpenSLRR
		// host keeps text without that instance, and Text.getPos is valid.
		if ( label && label.txt )
		{
			animate_end_pos = label.txt.getPos();

			int				vscroll = 0;
			// check vertical margins
			if ( animate_end_pos.y < osd.scrollmargin_top )
			{
				// felulrol jon
				vscroll = 1;
				animate_start_pos.y = -2.0;
			} else
			if ( animate_end_pos.y > osd.scrollmargin_bottom )
			{
				// alulrol jon
				vscroll = 1;
				animate_start_pos.y = 2.0;
			}

			if ( vscroll == 0 )
			{
				if ( animate_end_pos.x < osd.scrollmargin_left )
				{
					// balrol jol
					animate_start_pos.x = -2.0;
				} else
				if ( animate_end_pos.x >= osd.scrollmargin_right )
				{
					// jobbrol jol
					animate_start_pos.x = 2.0;
				}
			}

			if ( vscroll == 0 )
			{
				// horizontal scroll setup
				animate_start_pos.y = animate_end_pos.y;
				animate_length = animate_length*(0.5*(animate_end_pos.y+1.0));
				animate_timer = animate_length;
			} else
			{
				// vertical scroll setup
				animate_start_pos.x = animate_end_pos.x;
//				animate_length = animate_length*(0.5*(animate_end_pos.y+1.0));
//				animate_timer = animate_length;
			}
		}

		if (sty.highlight)
			customHL(sty.highlight);

		m_group.gadget.addElement( this );
	}

	public Gadget( Osd o, Style sty, float px, float py, int pri, String string, int command )
	{
		this( o, sty, px, py, pri, string, command, 0.0 );
	}

	public void recalcPos(Osd o, Style sty, float px, float py)
	{
		py += m_offset_y;

		w = sty.width*sty.aspect*sty.aspect/o.vpAspect;

		switch( sty.align )
		{
			case (Text.ALIGN_LEFT):
				x = px+0.5*w;
				break;
			case (Text.ALIGN_RIGHT):
				x = px-0.5*w;
				break;
			default:
				x=px;
		}

		y = py;

		Vector3 pos = m_group.convertTextCoordinates( x, y, this.pri );

		if ( sty.rt && rect )
			rect.setPos(pos);

		phy.setMatrix( pos, null );

		if( label && label.txt )
		{
			//-------x
			float tx = x;

			int	p1, p2;
			if( this instanceof Button && !(this instanceof TextButton))	//patch - buttonnal marad a gomb kozepen a text!
			{
				p1=sty.la1;
				p2=sty.la2;
				sty.la1 = sty.la2 = Text.ALIGN_CENTER;
			}

			switch( sty.la1 )
			{
				case (Text.ALIGN_LEFT):
					tx-=w;
					break;
				case (Text.ALIGN_RIGHT):
					tx+=w;
					break;
			}

			switch( sty.la2 )
			{
				case (Text.ALIGN_LEFT):
					tx-=w*0.45;
					break;
				case (Text.ALIGN_RIGHT):
					tx+=w*0.45;
					break;
			}

			//-------y
			//a font def fogoja a teteje, nekunk a kozepe kell
			float ty = y-0.5*Text.getHeight( sty.charset, osd.getViewport() );

			if( this instanceof Button && !(this instanceof TextButton))
			{
				sty.la1=p1;
				sty.la2=p2;
			}

			if ( label && label.txt )
				label.txt.setPos(tx, ty);
		}
	}

	public void setOffsety(float oy)
	{
		m_offset_y = oy;
		recalcPos(osd, m_style, m_px, m_py);
	}

	//handler megvaltoztatasara
	public Gadget changeEventHandler( EventReceiver er )
	{
		handler = er;
		return this;
	}

	public Gadget customHL( ResourceRef hlTex )
	{
		highliteTexture = hlTex;
		highliteMethod|=Osd.HL_BACKLIGHT;
		return this;
	}

	public Gadget customCU( int argb )
	{
		colorUnselected = argb;
		if( !focused && !pressed && !disabled && (highliteMethod & Osd.HL_LABELCOLOR && label) )
			label.txt.changeColor( argb );
			
		return this;
	}

	public void setLabel( Text txt )
	{
		label = new Label( txt );
	}

	public Text getLabel()
	{
		if (label)
			return label.txt;
		return null;
	}

	public String getLabelText()
	{
		if( label )
			return label.txt.text;

		return null;
	}

	public void changeLabelText( String newtxt )
	{
		if( label )
			label.txt.changeText( newtxt );
	}

	public void changeTexture( ResourceRef newTexture )
	{
		if( rect )
			rect.changeTexture( newTexture );
	}

	public void setToolTip( String t )
	{
		tooltip = t;
	}

	public void enableDrop()
	{
		dropEnabled = 1;
	}

	public void drop( GameRef what )
	{
		Event e = new Event( Event.F_GAD_DROP );
		e.data = what;
		sendEvent( e );
	}

	public void drop( Gadget what )
	{
		Event e = new Event( Event.F_GAD_DROP );
		e.data = what;
		sendEvent( e );
	}

	public Gadget disable()
	{ 
		disabled = 1; 

		if( osd.pointedGadget == this )
			osd.unFocusGadget();

		if( label )
			label.txt.changeColor( Osd.COLOR_DISABLED );

		if (rect && !norectcolormodify)
			rect.setColor(Osd.COLOR_DISABLED);

		return this;
	}
	public void enable()
	{ 
		disabled = 0;
		 
		if( osd.pointedGadget != this )
		if( label )
				label.txt.changeColor( colorUnselected );

		if (rect && !norectcolormodify)
			rect.setColor(colorUnselected);
	}

	public void focus()
	{ 
		if( highliteMethod & Osd.HL_LABELCOLOR && label )
			label.txt.changeColor( Osd.COLOR_SELECTED );

		if( highliteMethod & Osd.HL_BACKLIGHT && rect )
		{
			Vector3 p2 = new Vector3( rect.pos );
			if( !disabled )
			{
				p2.z+= Osd.LAYER_FACTOR*0.5; //fel prioritasi szint
				hr = new Rectangle( rect.root, p2, new RectangleTemplate( rect.rt.width*HIGHLIGHT_NORMAL_SCALE, rect.rt.height*HIGHLIGHT_NORMAL_SCALE, highliteTexture ) );
			}
			else
			{
				p2.z+=0.0005; //fel prioritasi szint
				hr = new Rectangle( rect.root, p2, new RectangleTemplate( rect.rt.width*HIGHLIGHT_DISABLED_SCALE, rect.rt.height*HIGHLIGHT_DISABLED_SCALE, HIGHLIGHT_DISABLED ) );
			}
		}
		focused = 1;

		sendEvent( Event.F_GAD_FOCUS );
	}
	public void unfocus()
	{ 
		if( highliteMethod & Osd.HL_LABELCOLOR && label )
			label.txt.changeColor( colorUnselected );

		if( hr )
		{
			hr.finalize();
			hr=null;
		}
		focused = 0;

		sendEvent( Event.F_GAD_UNFOCUS );
	}

	public void press()
	{ 
		if( !pressed )
		{
			pressed=1; 
			
			if( highliteMethod & Osd.HL_LABELCOLOR && label )
				label.txt.changeColor( Osd.COLOR_PRESSED );

			if( hr )
			{
				hr.finalize();
				hr=null;
			}
			if( highliteMethod & Osd.HL_BACKLIGHT && rect )
			{
				Vector3 p2 = new Vector3( rect.pos );
				p2.z+=0.0005; //fel prioritasi szinttel hatrebb :)
				hr = new Rectangle( rect.root, p2, new RectangleTemplate( rect.rt.width*HIGHLIGHT_ACTIVE_SCALE, rect.rt.height*HIGHLIGHT_ACTIVE_SCALE, highliteTexture ) );
			}

			osd.pressEffect();
		}
	}
	public void release()
	{ 
		if( pressed )
		{
			pressed=0; 

			if( highliteMethod & Osd.HL_LABELCOLOR && label )
				label.txt.changeColor( Osd.COLOR_SELECTED );

			if( hr )
			{
				hr.finalize();
				hr=null;
			}
			if( highliteMethod & Osd.HL_BACKLIGHT && rect )
			{
				Vector3 p2 = new Vector3( rect.pos );
				p2.z+=0.0005; //fel prioritasi szinttel hatrebb :)
				hr = new Rectangle( rect.root, p2, new RectangleTemplate( rect.rt.width*HIGHLIGHT_NORMAL_SCALE, rect.rt.height*HIGHLIGHT_NORMAL_SCALE, highliteTexture ) );
			}

			osd.pressEffectStop();
		}
	}

	public void click(){}
	public void stop(){}	//az actGadget-et hasznalo gadok ertesitese arrol, hogy elveszitik az input focust

	public void increase(){}
	public void decrease(){}


	public void sendEvent( int eventType )
	{
		if( handler && (osd.eventFilter&eventType) )
		{
			Event e =  new Event( cmd, eventType );
			e.gadget = this;
			if( !((Object)handler).sema )
			{
				//szuper-ratyi vedelem!! mashonnan is hivodhat a handleevent:
				//a, mindenhol at kellene irni, hogy egy mas nevu vedett metodust hivjon
				//b, megcsinalni a synchronyzed metodusokat
				((Object)handler).sema = 1;
				handler.handleEvent( e );
				((Object)handler).sema = 0;
			}
			//else drop...?
		}
	}

	public void sendEvent( Event e )
	{
		if( handler && (osd.eventFilter&e.flags) )
		{
			e.gadget = this;
			handler.handleEvent( e );
		}
	}

	public Vector3 getHotSpot()
	{
		return phy.getPos();
	}


	// Plays the label slide-in again from its start position.
	public void restartAnimate()
	{
		if( animate_end_pos && label && label.txt )
		{
			animate_timer = animate_length;
			animate_running = 1;
			label.txt.setPos( animate_start_pos.x, animate_start_pos.y );
		}
	}

	public void animate(float dt)
	{
		if ( animate_running == 0 )
			return;

		animate_timer -= dt;

		if ( animate_timer <0 )
			animate_timer = 0;

		if ( animate_timer == 0 )
			animate_running = 0;
			
		float				time = ((float)(animate_length-animate_timer))/animate_length;

		if ( label && label.txt && animate_end_pos)
		{
			float			x = time*animate_end_pos.x + (1.0-time)*animate_start_pos.x;
			float			y = time*animate_end_pos.y + (1.0-time)*animate_start_pos.y;

			label.txt.setPos(x, y);
		}
	}
}
