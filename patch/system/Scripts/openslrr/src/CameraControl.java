package java.openslrr;

import java.util.resource.*;

// Exact poses on a script camera (a GameRef of type GameRef.RID_CAMERA) for
// mods (#487). While a pose is set it is the camera's whole view: the move,
// look, dist and roll commands do not apply until clearPose.
public class CameraControl
{
	// Eye, focus and up in world metres; fovDeg is the vertical view angle,
	// 0 (or less) keeps the camera's own (the zoom command, 60 by default).
	// No smoothing. A zero up is world up.
	public static native void setPose( GameRef cam, Vector3 eye, Vector3 at, Vector3 up, float fovDeg );

	// Eye and focus as world-space offsets from the car's drawn origin. The
	// engine adds the origin when it draws the frame, so the camera stays
	// exactly on the drawn car at any frame rate; the script rotates the
	// offsets itself (CarState.drawnOri).
	public static native void setPoseRelative( GameRef cam, GameRef car, Vector3 eyeOffset, Vector3 atOffset, Vector3 up, float fovDeg );

	// Back to the stock camera behaviour, starting from the last pose.
	public static native void clearPose( GameRef cam );

	// Fraction 0..1 of the segment from -> to that a sphere of this radius
	// (at most 2 m) travels before it touches the scenery; 1 is clear.
	public static native float sweep( Vector3 from, Vector3 to, float radius );
}
