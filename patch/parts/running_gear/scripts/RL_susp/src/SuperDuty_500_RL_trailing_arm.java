package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SuperDuty_500_RL_trailing_arm extends Suspension
{
	public SuperDuty_500_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Hauler's SuperDuty 500 rear left trailing-arm";

		value = tHUF2USD(80.0);
		brand_new_prestige_value = 62.0;

		VICx = -0.380;
		VICy = -0.100;

		LICy = -0.190;
		LICz = -0.350;

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
				// OpenSLRR (#450): live rear axle (25 m arm, no camber gain) of the Dodge Ram 1500, laid out at the static ride height
				wheel.setArm( 25.0000,
				              24.9720, 0.1732, 0.0000,
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
