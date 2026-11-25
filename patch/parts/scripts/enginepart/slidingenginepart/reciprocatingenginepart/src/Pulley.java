package java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.slidingenginepart.*;
import java.game.parts.rgearpart.*;

public class Pulley extends ReciprocatingEnginePart
{
	float	surface_diameter = 1.0; // in milimeters //
	float	surface_width = 1.0; // in milimeters //

	public Pulley( int id )
	{
		super( id );

		name = "Pulley";

		prestige_calc_weight = 2.5;
	}

	// OpenSLRR #415: the crank pulley that drives a supercharger sets its
	// drive ratio with the blower pulley.
	public void fillDynoData( DynoData dd, int parentSlot )
	{
		dd.crank_pulley_diameter = surface_diameter;
		super.fillDynoData( dd, parentSlot );
	}
}
