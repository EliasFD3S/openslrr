package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Prime_RR_trailing_arm extends Suspension
{
	public Prime_RR_trailing_arm ( int id )
	{
		super( id );

		name = "Prime DLH 500 rear right trailing-arm";

		value = tHUF2USD(157.0);
		brand_new_prestige_value = 90.0;

		VICx = -1.146;
		VICy = -0.150;

		LICy = -0.220;
		LICz = -0.400;

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
				// OpenSLRR (#450): rear swing arm of the none, laid out at the static ride height
				wheel.setArm( 2.0463,
				              -2.0463, 0.1809, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( -1.0000,
				              -0.1210, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000,
				              0.0000, 0.0000, 0.0000);

				the_car.setSlotPos(101+wheelID, new Vector3( 0.024,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3( 0.013,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
