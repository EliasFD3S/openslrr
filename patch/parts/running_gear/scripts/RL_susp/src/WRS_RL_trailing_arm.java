package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class WRS_RL_trailing_arm extends Suspension
{
	public WRS_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Ishima Enula WR SuperTurizmo rear left trailing-arm";

		value = tHUF2USD(163.0);
		brand_new_prestige_value = 80.0;

		VICx =  1.780;
		VICy = -0.110;

		LICy =  0.050;
		LICz = -1.350;

		camber = -1.20;
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
				// OpenSLRR (#450): rear swing arm of the Impreza GC8 STi, laid out at the static ride height
				wheel.setArm( 3.5810,
				              3.5530, 0.1986, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.30, 0.40 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.40, 0.10 );
				// OpenSLRR (#484): bump steer, degrees of toe-in per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setBumpSteer( 0.05, 0.00 );
				// OpenSLRR (#484): compliance steer, degrees of toe-in per kN of cornering force pushing the tyre inboard and per kN of braking, typical for the real car
				wheel.setComplianceSteer( 0.03, 0.05 );
				// OpenSLRR (#484): the bushings' give, mm per kN of cornering and of longitudinal force, typical for the real car
				wheel.setBushingCompliance( 0.20, 0.80 );
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
