package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.charger.*;
import java.game.cars.*;


public class Einvagen_GTK_9psi_supercharger extends SuperCharger
{
	public Einvagen_GTK_9psi_supercharger( int id )
	{
		super( id );
		name = "Einvagen 110 GTK supercharger";
		description = "This supercharger is built into the Einvagen 110 GTK models. \n It's designed to work from 1500 to 7000 RPM. The compressor drive ratio is 2.0 on a stock 110 GTK engine, and produces 0.80 bar (11.6 psi) of boost limited by an integrated wastegate.";

		value = tHUF2USD(420);
		brand_new_prestige_value = 49.80;

		pulley_diameter = 50.0; // in milimeters //
		pulley_width	= 50.0; // in milimeters //

		calculateOptandRange (1500.0, 7000.0);
		default_rpm_turbo_mul = 1.0;
		P_turbo_max	= 3.1;
		P_turbo_waste	= 0.8;
	}

	public void updatevariables()
	{
		rpm_turbo_mul = default_rpm_turbo_mul;
		setMaxWear(kmToMaxWear(205000.0));
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_EINVAGEN)
		{
			if (the_car.model == Einvagen_models.MODEL_110_GTK)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000053r, "Einvagen_supercharger_air_out_pipe", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000059r, "Einvagen_supercharger_drive_belt", desc );
			}
		}
	}

	public String isDynoable()
	{
		Part p;

		p = partOnSlot( 16 );
		if (!p)
			return "the supercharger is missing the drive belt.";

		return super.isDynoable();
	}
}
