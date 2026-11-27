package java.openslrr;

import java.util.resource.*;

// Read-only car state for mods (#487). Every method takes the car's GameRef,
// as Track.cameraTarget or player.car. A GameRef that is not a car gives
// null (vectors), 0 or 0.0. Units: metres, m/s, rad/s; world axes, y up.
public class CarState
{
	// The pose drawn this frame: the car between its last two physics steps.
	// With the free-running physics of the game the frame's poses are
	// published after scripts run, so this is the pose of the previous frame;
	// use CameraControl.setPoseRelative to place a camera on the drawn car.
	public static native Vector3 drawnPos( GameRef car );
	public static native Ypr drawnOri( GameRef car );

	// World linear and angular velocity of the chassis (last physics step).
	public static native Vector3 vel( GameRef car );
	public static native Vector3 angVel( GameRef car );

	// 1 while the nitrous burns.
	public static native int nitro( GameRef car );

	// Wheels touching the ground, 0..4.
	public static native int wheelsOnGround( GameRef car );

	// Extent of the car's meshes in its chassis frame (x right, y up, z back),
	// relative to the chassis origin.
	public static native Vector3 boxMin( GameRef car );
	public static native Vector3 boxMax( GameRef car );

	// Chassis speed, m/s.
	public static native float speed( GameRef car );
}
