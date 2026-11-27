package java.openslrr;

import java.util.resource.*;

// Exact poses on a script camera (a GameRef of type GameRef.RID_CAMERA) for
// mods (#487). While a pose is set it is the camera's whole view: the move,
// look, dist and roll commands do not apply until clearPose.
public class CameraControl
{
	// Eye, focus and up in world metres; fovDeg is the vertical view angle,
	// 0 (or less) keeps the camera's own (the zoom command, 60 by default).
	// No smoothing. A null or zero up is world up. A NaN or infinite value
	// anywhere rejects the call and the previous pose stays.
	public static native void setPose( GameRef cam, Vector3 eye, Vector3 at, Vector3 up, float fovDeg );

	// Eye and focus as world-space offsets from the car's drawn origin. The
	// engine adds the origin when it draws the frame, so the camera stays
	// exactly on the drawn car at any frame rate. The offsets are not turned
	// with the car: use setPoseLocal for a camera fixed to the body.
	public static native void setPoseRelative( GameRef cam, GameRef car, Vector3 eyeOffset, Vector3 atOffset, Vector3 up, float fovDeg );

	// Eye, focus and up in the car's chassis frame (metres; x right, y up,
	// z back; the frame of CarState.boxMin/boxMax). The engine turns and
	// places them with the car's drawn frame when it draws, so a hood or
	// bumper camera stays on the body while it turns, pitches and banks. A
	// null or zero up is the car's up.
	public static native void setPoseLocal( GameRef cam, GameRef car, Vector3 eyeLocal, Vector3 atLocal, Vector3 upLocal, float fovDeg );

	// Back to the stock camera behaviour, starting from the last pose.
	public static native void clearPose( GameRef cam );

	// Fraction 0..1 of the segment from -> to that a sphere of this radius
	// (at most 2 m) travels before it touches the scenery; 1 is clear.
	public static native float sweep( Vector3 from, Vector3 to, float radius );
}
