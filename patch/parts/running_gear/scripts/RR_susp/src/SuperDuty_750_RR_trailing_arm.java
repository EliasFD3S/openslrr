package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SuperDuty_750_RR_trailing_arm extends Suspension
{
	public SuperDuty_750_RR_trailing_arm ( int id )
	{
		super( id );

		name = "Hauler's SuperDuty Extra 750 rear right trailing-arm";

		value = tHUF2USD(88.0);
		brand_new_prestige_value = 70.0;

		VICx = -0.380;
		VICy = -0.100;

		LICy = -0.250;
		LICz = -0.420;

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
				// OpenSLRR (#450): live rear axle (25 m arm, no camber gain) of the Dodge Ram 2500, laid out at the static ride height
				wheel.setArm( 25.0000,
				              -24.9720, 0.1748, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.20, 0.50 );
				// OpenSLRR (#490): live axle, degrees of axle steer per degree of axle roll, typical for the real car
				wheel.setLiveAxle( 0.10 );
				// OpenSLRR (#494): the beam's mass (kg), its wrap (kN per mm) and damping ratio, typical for the real car
				wheel.setAxleMass( 95.0 );
				wheel.setAxleWrap( 0.30, 0.10 );
				// OpenSLRR (#490): roll centre height above the ground (m), typical for the real car
				wheel.setRollCenterHeight( 0.45 );

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
