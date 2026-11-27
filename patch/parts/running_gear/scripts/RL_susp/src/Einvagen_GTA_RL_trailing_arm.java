package java.game.parts.running_gear.rl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Einvagen_GTA_RL_trailing_arm extends Suspension
{
	public Einvagen_GTA_RL_trailing_arm ( int id )
	{
		super( id );

		name = "Einvagen 140 GTA rear left trailing-arm";

		value = tHUF2USD(89.0);
		brand_new_prestige_value = 43.0;

		VICx =  1.230;
		VICy = -0.130;

		LICy = -0.150;
		LICz = -0.630;

		camber = -1.50;
		toe    = -0.17;

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
				// OpenSLRR (#450): rear swing arm of the VW Golf III VR6, laid out at the static ride height
				wheel.setArm( 7.1620,
				              7.1340, 0.1680, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.25, 0.30 );
				                        
				the_car.setSlotPos(101+wheelID, new Vector3(-0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
