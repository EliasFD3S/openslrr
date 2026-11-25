package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.charger.*;


public class Ishima_WRZ_turbocharger extends TurboCharger
{
	public Ishima_WRZ_turbocharger( int id )
	{
		super( id );
		name = "Ishima WRZ turbocharger";
		description = "This turbocharger is built into the Ishima Enula WRZ. \n It's designed to work from 2600 to 7800 RPM, but optimal pressure is from 3700 to 6500 RPM. It's a big help for the IshiBox powerplant. \n Charging stops at 1.0 bar (14.5 psi) thanks to the wastegate. The WG cannot be adjusted or bypassed on this model.";

		value = tHUF2USD(315);
		brand_new_prestige_value = 109.30;

		calculateOptandRange (2600.0, 7800.0);
		P_turbo_max	= 1.15;
		default_P_turbo_waste	= 1.0;
		P_turbo_waste	= default_P_turbo_waste;

		max_waste	= 1.0;
		min_waste	= 0.25;
	}

	public void updatevariables()
	{
		setMaxWear(kmToMaxWear(240000.0));
	}
        public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000054r, "turbocharger_air_out_pipe", desc );
		addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000055r, "turbocharger_exhaust_pipe", desc );
		
		
	}
}
