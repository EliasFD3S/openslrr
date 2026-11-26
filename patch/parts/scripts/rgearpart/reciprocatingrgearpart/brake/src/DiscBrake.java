package java.game.parts.rgearpart.reciprocatingrgearpart.brake;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.rgearpart.*;
import java.game.parts.rgearpart.reciprocatingrgearpart.*;

public class DiscBrake extends Brake
{
	final static float	BRAKE_FORCE_FINE_TUNE = 2.0;

	final static float	BP_RED_STUFF	= 1.00;
	final static float	BP_GREEN_STUFF	= 1.05;
	final static float	BP_BLACK_STUFF	= 1.10;

	final static float	RT_STEEL	= 1.000;
	final static float	RT_GROOVED	= 1.025;
	final static float	RT_VENTEDGROOVED= 1.050;
	final static float	RT_CARBON	= 1.150;

	final static float	CL_FORCE_1	=  950.0*9.81*BRAKE_FORCE_FINE_TUNE;
	final static float	CL_FORCE_BRUTAL	= 1100.0*9.81*BRAKE_FORCE_FINE_TUNE;
	final static float	CL_RACEFORCE	= 1400.0*9.81*BRAKE_FORCE_FINE_TUNE;

	// OpenSLRR (#446): the torque comes from the hardware. The pedal at full
	// travel gives LINE_PRESSURE; each pad face presses with the pistons of
	// one side of the caliper (number_of_calipers of them, piston_mm across;
	// one is a floating caliper's single piston). Torque = 2 faces x pad
	// friction x pressure x piston area x caliper stiffness x the radius of
	// the pad's centre (25 mm inside the rotor's edge). The force and
	// friction constants above keep naming the parts and setting the price.
	final static float	LINE_PRESSURE	= 10000000.0;	// 100 bar

	float			number_of_calipers = 1.0;
	float			piston_mm	= 0.0;	// 0: 54, 40 or 34 mm for 1, 2 or 3 per side
	float			diam_mm		= 275.0;
	float			friction_pad	= BP_RED_STUFF;
	float			friction_disc	= RT_STEEL;

	public DiscBrake( int id )
	{
		super( id );

		name = "Disc brake";

		prestige_calc_weight = 20.0;

		force = CL_FORCE_1;
	}

	public void updatevariables()
	{
		float w = getWear();
		if (w>0)
			w = Math.sqrt(w);
		else
			w = 0.0;
		float piston = piston_mm;
		if (piston <= 0.0)
		{
			if (number_of_calipers >= 3.0)
				piston = 34.0;
			else
			if (number_of_calipers >= 2.0)
				piston = 40.0;
			else
				piston = 54.0;
		}
		float area = number_of_calipers*3.14159265*piston*piston/4000000.0;
		float arm = radius-0.025;
		if (arm < 0.05)
			arm = 0.05;
		torque = (0.2+w*0.8)*2.0*padFriction()*LINE_PRESSURE*area*caliperStiffness()*arm;

		pad_compound = 0.0;
		if (friction_pad == BP_GREEN_STUFF)
			pad_compound = 1.0;
		else
		if (friction_pad == BP_BLACK_STUFF)
			pad_compound = 2.0;
		rotor_type = 0.0;
		if (friction_disc == RT_GROOVED)
			rotor_type = 1.0;
		else
		if (friction_disc == RT_VENTEDGROOVED)
			rotor_type = 2.0;
		else
		if (friction_disc == RT_CARBON)
			rotor_type = 3.0;

		super.updatevariables();
	}

	// Working friction of the pad compounds (street, fast road, race).
	public float padFriction()
	{
		if (friction_pad == BP_BLACK_STUFF)
			return 0.50;
		if (friction_pad == BP_GREEN_STUFF)
			return 0.45;
		return 0.40;
	}

	// Stiffer calipers flex less and waste less of the pressure.
	public float caliperStiffness()
	{
		if (force == CL_RACEFORCE)
			return 1.10;
		if (force == CL_FORCE_BRUTAL)
			return 1.05;
		return 1.0;
	}

	public void calcStuffs()
	{
		radius = diam_mm/2.0/1000.0;
		friction = friction_pad*friction_disc;
		updatevariables();
		value = brand_prestige_factor * HUF2USD( diam_mm*50.0*friction_disc + force*number_of_calipers + friction_pad*number_of_calipers*2.0*2000.0);
		brand_new_prestige_value = value / 8.0;

		int dmmI=diam_mm;
		int number_of_calipersI=number_of_calipers*2;
		float piston = piston_mm;
		if (piston <= 0.0)
		{
			if (number_of_calipers >= 3.0)
				piston = 34.0;
			else
			if (number_of_calipers >= 2.0)
				piston = 40.0;
			else
				piston = 54.0;
		}
		int pistonI=piston;

		String rotor_mat_text="";
		if (friction_disc == RT_STEEL)
			rotor_mat_text="steel";
		else
		if (friction_disc == RT_GROOVED)
			rotor_mat_text="grooved";
		else
		if (friction_disc == RT_VENTEDGROOVED)
			rotor_mat_text="vented";
		else
		if (friction_disc == RT_CARBON)
		{
			rotor_mat_text="carbon";
			police_check_fine_value = 100;
		}

		String caliper_mat_text = "";
		if (force == CL_FORCE_1)
			caliper_mat_text="conventional street";
		else
		if (force == CL_FORCE_BRUTAL)
			caliper_mat_text="SL Tuners Street";
		else
		if (force == CL_RACEFORCE)
		{
			caliper_mat_text="SL Tuners Race/Track";
			police_check_fine_value = 100;
		}

		String pad_mat_text = "";
		if (friction_pad == BP_RED_STUFF)
			pad_mat_text="conventional street";
		else
		if (friction_pad == BP_GREEN_STUFF)
			pad_mat_text="SL Tuners Green";
		else
		if (friction_pad == BP_BLACK_STUFF)
			pad_mat_text="SL Tuners Black";

		name = name_prefix + " " + dmmI+"mm "+number_of_calipersI+" piston disc brake";
		description = "A disc brake utilizing "+dmmI+"mm "+rotor_mat_text+" rotor, "+number_of_calipersI+" "+pistonI+"mm "+caliper_mat_text+" pistons and "+pad_mat_text+" brake pads. Max torque: "+torque+" Nm.";
	}
}
