package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.cars.*;


public class SuperDuty_500_flywheel extends Flywheel
{
	public SuperDuty_500_flywheel( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 500 flywheel";
		description = "";
		brand_new_prestige_value = 57.34;

		value = tHUF2USD(59.023);
		setMaxWear(kmToMaxWear(760000.0));

		maxRPM = 7250.0;

		clutch_slot_ID = 4;

		inertia = kgToInertia(20.0);

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
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A7r, "SuperDuty_500_clutch", desc );
			}
			else
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A7r, "SuperDuty_500_clutch", desc );
			}
		}
	}
}
