package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Baiern_DS_FL_McPherson_strut extends Suspension
{
	public Baiern_DS_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Baiern DevilSport front left McPherson strut";

		value = tHUF2USD(108.0);
		brand_new_prestige_value = 49.0;

		VICx =  1.446;
		VICy = -0.055;

		LICy =  0.100;
		LICz =  1.500;

		camber = 0.00;
		toe    = -0.10;

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
				// OpenSLRR (#450): front geometry of the BMW E36 M3, laid out at the static ride height, 6.6 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3143, 0.1461, -0.0003,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4046, 0.0446,
				              0.1276, 0.7107, 0.0660);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.15, 0.10 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.40, 0.15 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( -0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( -0.10, -0.03 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
