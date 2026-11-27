package java.game.parts.running_gear.rr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Baiern_GT_RR_trailing_arm extends Suspension
{
	public Baiern_GT_RR_trailing_arm ( int id )
	{
		super( id );

		name = "Baiern CoupeSport GT III rear right trailing-arm";

		value = tHUF2USD(127.0);
		brand_new_prestige_value = 68.0;

		VICx = -0.880;
		VICy = -0.110;

		LICy = -0.170;
		LICz = -0.600;

		camber = -2.00;
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
				// OpenSLRR (#450): rear swing arm of the E36 race car, laid out at the static ride height
				wheel.setArm( 1.5915,
				              -1.5635, 0.1981, 0.0000,
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
