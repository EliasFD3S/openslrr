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
				              3.5810, 0.1986, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
