package java.render.osdx;

// OpenSLRR addition (OSD v2): something an Osd moves every frame.
// Osd.addAnimated registers it; Osd.animate(dt) calls tick(dt).
public interface Animated
{
	public void tick( float dt );
}
