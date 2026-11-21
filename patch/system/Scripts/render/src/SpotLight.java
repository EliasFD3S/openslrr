package java.render;

import java.util.resource.*;

// Cone light (#235), pointing along setDirection.
public class SpotLight extends LightSource
{
	public SpotLight(){}
	public SpotLight( ResourceRef parent ){ super(); create( parent, KIND_SPOT ); }

	// Inner and outer half-angles in degrees (default 65 and 80). The current
	// renderer keeps the inner edge at 65 degrees and clamps the outer one to
	// 65.5 to 89 degrees.
	public native void setCone( float innerDeg, float outerDeg );
}
