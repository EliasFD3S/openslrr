package java.render.osdx;

import java.render.*;
import java.util.resource.*;

// java.render.Rectangle with a colour (CRC 2005 API), drawn by OsdHost.tint.
public class Rectangle extends java.render.Rectangle
{
	int	color = 0xFFFFFFFF;
	// 1: placed by its owner every frame (LevelBar); OsdAnimator leaves it.
	public	int	fixed;

	public Rectangle( ResourceRef r, Vector3 p, java.render.RectangleTemplate tmpl )
	{
		super( r, p, tmpl );
		if( tmpl instanceof RectangleTemplate )
			setColor( ((RectangleTemplate)tmpl).color );
	}

	public void setColor( int c )
	{
		color = c;
		if( ins )
			OsdHost.tint( ins, c );
	}

	public int getColor()
	{
		return color;
	}

	public void changeTexture( ResourceRef newTexture )
	{
		super.changeTexture( newTexture );
		if( ins )
			OsdHost.tint( ins, color );
	}
}
