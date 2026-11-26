package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SunStrip_15_RL_trailing_arm extends Suspension
{
	public SunStrip_15_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Duhen SunStrip 1.5 DVC rear left trailing-arm";

		value = tHUF2USD(50.0);
		brand_new_prestige_value = 30.0;

		VICx =  1.480;
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
				// OpenSLRR (#450): rear swing arm of the Honda CR-X del Sol, laid out at the static ride height
				wheel.setArm( 2.0463,
				              2.0183, 0.1620, 0.0000,
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
