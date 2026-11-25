package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.cars.*;


public class SuperDuty_5_4_crankshaft extends Crankshaft
{
	public SuperDuty_5_4_crankshaft( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 5.4 crankshaft";
		description = "";
		brand_new_prestige_value = 51.61;

		value = tHUF2USD(53.121);
		setMaxWear(kmToMaxWear(700000.0));

		con_rod_slot_ID = 40;
		flywheel_slot_ID = 6;

		maxRPM = 7250.0;

		stroke = 106.0;
		main_bearing_diam = 45.05;
		con_bearing_diam = 45.05;

		inertia = kgToInertia(18.0);

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
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A6r, "SuperDuty_500_flywheel", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A8r, "SuperDuty_500_connecting_rods", desc );
			}
			else
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A6r, "SuperDuty_500_flywheel", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A8r, "SuperDuty_500_connecting_rods", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000088r, "supercharger driver pulley", desc );
			}
		}
	}

	public String isDynoable()
	{
		Part p;

		p = partOnSlot( 11 );
		if (!p)
			return "the engine is missing the camshaft drive belt.";

		p = partOnSlot( 3 );
		if (!p)
			return "the alternator is missing the drive belt.";

		return super.isDynoable();
	}
}
