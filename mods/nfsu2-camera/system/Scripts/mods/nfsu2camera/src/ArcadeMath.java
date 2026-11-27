package java.mods.nfsu2camera;

import java.lang.*;

// Pure math of the ARCADE camera, ported from the removed engine arcade
// camera of #470 (arcade_camera.cpp at 46f90d7). No natives besides Math, so
// ArcadeTest can check it in the VM. The values approximate Need for Speed:
// Underground 2 by eye; the game's own camera table was not available.
public class ArcadeMath
{
	public final static int VIEWS = 4;
	public final static int BUMPER = 0;
	public final static int HOOD = 1;
	public final static int NEAR = 2;
	public final static int FAR = 3;

	public final static float MAX_LAG = 1.2;		// radians the eye may trail
	public final static float SLIP_SHARE = 0.6;		// of the slide angle shown
	public final static float MAX_SLIP = 0.6;		// radians of slide followed
	public final static float BUMPER_HEIGHT = 0.55;	// bumper eye above the lowest point
	public final static float MAX_DROP = 0.8;		// metres the focus may trail the car
	public final static float NITRO_FOV = 10.0;		// degrees added while nitrous burns
	public final static float NITRO_REACH = 0.8;	// metres added while nitrous burns
	public final static float IMPACT_ACCEL = 40.0;	// m/s^2 before a jolt shakes the view
	public final static float SPEED_SHAKE = 0.012;	// metres at full road vibration
	public final static float NITRO_SHAKE = 0.02;	// metres while nitrous burns
	public final static float IMPACT_SHAKE = 0.15;	// metres for the hardest crash
	public final static float TILT_PER_G = 0.05;	// radians of tilt per g of cornering
	public final static float MAX_TILT = 0.06;		// radians
	public final static float PHYSICS_STEP = 1.0 / 60.0;	// host physics step, s

	public static String viewName( int v )
	{
		if( v == BUMPER )
			return "BUMPER";
		if( v == HOOD )
			return "HOOD";
		if( v == NEAR )
			return "NEAR";
		return "FAR";
	}

	public static int clampView( int v )
	{
		if( v < 0 || v >= VIEWS )
			return FAR;
		return v;
	}

	public static int mounted( int v )
	{
		if( v == BUMPER || v == HOOD )
			return 1;
		return 0;
	}

	// Chase tuning: eye to focus parked, eye above the focus, focus above the
	// car origin, look point past the car (metres).
	public static float distance( int v )
	{
		if( v == NEAR )
			return 4.4;
		return 6.3;
	}

	public static float height( int v )
	{
		if( v == NEAR )
			return 1.1;
		return 1.45;
	}

	public static float focusLift( int v )
	{
		if( v == NEAR )
			return 0.7;
		return 0.75;
	}

	public static float ahead( int v )
	{
		if( v == NEAR )
			return 0.5;
		return 0.6;
	}

	// Vertical view angle in degrees: parked, added by 250 km/h, added at
	// full acceleration.
	public static float fovBase( int v )
	{
		if( v == BUMPER )
			return 58.0;
		if( v == HOOD )
			return 55.0;
		if( v == NEAR )
			return 54.0;
		return 50.0;
	}

	public static float fovSpeed( int v )
	{
		if( v == FAR )
			return 22.0;
		return 20.0;
	}

	public static float fovPull( int v )
	{
		if( mounted( v ) )
			return 3.0;
		return 4.0;
	}

	public static float clamp( float v, float lo, float hi )
	{
		if( v < lo )
			return lo;
		if( v > hi )
			return hi;
		return v;
	}

	public static float smooth01( float t )
	{
		t = clamp( t, 0.0, 1.0 );
		return t * t * (3.0 - 2.0 * t);
	}

	// Share of the way to a goal after dt seconds at this rate (1/s).
	public static float approach( float dt, float rate )
	{
		if( dt <= 0.0 )
			return 0.0;
		return 1.0 - Math.exp( -dt * rate );
	}

	// 0 parked or slow, 1 from 250 km/h.
	public static float speedShare( float speed )
	{
		return smooth01( (speed * 3.6 - 20.0) / 230.0 );
	}

	public static float fovGoal( int v, float speed, float pull )
	{
		return fovBase( v ) + fovSpeed( v ) * speedShare( speed ) + fovPull( v ) * clamp( pull / 10.0, 0.0, 1.0 );
	}

	public static float hypot( float a, float b )
	{
		return Math.sqrt( a * a + b * b );
	}

	// Speed along the car's heading (chassis -z), m/s; negative reversing.
	public static float forwardSpeed( float backX, float backZ, float vx, float vz )
	{
		float back = hypot( backX, backZ );
		if( back < 0.000001 )
			return 0.0;
		return -(vx * backX + vz * backZ) / back;
	}

	// The car's heading blended toward its direction of travel, as the eye's
	// azimuth (atan2(x, z) of the side the eye sits on). Reversing, crawling
	// or in the air the heading rules.
	public static float targetYaw( float backX, float backZ, float vx, float vz, int airborne )
	{
		float body = Math.atan2( backX, backZ );
		float speed = hypot( vx, vz );
		if( speed < 1.0 || forwardSpeed( backX, backZ, vx, vz ) <= 0.0 || airborne )
			return body;
		float travel = Math.atan2( -vx, -vz );
		float slip = clamp( Math.wrapAngle( travel - body ), -MAX_SLIP, MAX_SLIP );
		// A slide shows from about 40 km/h.
		float share = SLIP_SHARE * smooth01( (speed - 1.0) / 11.0 );
		return Math.wrapAngle( body + slip * share );
	}

	// The lag of the eye behind targetYaw after dt seconds: it closes at a
	// rate that grows with speed, never beyond MAX_LAG. Returns the new yaw.
	public static float lagYaw( float yaw, float target, float speed, float dt )
	{
		float rate = 4.0 + 2.0 * speedShare( speed );
		float lag = Math.wrapAngle( target - yaw );
		lag = lag - lag * approach( dt, rate );
		lag = clamp( lag, -MAX_LAG, MAX_LAG );
		return Math.wrapAngle( target - lag );
	}

	// Road vibration from 150 km/h, full at 300 km/h.
	public static float vibration( float speed )
	{
		return smooth01( (speed * 3.6 - 150.0) / 150.0 );
	}

	// The time a velocity change seen this frame took. The car's velocity is
	// published once per physics step: at a high frame rate a change shows up
	// after several unchanged frames (one step, not the frames since the last
	// change: a car that stood still for a second and is then hit changed in
	// one step), at a low one a frame spans several steps.
	public static float joltInterval( float frameDt, float sinceChange )
	{
		float t = sinceChange;
		if( t > PHYSICS_STEP )
			t = PHYSICS_STEP;
		if( t < frameDt )
			t = frameDt;
		return t;
	}

	// Eye azimuth (atan2(x, z)) of an eye at (ex, ez) around a car at (cx, cz);
	// the fallback when the eye is on the car.
	public static float azimuthOf( float ex, float ez, float cx, float cz, float fallback )
	{
		float dx = ex - cx, dz = ez - cz;
		if( dx * dx + dz * dz <= 0.0001 )
			return fallback;
		return Math.atan2( dx, dz );
	}

	// A velocity change above IMPACT_ACCEL starts a crash shake (0..1).
	public static float impactFromJolt( float jolt )
	{
		if( jolt <= IMPACT_ACCEL )
			return 0.0;
		return clamp( (jolt - IMPACT_ACCEL) / 200.0, 0.0, 1.0 );
	}

	// Tilt toward the outside of a bend from the lateral acceleration.
	public static float tilt( float yawRate, float speed, int airborne )
	{
		if( airborne )
			return 0.0;
		return clamp( yawRate * speed / 9.81 * TILT_PER_G, -MAX_TILT, MAX_TILT );
	}

	// Shake offsets (metres) at clock t: incommensurate sines, so the pattern
	// never repeats visibly.
	public static float shakeX( float amplitude, float t )
	{
		return amplitude * (0.6 * Math.sin( t * 57.0 ) + 0.4 * Math.sin( t * 89.0 + 1.3 ));
	}

	public static float shakeY( float amplitude, float t )
	{
		return amplitude * (0.6 * Math.sin( t * 71.0 + 0.7 ) + 0.4 * Math.sin( t * 113.0 ));
	}
}
