package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Shimutshibu_RC_DDSA1C_cylinder_head extends DOHC_CylinderHead
{
	public Shimutshibu_RC_DDSA1C_cylinder_head( int id )
	{
		super( id );
		name = "Shimutshibu RC-DDSA1C cylinder head";
		description = "The stock head for the RC200.";

		value = tHUF2USD(4*5+65);
		brand_new_prestige_value = 85.42;
		setMaxWear(kmToMaxWear(230000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 30.5;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 29.5;
		number_of_exhaust_valves_per_cylinder = 2.0;

		intake_efficiency_tuning = 1.16;
		exhaust_efficiency_tuning = 1.16;

		cc_dome_safe_clearance = 0.0;

		maxRPM = 18000.0;
		inertia = kgToInertia(4*0.3);

		T_loss = 80.0;
		Vmin = 0.00; // in cc //
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_SHIMUTSHIBU)
		{
			if (the_car.model == Shimutshibu_models.MODEL_FOCER_RC_200)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000102r, "Shimutshibu_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000103r, "Shimutshibu_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000104r, "Shimutshibu_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000105r, "Shimutshibu_RC_200_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000108r, "Shimutshibu_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000109r, "Shimutshibu_RC_HIB535T4_intake_manifold", desc );
				if (desc.power <= 1.1)
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x0000010Ar, "Shimutshibu_RC_23W44EX_exhaust_header", desc );
			}
		}
	}
}
