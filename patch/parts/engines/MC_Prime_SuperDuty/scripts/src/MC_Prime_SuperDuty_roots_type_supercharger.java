package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.charger.*;
import java.game.cars.*;

public class MC_Prime_SuperDuty_roots_type_supercharger extends SuperCharger
{
	public MC_Prime_SuperDuty_roots_type_supercharger( int id )
	{
		super( id );
		name = "SL Tuners 7.0 psi roots type supercharger";
		description = "";
		brand_new_prestige_value = 105.05;

		value = tHUF2USD(1000.000);

		pulley_diameter = 36.0; // in milimeters //
		pulley_width	= 60.0; // in milimeters //

		calculateOptandRange (900.0, 2500.0);
		default_rpm_turbo_mul = 2.5;
		P_turbo_max	= 1.15;
		belt_slot_ID = 15;
	}

	public void updatevariables()
	{
		rpm_turbo_mul = default_rpm_turbo_mul;
		setMaxWear(kmToMaxWear(9000.0));
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		addPart( parts.engines.MC_Prime_SuperDuty:0x00000087r, "roots type supercharger drive belt", desc );
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
