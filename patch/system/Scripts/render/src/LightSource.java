package java.render;

import java.util.resource.*;

// OpenSLRR light source (#235). A light is attached to a parent scene object
// (a GameRef, RenderRef or GroundRef already placed in the world). Position
// and direction are in the parent's local space, in metres, like the
// coordinates of RenderRef.setMatrix. The light follows its parent and is
// removed with it, or by destroy().
//
// Defaults: position 0,0,0; direction 0,-1,0; colour white; intensity 24;
// range 20 m; shadows on; enabled. Out-of-range values are clamped by the
// engine; non-finite values are ignored.
//
// Do not instantiate LightSource itself; use PointLight, SpotLight or RectLight.
public class LightSource
{
	final static int KIND_POINT	= 0;
	final static int KIND_SPOT	= 1;
	final static int KIND_RECT	= 2;

	public LightSource(){}

	// Registers the light with the engine. kind is one of KIND_*.
	public native void create( ResourceRef parent, int kind );

	// Position in the parent's local space (metres).
	public native void setPosition( Vector3 pos );
	// Direction of travel of the light in the parent's local space. Normalised
	// by the engine; a zero vector is ignored. Spot and rect lights only.
	public native void setDirection( Vector3 dir );
	// 0xRRGGBB. The alpha byte is ignored.
	public native void setColor( int rgb );
	// Engine units (not lumens), 0 to 1000.
	public native void setIntensity( float intensity );
	// Distance in metres at which the light fades out, 0.2 to 500.
	public native void setRange( float range );
	// 0 off, otherwise on.
	public native void setShadows( int on );
	// 0 off, otherwise on. A disabled light still replaces the stock garage
	// fixtures while its parent is in the world.
	public native void setEnabled( int on );
	// Removes the light. Later calls on this object are ignored.
	public native void destroy();
}
