package java.game.parts.enginepart;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.rgearpart.*;

public class AirFuelDeliverySystem extends EnginePart
{
	// constants //
	static int	FT_GAS_95	= 0;
	static int	FT_GAS_98	= 1;
	static int	FT_GAS_100	= 2;
	static int	FT_METHANOL	= 3;
	static int	FT_DIESEL	= 4;

	// OpenSLRR #407: handbook fuel data. Pump 95 is E5 and pump 98 is E10
	// (ethanol by volume); 100 octane race fuel has no ethanol.
	static float	GASOLINE_LHV		= 43400000.0;	// J/kg //
	static float	GASOLINE_AFR		= 14.7;
	static float	GASOLINE_DENSITY	= 0.745;	// kg/l //
	static float	ETHANOL_LHV		= 26800000.0;
	static float	ETHANOL_AFR		= 9.0;
	static float	ETHANOL_DENSITY		= 0.789;
	static float	METHANOL_LHV		= 19900000.0;
	static float	METHANOL_AFR		= 6.47;	// CH3OH + 1.5 O2 //
	static float	DIESEL_LHV		= 43400000.0;
	static float	DIESEL_AFR		= 14.5;

	// The mixture range reaches methanol's best power (about 5:1).
	static float	MIN_MIXTURE_RATIO	= 4.0;
	static float	MAX_MIXTURE_RATIO	= 20.0;

	// input //
	float	mixture_ratio		= 1.0;
	float	optimal_mixture_ratio	= 1.0;
	float	max_fuel_consumption	= 0.000001;
	float	max_air_consumption	= 0.00001;

	// selectables //
	int	fuel_type		= FT_GAS_95;

	// output //
	float	mixture_H		= 1.0;
	float	time_burn		= 0.1;
	float	nitro_H			= 1.0;
	float	nitro_cooling		= 1.0;
	float	nitro_consumption	= 1.0;
	float	nitro_minRPM		= 0.0;
	float	nitro_maxRPM		= -1.0;
	float	nitro_minThrottle	= 0.0;

	float	optimal_burn_time_base	= 0.003;
	float	optimal_burn_time_offset= 0.001;

	float	minComp			= 0.0;
	float	maxComp			= 0.0;
	float	knockComp		= 0.0;

	public AirFuelDeliverySystem(){}

	public AirFuelDeliverySystem( int id )
	{
		super( id );

		name = "Air/Fuel delivery system";
	}

	public void calcStuffs()
	{
		optimal_mixture_ratio = bestPowerRatio(fuel_type);
	}

	// Ethanol mass fraction of a pump gasoline, from its share by volume.
	public float ethanolMassFraction( int ft )
	{
		float volume = 0.0;

		if (ft == FT_GAS_95)
			volume = 0.05;
		else
		if (ft == FT_GAS_98)
			volume = 0.10;

		float ethanol = volume*ETHANOL_DENSITY;
		return ethanol/(ethanol+(1.0-volume)*GASOLINE_DENSITY);
	}

	// Stoichiometric air:fuel ratio by mass.
	public float stoichiometricRatio( int ft )
	{
		if (ft == FT_METHANOL)
			return METHANOL_AFR;
		else
		if (ft == FT_DIESEL)
			return DIESEL_AFR;

		float w = ethanolMassFraction(ft);
		return (1.0-w)*GASOLINE_AFR+w*ETHANOL_AFR;
	}

	// Lower heating value, J/kg of fuel.
	public float lowerHeatingValue( int ft )
	{
		if (ft == FT_METHANOL)
			return METHANOL_LHV;
		else
		if (ft == FT_DIESEL)
			return DIESEL_LHV;

		float w = ethanolMassFraction(ft);
		return (1.0-w)*GASOLINE_LHV+w*ETHANOL_LHV;
	}

	// Best-power mixture: gasolines at lambda 0.86, methanol at 0.78; diesel
	// stays lean of stoichiometric (smoke limit, lambda 1.25).
	public float bestPowerRatio( int ft )
	{
		if (ft == FT_METHANOL)
			return stoichiometricRatio(ft)*0.78;
		else
		if (ft == FT_DIESEL)
			return stoichiometricRatio(ft)*1.25;

		return stoichiometricRatio(ft)*0.86;
	}

	// Highest static compression that runs at full spark advance on a
	// naturally aspirated 20 C charge. The host retards the spark above it,
	// boost and intercooling included. Diesel has no spark knock.
	public float knockLimitedCompression( int ft )
	{
		if (ft == FT_GAS_98)
			return 11.5;
		else
		if (ft == FT_GAS_100)
			return 12.5;
		else
		if (ft == FT_METHANOL)
			return 15.0;
		else
		if (ft == FT_DIESEL)
			return 0.0;

		return 10.5; // FT_GAS_95 //
	}

	public void updatevariables()
	{
//		System.log("----->>>>> updating "+name+":");
		// OpenSLRR #203/#407: H is the heat released per kg of inducted air at
		// the fuel's best mixture, lower heating value / stoichiometric ratio
		// of the fuel blend.
		int ft = getFuelType();

		if (ft == FT_METHANOL)
			police_check_fine_value = 200;
		else
			police_check_fine_value = 0;

		// stock flame speeds //
		if (ft == FT_GAS_98)
		{
//			System.log("  fuel type: 98 octane pump gas");
			optimal_burn_time_base	= 0.00247;
			optimal_burn_time_offset= 0.00075;
		}
		else
		if (ft == FT_GAS_100)
		{
//			System.log("  fuel type: 100 octane premium gas");
			optimal_burn_time_base	= 0.00242;
			optimal_burn_time_offset= 0.00065;
		}
		else
		if (ft == FT_METHANOL)
		{
//			System.log("  fuel type: racing methanol");
			optimal_burn_time_base	= 0.00225;
			optimal_burn_time_offset= 0.00040;
		}
		else
		if (ft == FT_DIESEL)
		{
//			System.log("  fuel type: diesel oil");
			optimal_burn_time_base	= 0.00340;
			optimal_burn_time_offset= 0.00030;
		}
		else
		{
//			System.log("  fuel type: 95 octane pump gas");
			optimal_burn_time_base	= 0.00250;
			optimal_burn_time_offset= 0.00080;
		}

		optimal_mixture_ratio = bestPowerRatio(ft);
		float H = lowerHeatingValue(ft)/stoichiometricRatio(ft); // J/kg //

		float ratio = clampTo(getMixtureRatio(), MIN_MIXTURE_RATIO, MAX_MIXTURE_RATIO);
		float ratio_detuning_ratio;

//		System.log("     current air:fuel ratio: "+ratio+":1.0");
//		System.log("     optimal air:fuel ratio: "+optimal_mixture_ratio+":1.0");

		if (ratio < optimal_mixture_ratio)
			ratio_detuning_ratio = ratio/optimal_mixture_ratio;
		else
			ratio_detuning_ratio = optimal_mixture_ratio/ratio;

		ratio_detuning_ratio = 0.45*ratio_detuning_ratio+0.55;
		ratio_detuning_ratio = ratio_detuning_ratio*ratio_detuning_ratio;
		ratio_detuning_ratio = ratio_detuning_ratio*ratio_detuning_ratio*ratio_detuning_ratio;
		ratio_detuning_ratio = 1.0 - ratio_detuning_ratio;

 		mixture_H = H*(0.8+0.2*(1.0-ratio_detuning_ratio));
		time_burn = (optimal_burn_time_base+optimal_burn_time_offset*ratio_detuning_ratio)*0.25;

		// Knock is a spark retard in the host. The window below only refuses
		// engines that cannot run on the fuel at all: a diesel needs
		// compression ignition, and far above the knock limit the retard no
		// longer prevents pre-ignition.
		knockComp = knockLimitedCompression(ft);
		if (ft == FT_DIESEL)
		{
			minComp = 14.0;
			maxComp = 24.0;
		}
		else
		if (ft == FT_METHANOL)
		{
			minComp = 8.0;
			maxComp = knockComp+5.5;
		}
		else
		{
			minComp = 6.5;
			maxComp = knockComp+5.5;
		}

//		System.log("     minComp = "+minComp);
//		System.log("     maxComp = "+maxComp);
//		System.log("     knockComp = "+knockComp);

//		System.log("     detuning_coeff.: "+(ratio_detuning_ratio*100.0)+"%");
//		System.log("     burn time: "+(time_burn*1000.0)+" ms");
//		System.log("     heat value: "+mixture_H+" J/kg");
//		System.log("----->>>>> end of updating "+name);
	}

	public int getFuelType()
	{
		return fuel_type;
	}

	public String getFuelTypeName()
	{
		if (fuel_type == FT_GAS_95)
			return "95 octane pump gas (E5)";
		else
		if (fuel_type == FT_GAS_98)
			return "98 octane pump gas (E10)";
		else
		if (fuel_type == FT_GAS_100)
			return "100 octane premium gas";
		else
		if (fuel_type == FT_METHANOL)
			return "racing methanol";
		else
		if (fuel_type == FT_DIESEL)
			return "diesel oil";

		return "unknown fuel type";
	}

	public float getMixtureRatio()
	{
		return mixture_ratio;
	}

	public float getMaxFuelConsumption()
	{
		return max_fuel_consumption;
	}

	public float getMaxAirConsumption()
	{
		return max_air_consumption;
	}
}
