package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.resource.*;

public class ProgressBar extends Gadget
{
	final static ResourceRef	RRT_INLAY	= new ResourceRef(/*frontend:0x001Er*/);

	float		x0, x1;
	float		value;

	float		a=0.0, b=1.0;	//range bounds
	int			ticks;			//0 - unlimited

	Rectangle	progRect;

	public ProgressBar( Osd o, Style sty, float x, float y, int pri, String string, float v )
	{
		super( o, sty, x, y, pri, string, -1 );
		handler = null;

		x0 = this.x-0.5*w/osd.vpWidth;
		x1 = this.x+0.5*w/osd.vpWidth;

		setValue(v);
	}

	public void setRange( float a, float b )
	{
		this.a = a;
		this.b = b;
	}

	public void setTicks( int t )
	{
		ticks=t;
	}

	public void setValue( float v )
	{
		if( a < b )
		{
			if( v > b )
				value = b;
			else
			if( v < a )
				value = a;
			else
				value = v;
		}
		else
		{
			if( v > a )
				value = a;
			else
			if( v < b )
				value = b;
			else
				value = v;
		}
		
		if( ticks > 1)
		{
			float tickRange = (b-a)/(ticks-1);
			int	castint = (value-a)/tickRange;
			float diff = (value-a) - tickRange*castint;
			if( diff > 0.0 )
			{
				if( diff < tickRange/2.0 )
					value = a+tickRange*castint;
				else
					value = a+tickRange*(castint+1);
			}
		}

		float factor = (value-a)/(b-a);
		float dx = x0+(factor*(x1-x0)/2.0);

		Vector3 pos = osd.convertTextCoordinates( dx, y, pri+1 );

		float newWidth = rect.rt.width*factor;
		if( !progRect || progRect.rt.width - newWidth > 0.02 || progRect.rt.width - newWidth < -0.02 )
		{
			RectangleTemplate rt = new RectangleTemplate( newWidth, rect.rt.height, RRT_INLAY );

			if( progRect )
				progRect.finalize();
			progRect = new Rectangle( rect.root, pos, rt);
		}


	}
}
