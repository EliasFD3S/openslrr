package java.game.parts.running_gear.fr_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Einvagen_GT_FR_McPherson_strut extends Suspension
{
	public Einvagen_GT_FR_McPherson_strut ( int id )
	{
		super( id );

		name = "Einvagen 110 GT front right McPherson strut";

		value = tHUF2USD(50.0);
		brand_new_prestige_value = 30.0;

		VICx =  1.446;
		VICy = -0.125;

		LICy = -0.130;
		LICz =  0.800;

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
				// OpenSLRR (#450): front geometry of the VW Golf III, laid out at the static ride height
				wheel.setArm( 0.2960,
				              -0.2183, 0.1063, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              0.0770, -0.0430, -0.0040,
				              -0.0710, 0.4070, 0.0000,
				              -0.0277, 0.6907, 0.0000);

				the_car.setSlotPos(101+wheelID, new Vector3(0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
