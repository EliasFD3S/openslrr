package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;


public class SuperDuty_500_pistons extends Piston
{
	public SuperDuty_500_pistons( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 500 pistons";
		description = "";
		brand_new_prestige_value = 71.68;

		value = tHUF2USD(73.779);
		setMaxWear(kmToMaxWear(411000.0));

		maxRPM = 7750.0;

		bore = 90.0;
		length_from_wrist_pin_center_to_crown_base = 62.00;
		crown_height = 22.19;
		crown_volume = 90.0;
		length_from_lowest_safe_belt_to_crown_base = 47.00;

		inertia = kgToInertia(8*0.620);

		check4warnings();
	}
}
