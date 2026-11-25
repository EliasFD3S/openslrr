package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.camshaft.*;


public class MC_Prime_SuperDuty_L_DOHC_exhaust_camshaft extends DOHC_Camshaft
{
	public MC_Prime_SuperDuty_L_DOHC_exhaust_camshaft( int id )
	{
		super( id );
		name = "Prime DLH left exhaust camshaft";
		description = "";
		brand_new_prestige_value = 45.80;

		value = tHUF2USD(29.512);
		setMaxWear(kmToMaxWear(390000.0));

		maxRPM = 25000.0/2.0;

		static_input_lobe_separation	= 230.0;
		static_input_lift_in_open	= 0.0;
		static_input_lift_in_close	= 0.0;
		static_input_lift_out_open	= 13.462;
		// OpenSLRR #409: a closed valve is seated. 1.27 mm (0.050 in) is
		// the lift at which cam duration is measured, not a leak.
		static_input_lift_out_close	= 0.0;
		static_input_time_in_duration	= 0.0;
		static_input_time_out_duration	= 244.0;

		advance_positive_peak		= +15.0;
		advance_negative_peak		= -15.0;
		advance_minimum_step		= 1.0;

		advance				= 0.0;

		inertia = kgToInertia(4.5);

		check4warnings();
	}
}
