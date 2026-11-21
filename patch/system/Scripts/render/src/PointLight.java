package java.render;

import java.util.resource.*;

// Omnidirectional light (#235). The renderer currently draws one point light
// per interior: the first one created wins.
public class PointLight extends LightSource
{
	public PointLight( ResourceRef parent ){ super(); create( parent, KIND_POINT ); }
}
