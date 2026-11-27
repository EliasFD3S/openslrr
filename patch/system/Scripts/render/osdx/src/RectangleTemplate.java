package java.render.osdx;

import java.render.*;
import java.util.resource.*;

// java.render.RectangleTemplate with a colour (CRC 2005 API).
public class RectangleTemplate extends java.render.RectangleTemplate
{
	int	color = 0xFFFFFFFF;

	public RectangleTemplate( float w, float h, ResourceRef texture )
	{
		super( w, h, texture );
	}

	// Solid colour: the empty texture, tinted by the host.
	public RectangleTemplate( float w, float h, int c )
	{
		super( w, h, Osd.RRT_EMPTY );
		color = c;
	}

	public RectangleTemplate( float w, float h, ResourceRef texture, int c )
	{
		super( w, h, texture );
		color = c;
	}
}
