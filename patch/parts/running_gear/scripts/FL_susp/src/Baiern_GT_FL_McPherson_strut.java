package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Baiern_GT_FL_McPherson_strut extends Suspension
{
	public Baiern_GT_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Baiern CoupeSport GT III front left McPherson strut";

		value = tHUF2USD(170.0);
		brand_new_prestige_value = 70.0;

		VICx = -0.316;
		VICy = -0.235;

		LICy = -0.170;
		LICz =  0.250;

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
				// OpenSLRR (#450): front geometry of the E36 race car, laid out at the static ride height
				wheel.setArm( 0.2960,
				              0.2156, 0.1630, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4070, 0.0000,
				              0.0524, 0.7520, 0.0000);

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
