package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.cars.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;


public class MC_Prime_SuperDuty_supercharger_driver_pulley extends Pulley
{
	public MC_Prime_SuperDuty_supercharger_driver_pulley( int id )
	{
		super( id );
		name = "Vee-8 supercharger driver pulley";
		description = "";
		brand_new_prestige_value = 14.34;

		value = tHUF2USD(14.756);
		setMaxWear(kmToMaxWear(400000.0));

		maxRPM = 120000.0;

		inertia = kgToInertia(0.05);

		surface_diameter = 90.0;
		surface_width = 60.0;
		check4warnings();
	}
}
