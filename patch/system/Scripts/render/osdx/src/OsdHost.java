package java.render.osdx;

import java.util.resource.*;

// OSD v2 host API (docs/ui/osd-v2.md). An attached Osd is drawn as written
// and its hit boxes send EVENT_CURSOR to their Group; no host heuristics.
public class OsdHost
{
	public static native void attach( GameRef osd );
	public static native void tint( RenderRef rectangle, int argb );
	// Alpha factor 0..1 on top of the tint (fades).
	public static native void fade( RenderRef rectangle, float factor );
	// Half width and height in screen units (-1..1), without a new RenderRef.
	public static native void resize( RenderRef rectangle, float halfWidth, float halfHeight );
}
