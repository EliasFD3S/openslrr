package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class MC_GT_RR_trailing_arm extends Suspension
{
	public MC_GT_RR_trailing_arm ( int id )
	{
		super( id );

		name = "MC GT rear right trailing-arm";

		value = tHUF2USD(87.3);
		brand_new_prestige_value = 57.0;

		VICx = -0.646;
		VICy = -0.125;

		LICy = -0.230;
		LICz = -0.500;

		camber = 0.00;
		toe    = 0.00;

		side   = SD_RIGHT;
	}

	public void updatevariables()
	{
		if (!the_car)
			getCar_LocalVersion();
		if (the_car)
		{
			int wheelID = getWheelID();
			WheelRef wheel = the_car.getWheel(wheelID);
			if (wheel)
			{
				// OpenSLRR (#450): live rear axle (25 m arm, no camber gain) of the Chevrolet Camaro Z28, laid out at the static ride height
				wheel.setArm( 25.0000,
				              -24.9720, 0.1552, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.40, 1.00 );
				// OpenSLRR (#490): live axle, degrees of axle steer per degree of axle roll, typical for the real car
				wheel.setLiveAxle( 0.05 );
				// OpenSLRR (#494): the beam's mass (kg), its wrap (kN per mm) and damping ratio, typical for the real car
				wheel.setAxleMass( 55.0 );
				wheel.setAxleWrap( 0.80, 0.25 );
				// OpenSLRR (#490): roll centre height above the ground (m), typical for the real car
				wheel.setRollCenterHeight( 0.36 );
				// OpenSLRR (#496): roll centre migration, metres of height per metre of bump, typical for the real car
				wheel.setRollCenterMigration( -0.50 );
				// OpenSLRR (#496): Panhard rod length (m) and the side of its body end (-1 left)
				wheel.setPanhardRod( 0.95, -1.0 );

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
