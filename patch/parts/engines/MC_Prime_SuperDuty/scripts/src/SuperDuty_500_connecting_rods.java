package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.cars.*;


public class SuperDuty_500_connecting_rods extends ConnectingRod
{
	public SuperDuty_500_connecting_rods( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 500 connecting rods";
		description = "";
		brand_new_prestige_value = 71.68;

		value = tHUF2USD(73.779);
		setMaxWear(kmToMaxWear(413000.0));

		maxRPM = 7250.0;

		piston_slot_ID = 42;

		length = 146.69+19.1-12;

		inertia = kgToInertia(8*0.750);

		check4warnings();
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_HAULER_S_HEAVEN)
		{
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_500)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000AAr, "SuperDuty_500_pistons", desc );
			}
			else
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000AAr, "SuperDuty_500_pistons", desc );
			}
		}
	}
}
