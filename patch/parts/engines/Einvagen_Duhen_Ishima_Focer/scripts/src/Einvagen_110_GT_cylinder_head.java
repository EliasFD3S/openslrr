package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Einvagen_110_GT_cylinder_head extends DOHC_CylinderHead
{
	public Einvagen_110_GT_cylinder_head( int id )
	{
		super( id );
		name = "Einvagen 110 GT/GTK cylinder head";
		description = "The stock head on the 110 cui GT and GTK engines. Features four valves per cylinder, two 30.0 mm (1.181 inch) intake and two 27.0 mm (1.063 inch) exhaust.";
		brand_new_prestige_value = 55.88;

		value = tHUF2USD(4*5+65);
		setMaxWear(kmToMaxWear(200000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 30.0;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 27.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		cc_dome_safe_clearance = 0.0;
		inertia = kgToInertia(4*0.3);

		maxRPM = 18000.0;
		inertia = kgToInertia(4*0.26);

		T_loss = 86.0;
		exhaust_efficiency_tuning = 0.96;
		intake_efficiency_tuning = 0.96;
		Vmin = 0.00; // in cc //
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_EINVAGEN)
		{
			if (the_car.model == Einvagen_models.MODEL_110_GT)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DCr, "Einvagen_110_GT_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DDr, "Einvagen_110_GT_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000004Br, "Einvagen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000D9r, "Einvagen_110_GT_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000010Fr, "Einvagen_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DBr, "Einvagen_110_GT_intake_manifold", desc );
				if (desc.power <= 1.2)
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000008r, "Einvagen_110_GT_exhaust_header", desc );
			}
			else
			if (the_car.model == Einvagen_models.MODEL_110_GTK)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DCr, "Einvagen_110_GT_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DDr, "Einvagen_110_GT_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000004Br, "Einvagen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DAr, "Einvagen_110_GTK_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000010Fr, "Einvagen_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000DBr, "Einvagen_110_GT_intake_manifold", desc );
				if (desc.power <= 1.25)
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000008r, "Einvagen_110_GT_exhaust_header", desc );
			}
		}
	}
}
