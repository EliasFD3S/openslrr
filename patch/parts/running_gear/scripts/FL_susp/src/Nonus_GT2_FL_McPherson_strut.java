package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Nonus_GT2_FL_McPherson_strut extends Suspension
{
	public Nonus_GT2_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Emer Motorsport Nonus GT2 front left McPherson strut";

		value = tHUF2USD(241.0);
		brand_new_prestige_value = 80.0;

		VICx = -0.616;
		VICy = -0.235;

		LICy = -0.150;
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
				// OpenSLRR (#450): front geometry of the R34 JGTC, laid out at the static ride height, 7 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3212, 0.1945, -0.0009,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4049, 0.0409,
				              0.1572, 0.7311, 0.0688);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.30, 0.15 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
