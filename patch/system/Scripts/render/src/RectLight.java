package java.render;

import java.util.resource.*;

// Rectangular area light (#235), such as a fluorescent fitting. Drawn as a
// spot light for now; the size is recorded for a later area-light renderer.
public class RectLight extends SpotLight
{
	public RectLight( ResourceRef parent ){ super(); create( parent, KIND_RECT ); }

	// Width and height in metres, 0 to 100.
	public native void setSize( float width, float height );
}
