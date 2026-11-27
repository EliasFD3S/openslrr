package java.game.parts.running_gear.fr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Einvagen_GTK_FR_McPherson_strut extends Suspension
{
	public Einvagen_GTK_FR_McPherson_strut ( int id )
	{
		super( id );

		name = "Einvagen 110 GTK front right McPherson strut";

		value = tHUF2USD(72.0);
		brand_new_prestige_value = 34.0;

		VICx =  1.436;
		VICy = -0.125;

		LICy = -0.130;
		LICz =  0.800;
		camber = 0.00;
		toe    = -0.08;

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
				// OpenSLRR (#450): front geometry of the VW Golf III, laid out at the static ride height, 1.75 deg of caster (#458)
				wheel.setArm( 0.2960,
				              -0.3126, 0.1419, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              0.0770, -0.0430, -0.0040,
				              -0.0710, 0.4069, 0.0101,
				              -0.1217, 0.7093, 0.0147);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.10, 0.15 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.40, 0.15 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( -0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( -0.10, -0.03 );
				// OpenSLRR (#484): the bushings' give, mm per kN of cornering and of longitudinal force, typical for the real car
				wheel.setBushingCompliance( 0.30, 1.00 );
				// OpenSLRR (#490): roll centre height above the ground (m), typical for the real car
				wheel.setRollCenterHeight( 0.08 );
				// OpenSLRR (#496): roll centre migration, metres of height per metre of bump, typical for the real car
				wheel.setRollCenterMigration( -2.00 );

				the_car.setSlotPos(101+wheelID, new Vector3(0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
