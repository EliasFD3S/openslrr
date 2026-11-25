package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;


public class SuperDuty_500_transmission extends Transmission
{
	public SuperDuty_500_transmission( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 500 transmission";
		description = "";
		brand_new_prestige_value = 58.53;

		value = tHUF2USD(162.000);
		setMaxWear(kmToMaxWear(511000.0));

		gears	= 4;
		ratio[0]	= 0;
		ratio[1]	= 2.710;
		ratio[2]	= 1.530;
		ratio[3]	= 1.000;
		ratio[4]	= 0.710;
		ratio[7]	=-2.710;
		end_ratio	= 3.750;

		starter_torque	= 100.0;

		inertia = kgToInertia(13.0);

		maxRPM = 7250.0;

		check4warnings();

		drive_type = DT_RWD;
		drive_front_min = 0.0;
		drive_front_max = 0.0;
		def_drive_front = drive_front_min;
		drive_front = def_drive_front;

		diff_lock_min = 0.33;
		diff_lock_max = 0.33;
		def_diff_lock = 0.33;
		diff_lock = def_diff_lock;
	}
}
