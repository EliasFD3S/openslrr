package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class MC_GTB_RR_trailing_arm extends Suspension
{
	public MC_GTB_RR_trailing_arm ( int id )
	{
		super( id );

		name = "MC GT-B rear right trailing-arm";

		value = tHUF2USD(93.3);
		brand_new_prestige_value = 50.0;

		VICx = -0.646;
		VICy = -0.125;

		LICy = -0.170;
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
				// OpenSLRR (#450): live rear axle (25 m arm, no camber gain) of the Camaro Z28, laid out at the static ride height
				wheel.setArm( 25.0000,
				              -24.9720, 0.1554, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.40, 1.00 );

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
