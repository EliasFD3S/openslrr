package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.airfueldeliverysystem.*;


public class SL_Tuners_methanol_fuel_rail extends FuelInjectorSystem
{
	public SL_Tuners_methanol_fuel_rail( int id )
	{
		super( id );
		name = "SL Tuners 4 channel methanol injection fuel rail";
		description = "";

		value = tHUF2USD(220.000);
		brand_new_prestige_value = 90.00;
		setMaxWear(kmToMaxWear(230000.0));

		// OpenSLRR #407: real methanol mixture; the flow limit keeps the stock
		// air limit at 5.5:1 instead of 18:1.
		max_fuel_consumption	= 0.0655;
		default_mixture_ratio	= 5.5;
		default_fuel_type	= FT_METHANOL;
		mixture_ratio		= default_mixture_ratio;
		fuel_type		= default_fuel_type;
		calcStuffs();

		police_check_fine_value	= 500.000;

		check4warnings();
	}
}
