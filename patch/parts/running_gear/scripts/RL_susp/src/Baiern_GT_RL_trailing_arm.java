package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;
import java.game.parts.rgearpart.*;

public class Baiern_GT_RL_trailing_arm extends Suspension
{
	public Baiern_GT_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Baiern CoupeSport GT III rear left trailing-arm";

		value = tHUF2USD(127.0);
		brand_new_prestige_value = 68.0;

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
				// OpenSLRR (#450): rear swing arm of the E36 race car, laid out at the static ride height
				wheel.setArm( 1.5915,
				              1.5635, 0.1981, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.40, 0.60 );
				// OpenSLRR (#481): camber gain per 25 mm of bump at the static height and its change per 25 mm, typical for the real car
				wheel.setCamberGain( -0.90, -0.05 );
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
