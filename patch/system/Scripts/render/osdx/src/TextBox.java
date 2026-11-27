package java.render.osdx;

import java.render.*;
import java.util.*;
import java.util.resource.*;

public class TextBox
{
	Group		parent;
	ResourceRef charset;
	Viewport	vp;
	float		x, y, width;
	float		height;

	int			align;
	int			color=Text.DEF_COLOR;

	ResourceRef	background;
	Rectangle	rect;

	Vector		texts;
	public	String		text;

	public TextBox( Group parent_, ResourceRef charset_, Viewport vp_, int align_, String text, float left_, float top_, float width_, ResourceRef bg )
	{
		parent = parent_;
		charset = charset_;
		vp = vp_;
		align = align_;
		x = left_;
		y = top_;
		width = width_;
		background = bg;

		changeText( text );
	}

	public void appendText( String txt, int cutlines )
	{
		text += txt;
		refreshText(cutlines);
	}

	public void changeText( String txt )
	{
		text = txt;
		refreshText(0);
	}

	public void refreshText( int cutlines )
	{
		height = 0.0;

		if( texts )
		{
			Text t;
			for( int i=0; i<texts.size(); i++ )
			{
				t = texts.elementAt( i );
				t.finalize();
			}
			texts=null;

			if( background )
				rect.finalize();
		}

		if( text )
		{
			texts = new Vector();

			StringTokenizer tr = new StringTokenizer( text, " " );
			String token;

			String delimiter = " ";
			float delimiterWidth = Text.getWidth( delimiter, charset, vp );

			String line;
			int lineNumber, cut;
			float w, ww;
			float maxWidth;
			float lineheight = Text.getHeight( charset, vp );
			while( (token = tr.nextToken())!=null || line!=null )
			{
				ww = Text.getWidth( token, charset, vp );
				if( w+ww > width || token==null || token == "\n" )
				{	//create a line
					if( align == Text.ALIGN_LEFT )
					{
						texts.addElement( new Text( parent, charset, line, x, y +lineNumber*lineheight ));
					}
					else
					if( align == Text.ALIGN_RIGHT )
					{
						texts.addElement( new Text( parent, charset, line, x+(width-w), y +lineNumber*lineheight ));
					}
					else
					if( align == Text.ALIGN_CENTER )
					{
						texts.addElement( new Text( parent, charset, line, x+(width-w)/2, y +lineNumber*lineheight ));
					}

					if( maxWidth < w )
						maxWidth = w;

					w=0.0;
					line=null;
					lineNumber++;

					if (cutlines > 0 && lineNumber > cutlines)
					{	//scroll up, cut front of text
						texts.elementAt(0).finalize();
						texts.removeElementAt(0);
						lineNumber--;
						cut++;
					}

					if(	token == "\n" )
						token=null;
				}
				w+=ww;
				if( token )
				{
					if( line )
					{
						line = line+delimiter+token;
						w+=delimiterWidth;
					}
					else
						line=token;
				}

			}

			if (cut)
			{//update positions and recreate text
				text = null;
				for (int i=0; i<texts.size(); i++)
				{
					Text txt = (Text)texts.elementAt(i);
					Vector3 pos = txt.getPos();
					txt.setPos(pos.x, y+i*lineheight );
					if (text)
						text += " \n "+txt.text;
					else
						text = txt.text;
				}
			}

			height = lineNumber*lineheight;

			changeColor( color );

			if( background )
				rect = parent.osd.createRectangle( x+width/2, y+height/2, width*1.1, height*1.1, 0, background );
		}
	}

	public float getHeight()
	{
		return height;
	}

	public void changeColor( int color_ )
	{
		color=color_;

		if( texts )
		{
			for( int i=0; i<texts.size(); i++ )
			{
				Text txt = texts.elementAt( i );
				if( txt )
					txt.changeColor( color );
			}
		}
	}
}
