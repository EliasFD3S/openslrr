package java.openslrr;

import java.util.resource.*;
import java.render.*;

// A driving camera supplied by a mod (#487). Register an instance with
// Cameras.register; when it is selected, the patched Track hands it the
// follow view: the change-view key cycles its views and LOOK BACK goes to
// lookBack. Override what you need; every method has a harmless default.
public abstract class CameraProvider
{
	// The world (map) the car drives in, set by Track before activate: the
	// parent for the provider's camera.
	public GameRef world;

	public CameraProvider()
	{
	}

	// Shown in the options and in the "Camera:" message.
	public String name()
	{
		return "MOD CAMERA";
	}

	public int views()
	{
		return 1;
	}

	public String viewName( int v )
	{
		return name();
	}

	// Track has hidden its own camera. Create a GameRef.RID_CAMERA in world, render
	// it to vp ("render " + vp.id() + " 0 0 1 " + flags), drive it every
	// frame (an animate() callback) and follow car, starting in view.
	public void activate( GameRef car, Viewport vp, int view )
	{
	}

	// Another camera takes over: destroy the camera made in activate. Look
	// back counts as released.
	public void deactivate()
	{
	}

	// The change-view key chose another view while active.
	public void setView( int view )
	{
	}

	// LOOK BACK pressed (1) or released (0) while active.
	public void lookBack( int pressed )
	{
	}
}
