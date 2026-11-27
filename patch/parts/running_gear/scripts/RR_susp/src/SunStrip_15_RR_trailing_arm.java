package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SunStrip_15_RR_trailing_arm extends Suspension
{
	public SunStrip_15_RR_trailing_arm ( int id )
	{
		super( id );

		name = "Duhen SunStrip 1.5 DVC rear right trailing-arm";

		value = tHUF2USD(50.0);
		brand_new_prestige_value = 30.0;

		VICx =  1.480;
		VICy = -0.160;

		LICy = -0.150;
		LICz = -0.600;

		camber = -0.70;
		toe    = -0.10;

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
				// OpenSLRR (#450): rear swing arm of the Honda CR-X del Sol, laid out at the static ride height
				wheel.setArm( 2.0463,
				              -2.0183, 0.1620, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.25, 0.30 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.70, -0.10 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( 0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( 0.03, 0.05 );
				// OpenSLRR (#484): the bushings' give, mm per kN of cornering and of longitudinal force, typical for the real car
				wheel.setBushingCompliance( 0.20, 0.80 );

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
