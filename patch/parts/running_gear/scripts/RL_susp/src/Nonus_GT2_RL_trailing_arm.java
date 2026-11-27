package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Nonus_GT2_RL_trailing_arm extends Suspension
{
	public Nonus_GT2_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Emer Motorsport Nonus GT2 rear left trailing-arm";

		value = tHUF2USD(157.0);
		brand_new_prestige_value = 81.0;

		VICx = -0.880;
		VICy = -0.110;

		LICy = -0.170;
		LICz = -0.600;

		camber = -2.00;
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
				// OpenSLRR (#450): rear swing arm of the R34 JGTC, laid out at the static ride height
				wheel.setArm( 2.0463,
				              2.0183, 0.2091, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.40, 0.60 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.70, -0.10 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( 0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( 0.01, 0.02 );
				// OpenSLRR (#484): the bushings' give, mm per kN of cornering and of longitudinal force, typical for the real car
				wheel.setBushingCompliance( 0.05, 0.10 );
				// OpenSLRR (#490): roll centre height above the ground (m), typical for the real car
				wheel.setRollCenterHeight( 0.06 );
				// OpenSLRR (#496): roll centre migration, metres of height per metre of bump, typical for the real car
				wheel.setRollCenterMigration( -1.00 );
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
