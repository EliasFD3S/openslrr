package java.game.parts.enginepart.airfueldeliverysystem.charger;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.rgearpart.*;
import java.game.parts.enginepart.airfueldeliverysystem.*;

public class SuperCharger extends Charger
{
	float	pulley_diameter = 1.0; // in milimeters //
	float	pulley_width	= 1.0; // in milimeters //
	float	default_rpm_turbo_mul	= 1.0;

	// OpenSLRR #415: positive-displacement blower //
	float	leak_rpm		= 300.0;	// blower rpm whose flow equals the internal leakage
	float	adiabatic_efficiency	= 0.55;	// Roots blower
	float	belt_tension_per_mm	= 27.0;	// N per mm of belt width (8 mm pitch timing belt)
	int	belt_slot_ID		= 0;	// drive belt slot, 0 = none

	public SuperCharger( int id )
	{
		super( id );

		name = "supercharger";
	}

	public void updatevariables()
	{
	}

	public float kmToMaxWear(float km)
	{
		if (rpm_turbo_mul>0.0)
			return super.kmToMaxWear(km)*(default_rpm_turbo_mul/rpm_turbo_mul)*(default_rpm_turbo_mul/rpm_turbo_mul);
		return super.kmToMaxWear(km);
	}

	public void fillDynoData( DynoData dd, int parentSlot )
	{
		updatevariables();

		super.fillDynoData(dd,parentSlot);

		// The crank and blower pulleys set the drive ratio; without a driver
		// pulley part the stock ratio stays.
		if (dd.crank_pulley_diameter > 1.0 && pulley_diameter > 1.0)
			rpm_turbo_mul = dd.crank_pulley_diameter/pulley_diameter;

		dd.rpm_turbo_mul = rpm_turbo_mul;
		dd.rpm_turbo_opt = rpm_turbo_opt*rpm_turbo_mul*rpm_turbo_mul;
		dd.rpm_turbo_range = rpm_turbo_range*rpm_turbo_mul*rpm_turbo_mul;
		// A positive-displacement blower delivers in proportion to its speed:
		// P_turbo_max is the boost at the stock ratio.
		if (default_rpm_turbo_mul>0.0)
			dd.P_turbo_max = (1.0+P_turbo_max)*rpm_turbo_mul/default_rpm_turbo_mul-1.0;
		else
			dd.P_turbo_max = P_turbo_max;
		if (dd.P_turbo_max < 0.0)
			dd.P_turbo_max = 0.0;

		dd.charger_type = 1.0;
		dd.charger_leak_rpm = leak_rpm;
		dd.charger_efficiency = adiabatic_efficiency;
		dd.charger_pulley_radius = pulley_diameter*0.0005;
		// The belt as wide as the blower pulley; a worn belt holds less.
		dd.charger_belt_force = 0.0;
		if (belt_slot_ID)
		{
			Part belt = partOnSlot( belt_slot_ID );
			if (belt)
				dd.charger_belt_force = pulley_width*belt_tension_per_mm*belt.getWear();
		}

		if (P_turbo_waste>0.0)
			dd.P_turbo_waste = P_turbo_waste;
		else
			dd.P_turbo_waste = dd.P_turbo_max;
	}
}
