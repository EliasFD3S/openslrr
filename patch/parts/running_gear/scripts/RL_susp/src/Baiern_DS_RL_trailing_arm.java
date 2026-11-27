package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Baiern_DS_RL_trailing_arm extends Suspension
{
	public Baiern_DS_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Baiern DevilSport rear left trailing-arm";

		value = tHUF2USD(93.0);
		brand_new_prestige_value = 50.0;

		VICx =  1.280;
		VICy = -0.120;

		LICy =  0.050;
		LICz = -1.350;

		camber = -1.60;
		toe    = -0.15;

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
				// OpenSLRR (#450): rear swing arm of the BMW E36 M3, laid out at the static ride height
				wheel.setArm( 1.5915,
				              1.5625, 0.1846, 0.0000,
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
