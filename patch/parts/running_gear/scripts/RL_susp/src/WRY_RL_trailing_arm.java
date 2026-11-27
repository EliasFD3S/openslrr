package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class WRY_RL_trailing_arm extends Suspension
{
	public WRY_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Ishima Enula WRY rear left trailing-arm";

		value = tHUF2USD(50.0);
		brand_new_prestige_value = 40.0;

		VICx =  1.180;
		VICy = -0.100;

		LICy =  0.050;
		LICz = -1.350;

		camber = -1.00;
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
				// OpenSLRR (#450): rear swing arm of the Subaru Impreza GC8, laid out at the static ride height
				wheel.setArm( 3.5810,
				              3.5530, 0.1911, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.25, 0.35 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.40, 0.10 );
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
