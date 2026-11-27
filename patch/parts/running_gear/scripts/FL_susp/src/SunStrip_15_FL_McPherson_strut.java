package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SunStrip_15_FL_McPherson_strut extends Suspension
{
	public SunStrip_15_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Duhen SunStrip 1.5 DVC front left McPherson strut";

		value = tHUF2USD(48.0);
		brand_new_prestige_value = 33.0;

		VICx =  1.436;
		VICy = -0.125;

		LICy = -0.130;
		LICz =  0.800;

		camber = 0.00;
		toe    = 0.00;

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
				// OpenSLRR (#450): front geometry of the Honda CR-X del Sol, laid out at the static ride height, 2 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3130, 0.1801, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4068, 0.0124,
				              0.1187, 0.7166, 0.0174);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.10, 0.15 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.90, -0.15 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( -0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( -0.10, -0.03 );
				// OpenSLRR (#484): the bushings' give, mm per kN of cornering and of longitudinal force, typical for the real car
				wheel.setBushingCompliance( 0.30, 1.00 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
