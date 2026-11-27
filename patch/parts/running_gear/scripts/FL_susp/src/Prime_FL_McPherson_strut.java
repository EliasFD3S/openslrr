package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Prime_FL_McPherson_strut extends Suspension
{
	public Prime_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Prime DLH 500 front left McPherson strut";

		value = tHUF2USD(188.0);
		brand_new_prestige_value = 90.0;

		VICx = -0.946;
		VICy = -0.175;

		LICy = -0.240;
		LICz =  0.200;

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
				// OpenSLRR (#450): front geometry of the none, laid out at the static ride height, 5 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3148, 0.1826, -0.0002,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4057, 0.0329,
				              0.1306, 0.7191, 0.0491);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.15, 0.10 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
