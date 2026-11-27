package java.mods.nfsu2camera;

import java.lang.*;

// VM test of the ARCADE camera math (#470), a port of the engine version's
// tests/test_arcade_camera.cpp (46f90d7). Not shipped: install this folder as
// <game>/mods/nfsu2-camera-test next to mods/nfsu2-camera, boot the game, read
// the "[arcadetest]" lines in the log; the game quits after the run.
public class ArcadeTest
{
	static int failures, checks;

	static void check( String name, int ok )
	{
		checks++;
		if( ok )
			System.log( "[arcadetest] ok   " + name );
		else
		{
			failures++;
			System.log( "[arcadetest] FAIL " + name );
		}
	}

	static float abs( float a )
	{
		if( a < 0.0 )
			return -a;
		return a;
	}

	// A car at the origin heading -z (its rear, +z, is the chassis back axis).
	static ArcadeCamera car( float speed )
	{
		ArcadeCamera c = new ArcadeCamera();
		c.view = ArcadeMath.FAR;
		c.rx = 1.0; c.ry = 0.0; c.rz = 0.0;
		c.ux = 0.0; c.uy = 1.0; c.uz = 0.0;
		c.bx = 0.0; c.by = 0.0; c.bz = 1.0;
		c.vz = -speed;
		return c;
	}

	static void settle( ArcadeCamera c, int steps )
	{
		for( int i = 0; i < steps; i++ )
			c.step( 1.0 / 60.0 );
	}

	public static void modInit()
	{
		failures = 0;
		checks = 0;
		float pi = Math.PI;

		// Parked: seen from behind and above, looking past the car.
		ArcadeCamera c = car( 0.0 );
		c.step( 0.0 );
		check( "parked init", c.init );
		check( "parked eye behind", c.ez > 5.5 && c.ez < 7.5 && abs( c.ex ) < 0.0001 );
		check( "parked eye above", c.ey > 1.5 && c.ey < 3.0 );
		check( "parked looks past", c.az < 0.0 );
		check( "parked fov", abs( c.fov - 50.0 ) < 0.001 );

		// Read-only queries do not step.
		c.bx = 1.0; c.bz = 0.0;
		float y0 = c.yaw;
		for( int i = 0; i < 10; i++ )
			c.step( 0.0 );
		check( "dt 0 keeps yaw", c.yaw == y0 );

		// The eye trails a turn, bounded, then settles behind.
		c.step( 1.0 / 60.0 );
		float lag = abs( Math.wrapAngle( pi / 2.0 - c.yaw ) );
		check( "turn trails (" + lag + ")", lag > 0.5 && lag <= ArcadeMath.MAX_LAG + 0.0001 );
		settle( c, 300 );
		check( "turn settles", abs( Math.wrapAngle( pi / 2.0 - c.yaw ) ) < 0.001 );

		// A slide swings the view toward travel.
		float slip = pi / 6.0;
		float yaw = ArcadeMath.targetYaw( 0.0, 1.0, 25.0 * Math.sin( slip ), -25.0 * Math.cos( slip ), 0 );
		check( "slide swing (" + yaw + ")", yaw < -0.05 && yaw > -slip );
		check( "reverse keeps heading", abs( ArcadeMath.targetYaw( 0.0, 1.0, 3.0, 10.0, 0 ) ) < 0.000001 );
		check( "spin bounded", abs( ArcadeMath.targetYaw( 0.0, 1.0, 25.0, -1.0, 0 ) ) <= ArcadeMath.SLIP_SHARE * ArcadeMath.MAX_SLIP + 0.00001 );

		// Speed widens the view and the reach.
		ArcadeCamera slow = car( 0.0 ), fast = car( 70.0 );
		settle( slow, 600 );
		settle( fast, 600 );
		slow.step( 0.0 );
		fast.step( 0.0 );
		check( "speed fov (" + fast.fov + ")", fast.fov > slow.fov + 15.0 && fast.fov <= 72.001 );
		check( "speed reach", fast.ez > slow.ez + 0.5 );

		// Bumps move the car more than the view; a jump never leaves it far behind.
		c = car( 20.0 );
		c.step( 0.0 );
		c.py = 0.3;
		c.step( 1.0 / 60.0 );
		check( "bump smoothed (" + c.focusY + ")", c.focusY > 0.0 && c.focusY < 0.15 );
		c.py = 5.0;
		c.step( 1.0 / 60.0 );
		check( "drop bounded", c.focusY >= 4.2 - 0.0001 );

		// Views: names, default, mounted, near closer and lower than far.
		check( "view names", ArcadeMath.viewName( 0 ) == "BUMPER" && ArcadeMath.viewName( 3 ) == "FAR" );
		check( "clampView", ArcadeMath.clampView( 7 ) == ArcadeMath.FAR && ArcadeMath.clampView( 1 ) == 1 );
		check( "mounted", ArcadeMath.mounted( 0 ) && ArcadeMath.mounted( 1 ) && !ArcadeMath.mounted( 2 ) );
		ArcadeCamera n = car( 0.0 ), f = car( 0.0 );
		n.view = ArcadeMath.NEAR;
		n.step( 0.0 );
		f.step( 0.0 );
		check( "near vs far", n.ez < f.ez - 1.0 && n.ey < f.ey );

		// Mounted views ride on the car (chassis frame).
		c = car( 30.0 );
		c.boxValid = 1;
		c.loX = -0.9; c.loY = -0.4; c.loZ = -2.3;
		c.hiX = 0.9; c.hiY = 1.0; c.hiZ = 2.1;
		c.view = ArcadeMath.BUMPER;
		c.step( 0.0 );
		check( "bumper ahead", c.ez < -2.3 && c.ez > -2.5 );
		check( "bumper height", abs( c.ey - (-0.4 + ArcadeMath.BUMPER_HEIGHT) ) < 0.0001 );
		check( "bumper looks ahead", c.az < c.ez - 5.0 && abs( c.ay - c.ey ) < 0.0001 );
		c.view = ArcadeMath.HOOD;
		c.step( 0.0 );
		check( "hood eye", c.ey > 0.6 && c.ez < 0.0 && c.ez > -2.3 );
		check( "hood looks down", c.ay < c.ey );
		// A car rolled 90 degrees: the chassis-frame pose turned to the world.
		c.ux = 1.0; c.uy = 0.0; c.rx = 0.0; c.ry = -1.0;
		Vector3 e = c.toWorld( c.ex, c.ey, c.ez ), u = c.toWorld( c.upx, c.upy, c.upz );
		check( "hood rolls with the car", u.x > 0.99 && abs( e.y ) < 0.0001 && abs( e.x ) > 0.6 );
		ArcadeCamera bare = car( 0.0 );
		bare.view = ArcadeMath.BUMPER;
		bare.step( 0.0 );
		check( "bumper without extent", bare.ez < -2.0 );

		// Nitrous widens and pushes back, then fades.
		ArcadeCamera plain = car( 30.0 ), boost = car( 30.0 );
		boost.nitroOn = 1;
		settle( plain, 300 );
		settle( boost, 300 );
		plain.step( 0.0 );
		boost.step( 0.0 );
		check( "nitro fov", boost.fov > plain.fov + ArcadeMath.NITRO_FOV * 0.9 );
		check( "nitro reach", boost.ez > plain.ez + 0.5 );
		boost.nitroOn = 0;
		settle( boost, 600 );
		check( "nitro fades", boost.nitro < 0.01 );

		// A crash shakes the view, then settles; braking does not.
		c = car( 30.0 );
		c.step( 1.0 / 60.0 );
		c.vz = -5.0;
		c.step( 1.0 / 60.0 );
		check( "crash shake (" + c.impact + ")", c.impact > 0.5 );
		settle( c, 120 );
		check( "crash settles", c.impact < 0.01 );
		c = car( 30.0 );
		c.step( 1.0 / 60.0 );
		c.vz = -29.85;
		c.step( 1.0 / 60.0 );
		check( "braking no shake", c.impact == 0.0 );
		// Velocity seen at 60 Hz physics with 144 Hz frames: the jolt uses the
		// time since the last change, so braking still does not shake.
		c = car( 30.0 );
		c.step( 1.0 / 144.0 );
		c.step( 1.0 / 144.0 );
		c.vz = -29.85;
		c.step( 1.0 / 144.0 );
		check( "braking no shake at 144 Hz", c.impact == 0.0 );

		// Look back cuts to a rear view; the mounted views use the rear bumper.
		c = car( 20.0 );
		c.step( 0.0 );
		c.back = 1;
		c.step( 0.0 );
		check( "look back chase", c.ez < -5.0 && abs( c.ex ) < 0.001 && c.az > c.ez );
		c.view = ArcadeMath.BUMPER;
		c.step( 0.0 );
		check( "look back bumper", c.ez > 2.0 && c.az > c.ez );
		c.view = ArcadeMath.HOOD;
		c.step( 0.0 );
		check( "look back hood", c.ez > 2.0 && c.az > c.ez );

		// Air time and bends.
		check( "air keeps heading", abs( ArcadeMath.targetYaw( 0.0, 1.0, 10.0, -20.0, 1 ) ) < 0.000001 );
		c = car( 30.0 );
		c.yawRate = 1.0;
		settle( c, 300 );
		check( "bend tilt bounded (" + c.roll + ")", abs( c.roll - ArcadeMath.MAX_TILT ) < 0.001 );
		c.step( 0.0 );
		check( "bend tilts up", abs( c.upy ) < 0.9999 );

		// Shake amplitude helpers.
		check( "vibration", ArcadeMath.vibration( 30.0 ) == 0.0 && ArcadeMath.vibration( 90.0 ) > 0.99 );
		check( "shake bounded", abs( ArcadeMath.shakeX( 0.1, 1.234 ) ) <= 0.1 && abs( ArcadeMath.shakeY( 0.1, 2.5 ) ) <= 0.1 );

		System.log( "[arcadetest] " + (checks - failures) + "/" + checks + " passed" );
		System.exit( "arcadetest" );
	}
}
