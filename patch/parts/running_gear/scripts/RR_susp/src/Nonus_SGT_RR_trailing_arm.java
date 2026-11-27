package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Nonus_SGT_RR_trailing_arm extends Suspension
{
	public Nonus_SGT_RR_trailing_arm ( int id )
	{
		super( id );

		name = "Emer Nonus StreetGT rear right trailing-arm";

		value = tHUF2USD(83.0);
		brand_new_prestige_value = 46.0;

		VICx =  1.180;
		VICy = -0.100;

		LICy =  0.050;
		LICz = -1.350;

		camber = -1.50;
		toe    = -0.10;

		side   = SD_RIGHT;
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
				// OpenSLRR (#450): rear swing arm of the Nissan Skyline R34 GT-R, laid out at the static ride height
				wheel.setArm( 2.0463,
				              -2.0173, 0.1794, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);
				// OpenSLRR (#469): anti-lift braking, anti-squat driving, typical for the real car
				wheel.setAnti( 0.30, 0.50 );

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
