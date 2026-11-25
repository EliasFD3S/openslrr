package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.charger.*;
import java.game.cars.*;

public class SuperDuty_E750_supercharger extends SuperCharger
{
	public SuperDuty_E750_supercharger( int id )
	{
		super( id );
		name = "Hauler's SuperDuty Extra 750 supercharger";
		description = "";
		brand_new_prestige_value = 85.05;

		value = tHUF2USD(630.000);

		pulley_diameter = 60.0; // in milimeters //
		pulley_width	= 60.0; // in milimeters //

		calculateOptandRange (400.0, 4000.0);
		default_rpm_turbo_mul = 1.5;
		P_turbo_max	= 0.31;
		belt_slot_ID = 15;
	}

	public void updatevariables()
	{
		rpm_turbo_mul = default_rpm_turbo_mul;
		setMaxWear(kmToMaxWear(15000.0));
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_HAULER_S_HEAVEN)
		{
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_500)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000087r, "roots type supercharger drive belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A3r, "SuperDuty_500_carburetors", desc );
			}
			else
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000087r, "roots type supercharger drive belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A3r, "SuperDuty_500_carburetors", desc );
			}
		}
	}

	public String isDynoable()
	{
		Part p;

		p = partOnSlot( 9 );
		if (!p)
			return "the supercharger is missing the fuel system.";

		p = partOnSlot( 15 );
		if (!p)
			return "the supercharger is missing the drive belt.";

		return super.isDynoable();
	}
}
