package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Duhen_88er_cylinder_head extends DOHC_CylinderHead
{
	public Duhen_88er_cylinder_head( int id )
	{
		super( id );
		name = "Duhen SunStrip 1.8 DVC cylinder head";
		description = "";
		brand_new_prestige_value = 55.88;

		value = tHUF2USD(4*55+67);
		setMaxWear(kmToMaxWear(230000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 30.0;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 26.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		intake_efficiency_tuning = 1.14;
		exhaust_efficiency_tuning = 1.14;

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

		if (the_car.make == the_car.MAKE_DUHEN)
		{
			if (the_car.model == Duhen_models.MODEL_SUNSTRIP_1_8_DVC)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C3r, "Duhen_DVC_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C2r, "Duhen_DVC_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C4r, "Duhen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000CBr, "Duhen_D18V_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C7r, "Duhen_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C6r, "Duhen_SolarTunnel_intake_manifold", desc );
				if (desc.power <= 1.2 || desc.power > 1.4)
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C8r, "Duhen_SolarWind_exhaust_header", desc );
			}
		}
	}
}
