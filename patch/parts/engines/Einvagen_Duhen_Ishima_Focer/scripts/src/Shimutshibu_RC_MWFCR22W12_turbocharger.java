package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.charger.*;


public class Shimutshibu_RC_MWFCR22W12_turbocharger extends TurboCharger
{
	public Shimutshibu_RC_MWFCR22W12_turbocharger( int id )
	{
		super( id );
		name = "Shimutshibu RC MWFCR22W12 turbocharger";
		description = "";
		brand_new_prestige_value = 109.30;

		value = tHUF2USD(635.000);

		calculateOptandRange (1500.0, 8750.0);
		P_turbo_max	= 4.00;
		default_P_turbo_waste	= 1.5;
		P_turbo_waste	= default_P_turbo_waste;

		max_waste	= 2.0;
		min_waste	= 0.75;
	}

        public void updatevariables()
	{
		setMaxWear(kmToMaxWear(120000.0));
	}

        public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000054r, "turbocharger_air_out_pipe", desc );
		addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000055r, "turbocharger_exhaust_pipe", desc );
		
		
	}



	
}
