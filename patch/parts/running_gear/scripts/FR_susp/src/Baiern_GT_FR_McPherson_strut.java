package java.game.parts.running_gear.fr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Baiern_GT_FR_McPherson_strut extends Suspension
{
	public Baiern_GT_FR_McPherson_strut ( int id )
	{
		super( id );

		name = "Baiern CoupeSport GT III front right McPherson strut";

		value = tHUF2USD(170.0);
		brand_new_prestige_value = 70.0;

		VICx = -0.316;
		VICy = -0.235;

		LICy = -0.170;
		LICz =  0.250;

		camber = 0.00;
		toe    = 0.00;

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
				// OpenSLRR (#450): front geometry of the E36 race car, laid out at the static ride height, 6.6 deg of caster (#458)
				wheel.setArm( 0.2960,
				              -0.3247, 0.1737, -0.0008,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              0.0770, -0.0430, -0.0040,
				              -0.0710, 0.4052, 0.0384,
				              -0.1602, 0.7400, 0.0647);

				the_car.setSlotPos(101+wheelID, new Vector3(0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
