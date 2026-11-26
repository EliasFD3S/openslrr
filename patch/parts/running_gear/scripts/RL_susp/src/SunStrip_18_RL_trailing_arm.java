package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SunStrip_18_RL_trailing_arm extends Suspension
{
	public SunStrip_18_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Duhen SunStrip 1.8 DVC rear left trailing-arm";

		value = tHUF2USD(63.0);
		brand_new_prestige_value = 35.0;

		VICx =  1.380;
		VICy = -0.160;

		LICy = -0.150;
		LICz = -0.600;

		camber = -0.70;
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
				// OpenSLRR (#450): rear swing arm of the del Sol, laid out at the static ride height
				wheel.setArm( 2.0463,
				              2.0463, 0.1627, 0.0000,
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
