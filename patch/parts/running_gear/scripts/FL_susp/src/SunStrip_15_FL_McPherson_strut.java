package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class SunStrip_15_FL_McPherson_strut extends Suspension
{
	public SunStrip_15_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Duhen SunStrip 1.5 DVC front left McPherson strut";

		value = tHUF2USD(48.0);
		brand_new_prestige_value = 33.0;

		VICx =  1.436;
		VICy = -0.125;

		LICy = -0.130;
		LICz =  0.800;

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
				// OpenSLRR (#450): front geometry of the Honda CR-X del Sol, laid out at the static ride height
				wheel.setArm( 0.2960,
				              0.3130, 0.1801, 0.0000,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4070, 0.0000,
				              0.1187, 0.7168, 0.0000);

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
