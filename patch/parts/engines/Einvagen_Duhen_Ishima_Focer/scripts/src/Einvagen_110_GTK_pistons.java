package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;


public class Einvagen_110_GTK_pistons extends Piston
{
	public Einvagen_110_GTK_pistons( int id )
	{
		super( id );
		name = "Einvagen 110 GTK pistons";
		description = "These pistons are built into the supercharged Einvagen 110 GTK. A pack of four pistons capable of up to 8750 RPM. For 80.0 mm (3.15 inch) bore blocks.";
		brand_new_prestige_value = 27.94;

		value = tHUF2USD(4*14.5);
		setMaxWear(kmToMaxWear(305000.0));

		bore = 80.0;
		length_from_wrist_pin_center_to_crown_base = 57.57;
		crown_height = 0.00;
		crown_volume = -20.0;
		length_from_lowest_safe_belt_to_crown_base = 34.15;

		maxRPM = 8750;
		inertia = kgToInertia(4*0.370);
	}
}
