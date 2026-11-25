package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Ishima_WR_head_cylinder_head extends DOHC_CylinderHead
{
	public Ishima_WR_head_cylinder_head( int id )
	{
		super( id );
		name = "Ishima WR head cylinder head";
		description = "";
		brand_new_prestige_value = 85.42;

		value = tHUF2USD(4*5+65);
		setMaxWear(kmToMaxWear(230000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 32.0;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 30.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		intake_efficiency_tuning = 1.1;
		exhaust_efficiency_tuning = 1.1;

		cc_dome_safe_clearance = 0.0;

		maxRPM = 18000.0;
		inertia = kgToInertia(4*0.3);

		T_loss = 80.0;
		Vmin = 8.00; // in cc //
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_ISHIMA)
		{
			if (the_car.model == Ishima_models.MODEL_ENULA_WRY)
			{
				Camshaft cam;
				cam = addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EAr, "Ishima_exhaust_camshaft", desc );
				cam.advance = 10.0;
				cam = addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EBr, "Ishima_intake_camshaft", desc );
				cam.advance = -15.0;
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000ECr, "Ishima_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EDr, "Ishima_WRY_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F0r, "Ishima_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F1r, "Ishima_TunnelFold_intake_manifold", desc );
				if (desc.power <= 1.1 )
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F2r, "Ishima_EX_head_exhaust_header", desc );
			}
			else
			if (the_car.model == Ishima_models.MODEL_ENULA_WRZ)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EAr, "Ishima_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EBr, "Ishima_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000ECr, "Ishima_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EEr, "Ishima_WRZ_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F0r, "Ishima_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F1r, "Ishima_TunnelFold_intake_manifold", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F2r, "Ishima_EX_head_exhaust_header", desc );
			}
			else
			if (the_car.model == Ishima_models.MODEL_ENULA_WR_SUPERTURIZMO)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EAr, "Ishima_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EBr, "Ishima_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000ECr, "Ishima_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000EFr, "Ishima_WR_SuperTurizmo_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F0r, "Ishima_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F1r, "Ishima_TunnelFold_intake_manifold", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000F2r, "Ishima_EX_head_exhaust_header", desc );
			}
		}
	}
}
