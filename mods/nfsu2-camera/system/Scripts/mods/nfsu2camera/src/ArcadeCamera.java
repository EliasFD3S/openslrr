package java.mods.nfsu2camera;

import java.lang.*;
import java.io.*;
import java.util.resource.*;
import java.render.*;
import java.openslrr.*;

// The ARCADE camera (#470): Need for Speed: Underground 2 style driving views
// as a java.openslrr.CameraProvider. BUMPER and HOOD are fixed to the car;
// NEAR and FAR chase it low behind, swinging toward the direction of travel
// in a slide, with a short lag in bends, a wider view and longer reach at
// speed. The views widen, drop back and shake under nitrous, shake with road
// speed and in crashes, tilt in bends, float over jumps, and cut to a rear
// view while LOOK BACK is held. Port of the removed engine camera
// (arcade_camera.cpp at 46f90d7); the math is in ArcadeMath.
public class ArcadeCamera extends CameraProvider
{
	public final static String NAME = "ARCADE";
	public final static String KEY_DEFAULT_VIEW = "nfsu2camera.view";
	public final static String KEY_SHAKE = "nfsu2camera.shake";
	// 1 logs the camera state once a second ("[nfsu2camera]" lines); no UI.
	public final static String KEY_DEBUG = "nfsu2camera.debug";
	public final static int CMD_NEXTVIEW = 4701;

	static ArcadeCamera	theInstance;
	static int			nextAxis = -1;	// ModKeys axis of NEXT ARCADE VIEW

	ArcadeDriver	driver;
	Hotkey			nextKey;
	GameRef			cam, car;
	int				view = ArcadeMath.FAR;
	int				back;				// LOOK BACK held
	int				lastDefault = -1;	// the ARCADE VIEW option last applied

	// Car extent (chassis frame: x right, y up, z back) for the mounted views.
	int				boxValid;
	float			loX, loY, loZ, hiX, hiY, hiZ;

	// Camera state (ArcadeCameraState of the engine version).
	int				init;
	float			lastTime;
	float			yaw;			// eye azimuth around the car, atan2(x, z)
	float			focusY;			// smoothed height of the car origin
	float			speed;			// smoothed horizontal speed, m/s
	float			pull;			// smoothed forward acceleration, m/s^2
	float			fov;			// vertical view angle, degrees
	float			nitro;			// smoothed nitrous burn, 0..1
	float			impact;			// crash shake, 0..1
	float			roll;			// tilt in bends, radians
	float			clock;			// seconds, for the shakes
	float			pvx, pvy, pvz;	// last velocity seen
	float			joltTime;		// seconds since the velocity last changed
	int				nitroOn, airborne;
	float			yawRate;
	float			sweepFraction = 1.0;	// last chase sweep, for the debug log
	float			debugTime;

	// One frame's inputs: car origin and chassis axes (world).
	float			px, py, pz;
	float			rx, ry, rz, ux, uy, uz, bx, by, bz;
	float			vx, vy, vz;

	// The pose being built: eye, look point, up (chassis frame for the mounted
	// views, else world offsets from the car origin) and view angle.
	float			ex, ey, ez, ax, ay, az, upx, upy, upz;

	public static ArcadeCamera instance()
	{
		if( !theInstance )
			theInstance = new ArcadeCamera();
		return theInstance;
	}

	public static int defaultView()
	{
		return ArcadeMath.clampView( ModSettings.getInt( KEY_DEFAULT_VIEW, ArcadeMath.FAR ) );
	}

	public ArcadeCamera()
	{
		lastDefault = defaultView();
	}

	public String name()
	{
		return NAME;
	}

	public int views()
	{
		return ArcadeMath.VIEWS;
	}

	public String viewName( int v )
	{
		return ArcadeMath.viewName( v );
	}

	public void activate( GameRef c, Viewport vp, int v )
	{
		deactivate();
		car = c;
		view = ArcadeMath.clampView( v );
		back = 0;
		init = 0;
		applyDefaultView();
		cam = new GameRef( world, GameRef.RID_CAMERA, "0,0,0,0,0,0,0x02", "nfsu2 arcade cam" );
		camera = cam;	// the GPS follows it in its camera modes
		cam.command( "render " + vp.id() + " 0 0 1 " + (Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET) );
		measureBox();
		if( !driver )
			driver = new ArcadeDriver( this );
		driver.enableAnimateHook();
		if( nextAxis >= 0 )
			nextKey = new Hotkey( nextAxis, Input.VIRTUAL, CMD_NEXTVIEW, driver );
		update();
	}

	public void deactivate()
	{
		if( nextKey )
		{
			nextKey.inactivate();
			nextKey = null;
		}
		if( driver )
			driver.disableAnimateHook();
		if( cam )
		{
			cam.destroy();
			cam = null;
		}
		camera = null;
		car = null;
		back = 0;
	}

	public void setView( int v )
	{
		// Track still delivers V while LOOK BACK is held: keep the view (and
		// the saved index, which Track already advanced) until it is released.
		if( back )
		{
			Cameras.setView( this, view );
			return;
		}
		view = ArcadeMath.clampView( v );
		if( ArcadeMath.mounted( view ) && !boxValid )
			measureBox();
	}

	public void lookBack( int pressed )
	{
		back = pressed;
	}

	// NEXT ARCADE VIEW: the same cycle as the change-view key.
	public void nextView()
	{
		if( !cam || back )
			return;
		int v = view + 1;
		if( v >= ArcadeMath.VIEWS )
			v = 0;
		Cameras.setView( this, v );
		setView( v );
	}

	// A change of the ARCADE VIEW option (Options > Mods) selects that view.
	void applyDefaultView()
	{
		int d = defaultView();
		if( d == lastDefault )
			return;
		lastDefault = d;
		Cameras.setView( this, d );
		setView( d );
	}

	void measureBox()
	{
		boxValid = 0;
		if( !car )
			return;
		Vector3 lo = CarState.boxMin( car );
		Vector3 hi = CarState.boxMax( car );
		if( !lo || !hi )
			return;
		if( hi.y > lo.y && hi.z > lo.z )
		{
			loX = lo.x; loY = lo.y; loZ = lo.z;
			hiX = hi.x; hiY = hi.y; hiZ = hi.z;
			boxValid = 1;
		}
	}

	// Once per frame from ArcadeDriver.animate.
	public void update()
	{
		if( !cam || !car )
			return;
		applyDefaultView();
		Vector3 p = CarState.drawnPos( car );
		Ypr o = CarState.drawnOri( car );
		if( !p || !o )
			return;
		float now = System.simTime();
		float dt = now - lastTime;
		lastTime = now;
		if( !init || dt < 0.0 )
			dt = 0.0;
		if( dt > 0.1 )
			dt = 0.1;

		px = p.x; py = p.y; pz = p.z;
		Vector3 a = new Vector3( 1.0, 0.0, 0.0 );
		a.rotate( o );
		rx = a.x; ry = a.y; rz = a.z;
		a = new Vector3( 0.0, 1.0, 0.0 );
		a.rotate( o );
		ux = a.x; uy = a.y; uz = a.z;
		a = new Vector3( 0.0, 0.0, 1.0 );
		a.rotate( o );
		bx = a.x; by = a.y; bz = a.z;

		Vector3 v = CarState.vel( car );
		vx = 0.0; vy = 0.0; vz = 0.0;
		if( v )
		{
			vx = v.x; vy = v.y; vz = v.z;
		}
		// Nitrous, wheel contact and yaw rate, sampled on timed steps only.
		if( dt > 0.0 || !init )
		{
			nitroOn = CarState.nitro( car );
			// Inactive physics also reports no wheel down: only a moving car flies.
			airborne = 0;
			if( CarState.wheelsOnGround( car ) == 0 && ArcadeMath.hypot( ArcadeMath.hypot( vx, vy ), vz ) > 0.5 )
				airborne = 1;
			yawRate = 0.0;
			Vector3 w = CarState.angVel( car );
			if( w )
				yawRate = w.y;
		}

		step( dt );
		place();
		if( now - debugTime >= 1.0 && ModSettings.getInt( KEY_DEBUG, 0 ) )
		{
			debugTime = now;
			System.log( "[nfsu2camera] view=" + ArcadeMath.viewName( view ) + " back=" + back + " speed=" + speed
				+ " pull=" + pull + " fov=" + fov + " nitro=" + nitro + " impact=" + impact + " roll=" + roll
				+ " yaw=" + yaw + " air=" + airborne + " sweep=" + sweepFraction + " box=" + boxValid );
		}
	}

	// Advances the state by dt seconds (0: only reads it) and builds the pose.
	void step( float dt )
	{
		float target = ArcadeMath.targetYaw( bx, bz, vx, vz, airborne );
		float speedNow = ArcadeMath.hypot( vx, vz );
		if( !init )
		{
			yaw = target;
			focusY = py;
			speed = speedNow;
			pull = 0.0;
			fov = ArcadeMath.fovGoal( view, speedNow, 0.0 );
			nitro = nitroOn;
			impact = 0.0;
			roll = 0.0;
			pvx = vx; pvy = vy; pvz = vz;
			joltTime = 0.0;
			init = 1;
		}
		if( dt > 0.0 )
		{
			float previous = speed;
			speed = speed + (speedNow - previous) * ArcadeMath.approach( dt, 4.0 );
			float accel = 0.0;
			if( ArcadeMath.forwardSpeed( bx, bz, vx, vz ) >= 0.0 )
				accel = (speed - previous) / dt;
			pull = pull + (ArcadeMath.clamp( accel, -15.0, 15.0 ) - pull) * ArcadeMath.approach( dt, 2.5 );
			// A short lag in bends, tighter at speed. The mounted views keep it
			// too, so a chase view picked next starts behind the car.
			yaw = ArcadeMath.lagYaw( yaw, target, speed, dt );
			// Bumps move the car more than the view; in the air the view floats
			// behind a jump and settles after the landing.
			float rate = 10.0;
			if( airborne )
				rate = 3.0;
			focusY = focusY + (py - focusY) * ArcadeMath.approach( dt, rate );
			focusY = ArcadeMath.clamp( focusY, py - ArcadeMath.MAX_DROP, py + ArcadeMath.MAX_DROP );
			// Nitrous: the view widens, drops back and shakes while it burns.
			rate = 1.5;
			if( nitroOn )
				rate = 3.0;
			nitro = nitro + (nitroOn - nitro) * ArcadeMath.approach( dt, rate );
			// A crash or a hard landing: a velocity change above 4 g. The
			// velocity changes once per physics step, so the change is divided
			// by the time since it last changed, not by the frame time.
			joltTime = joltTime + dt;
			float dx = vx - pvx, dy = vy - pvy, dz = vz - pvz;
			float dv = Math.sqrt( dx * dx + dy * dy + dz * dz );
			if( dv > 0.0 )
			{
				impact = Math.max( impact, ArcadeMath.impactFromJolt( dv / joltTime ) );
				joltTime = 0.0;
				pvx = vx; pvy = vy; pvz = vz;
			}
			impact = impact * Math.exp( -dt * 6.0 );
			roll = roll + (ArcadeMath.tilt( yawRate, speed, airborne ) - roll) * ArcadeMath.approach( dt, 3.0 );
			clock = clock + dt;
			fov = fov + (ArcadeMath.fovGoal( view, speed, pull ) + ArcadeMath.NITRO_FOV * nitro - fov) * ArcadeMath.approach( dt, 2.5 );
		}

		float mount = 1.0;
		if( view == ArcadeMath.BUMPER )
			mount = 2.0;
		else
		if( view == ArcadeMath.HOOD )
			mount = 1.4;
		float intensity = ArcadeMath.clamp( ModSettings.getFloat( KEY_SHAKE, 100.0 ), 0.0, 200.0 ) / 100.0;
		float amplitude = (ArcadeMath.SPEED_SHAKE * ArcadeMath.vibration( speed ) * mount +
			ArcadeMath.NITRO_SHAKE * nitro + ArcadeMath.IMPACT_SHAKE * impact) * intensity;
		float sx = ArcadeMath.shakeX( amplitude, clock );
		float sy = ArcadeMath.shakeY( amplitude, clock );

		if( ArcadeMath.mounted( view ) )
		{
			mountedPose();
			motion( sx, sy, 0.0 );
			return;
		}
		chasePose();
		if( back )
			motion( sx, sy, -roll );
		else
			motion( sx, sy, roll );
		sweep();
	}

	// Bumper and hood eyes in the chassis frame. A saloon about 4.4 m long and
	// 1.4 m tall when the car's extent is unknown.
	void mountedPose()
	{
		float ly = -0.5, lz = -2.2, hy = 0.9, hz = 2.2;
		if( boxValid )
		{
			ly = loY; lz = loZ; hy = hiY; hz = hiZ;
		}
		float length = hz - lz, tall = hy - ly;
		if( view == ArcadeMath.BUMPER || back )
		{
			// Low, just ahead of the front bumper: the car itself is not seen.
			// Looking back (both mounted views), the rear bumper, facing back.
			float end = lz - 0.05, dir = -1.0;
			if( back )
			{
				end = hz + 0.05;
				dir = 1.0;
			}
			ex = 0.0;
			ey = ly + Math.min( ArcadeMath.BUMPER_HEIGHT, 0.6 * tall );
			ez = end;
			ax = 0.0;
			ay = ey;
			az = ez + 10.0 * dir;
		}
		else
		{
			// Just below the roof at the base of the windscreen, looking down
			// over the bonnet.
			ex = 0.0;
			ey = hy - 0.06 * tall;
			ez = lz + 0.35 * length;
			ax = 0.0;
			ay = ey - 1.0;
			az = ez - 10.0;
		}
		upx = 0.0; upy = 1.0; upz = 0.0;
	}

	// Chase eye as world offsets from the car origin.
	void chasePose()
	{
		float share = ArcadeMath.speedShare( speed );
		float dist = ArcadeMath.distance( view );
		// The eye drops back with speed and while the car pulls, closes in
		// while it brakes, sits a little lower at speed; nitrous pushes it back.
		float reach = dist + dist / 6.3 * share + ArcadeMath.clamp( pull * 0.1, -0.5, 0.9 ) + ArcadeMath.NITRO_REACH * nitro;
		float height = ArcadeMath.height( view ) * (1.0 - 0.12 * share - 0.08 * nitro);
		// Looking back is a straight cut to the same view from in front of the
		// car, without lag.
		float azimuth = yaw;
		if( back )
			azimuth = Math.wrapAngle( Math.atan2( bx, bz ) + Math.PI );
		float elevation = ArcadeMath.clamp( Math.atan2( height, reach ), -0.3, 1.3 );
		float radius = ArcadeMath.hypot( reach, height );
		float fy = focusY - py + ArcadeMath.focusLift( view );
		float s = Math.sin( azimuth ), c = Math.cos( azimuth );
		ex = s * Math.cos( elevation ) * radius;
		ey = fy + Math.sin( elevation ) * radius;
		ez = c * Math.cos( elevation ) * radius;
		// Look past the car, more so at speed: the car sits low in the frame.
		float ahead = ArcadeMath.ahead( view ) + 1.8 * share;
		ax = -s * ahead;
		ay = fy;
		az = -c * ahead;
		upx = 0.0; upy = 1.0; upz = 0.0;
	}

	// Shakes move the eye and look point together (the view jitters, the aim
	// does not); roll turns the up axis about the view direction.
	void motion( float sx, float sy, float r )
	{
		float fx = ax - ex, fy = ay - ey, fz = az - ez;
		float len = Math.sqrt( fx * fx + fy * fy + fz * fz );
		if( len < 0.000001 )
			return;
		fx = fx / len; fy = fy / len; fz = fz / len;
		float qx = fy * upz - fz * upy, qy = fz * upx - fx * upz, qz = fx * upy - fy * upx;	// right
		len = Math.sqrt( qx * qx + qy * qy + qz * qz );
		if( len < 0.000001 )
			return;
		qx = qx / len; qy = qy / len; qz = qz / len;
		float wx = qy * fz - qz * fy, wy = qz * fx - qx * fz, wz = qx * fy - qy * fx;	// up
		float ox = qx * sx + wx * sy, oy = qy * sx + wy * sy, oz = qz * sx + wz * sy;
		ex = ex + ox; ey = ey + oy; ez = ez + oz;
		ax = ax + ox; ay = ay + oy; az = az + oz;
		float c = Math.cos( r ), s = Math.sin( r );
		upx = wx * c + qx * s; upy = wy * c + qy * s; upz = wz * c + qz * s;
	}

	// Keeps a chase eye out of the walls between it and the look point.
	void sweep()
	{
		Vector3 from = new Vector3( px + ax, py + ay, pz + az );
		Vector3 to = new Vector3( px + ex, py + ey, pz + ez );
		float f = CameraControl.sweep( from, to, 0.15 );
		sweepFraction = f;
		if( f >= 1.0 )
			return;
		f = ArcadeMath.clamp( f - 0.02, 0.05, 1.0 );
		ex = ax + (ex - ax) * f;
		ey = ay + (ey - ay) * f;
		ez = az + (ez - az) * f;
	}

	// The one place the pose goes to the engine, resolved against the drawn
	// car when the frame is drawn. Mounted views are in the chassis frame, so
	// the engine also turns them with the drawn car (setPoseLocal); chase
	// views are world offsets from the car origin (setPoseRelative).
	void place()
	{
		Vector3 eye = new Vector3( ex, ey, ez );
		Vector3 at = new Vector3( ax, ay, az );
		Vector3 up = new Vector3( upx, upy, upz );
		if( ArcadeMath.mounted( view ) )
			CameraControl.setPoseLocal( cam, car, eye, at, up, fov );
		else
			CameraControl.setPoseRelative( cam, car, eye, at, up, fov );
	}

	// A chassis-frame vector in the world, with the axes read this frame
	// (ArcadeTest checks the mounted views with it).
	Vector3 toWorld( float x, float y, float z )
	{
		return new Vector3( rx * x + ux * y + bx * z, ry * x + uy * y + by * z, rz * x + uz * y + bz * z );
	}
}
