package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Einvagen_PerPro_GTA_cylinder_head extends DOHC_CylinderHead
{
	public Einvagen_PerPro_GTA_cylinder_head( int id )
	{
		super( id );
		name = "Einvagen PerPro GTA cylinder head";
		description = "";
		brand_new_prestige_value = 55.88;

		value = tHUF2USD(115);
		setMaxWear(kmToMaxWear(200000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 33.0;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 30.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		intake_efficiency_tuning = 1.27;
		exhaust_efficiency_tuning = 1.27;

		cc_dome_safe_clearance = 0.0;
		inertia = kgToInertia(4*0.3);

		T_loss = 82.0;
		Vmin = 0.00; // in cc //
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_EINVAGEN)
		{
			if (the_car.model == Einvagen_models.MODEL_140_GTA)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000005Fr, "Einvagen_PerPro_GTA_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000060r, "Einvagen_PerPro_GTA_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000004Br, "Einvagen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000050r, "Einvagen_PerPro_GTA_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000051r, "Einvagen_PerPro_GTA_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000004Dr, "Einvagen_PerPro_GTA_intake_manifold", desc );
				if (desc.power <=1.1 )
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000058r, "Einvagen_PerPro_GTA_exhaust_header", desc );
			}
		}
	}
}
