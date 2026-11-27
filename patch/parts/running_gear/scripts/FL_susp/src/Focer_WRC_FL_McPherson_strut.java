package java.game.parts.running_gear.fl_susp;

import java.game.parts.*;
import java.game.parts.rgearpart.*;

public class Focer_WRC_FL_McPherson_strut extends Suspension
{
	public Focer_WRC_FL_McPherson_strut ( int id )
	{
		super( id );

		name = "Shimutshibu Focer WRC front left McPherson strut";

		value = tHUF2USD(165.0);
		brand_new_prestige_value = 80.0;

		VICx =  1.746;
		VICy = -0.165;

		LICy = -0.100;
		LICz =  1.500;

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
				// OpenSLRR (#450): front geometry of the Evo WRC, laid out at the static ride height, 5 deg of caster (#458)
				wheel.setArm( 0.2960,
				              0.3165, 0.1488, -0.0004,
				              0.0000, 0.0000, 1.0000);
				wheel.setHub( 0.0000,
				              -0.0770, -0.0430, -0.0040,
				              0.0710, 0.4058, 0.0306,
				              0.1415, 0.7165, 0.0486);
				// OpenSLRR (#469): anti-dive braking, anti-lift driving, typical for the real car
				wheel.setAnti( 0.25, 0.15 );

				the_car.setSlotPos(101+wheelID, new Vector3(-0.124,0,0), null );
				the_car.setSlotPos(111+wheelID, new Vector3(-0.113,0,0), null );

				setRollCenter(wheel);
				setSuspensionGeometry(wheel);
			}
		}
	}
}
