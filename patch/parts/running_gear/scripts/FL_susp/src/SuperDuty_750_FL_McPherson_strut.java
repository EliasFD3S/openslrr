package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SuperDuty_750_FL_McPherson_strut extends Suspension
{
	public SuperDuty_750_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Hauler's SuperDuty Extra 750 front left McPherson strut";

		value = tHUF2USD(99.0);
		brand_new_prestige_value = 72.0;

		VICx = -0.646;
		VICy = -0.125;

		LICy = -0.330;
		LICz =  0.500;

		camber = 0.00;
		toe    = -0.06;

		side   = SD_LEFT;
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
				// OpenSLRR (#450): front geometry of the Dodge Ram 2500, laid out at the static ride height, 3.4 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3174, 0.1795, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4063, 0.0236,
				              0.1187, 0.7288, 0.0325);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.10, 0.10 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.60, -0.10 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( -0.10, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( -0.15, -0.05 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
