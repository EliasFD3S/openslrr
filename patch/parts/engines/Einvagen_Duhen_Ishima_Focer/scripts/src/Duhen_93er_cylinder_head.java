package java.game.parts.engines.Einvagen_Duhen_Ishima_Focer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Duhen_93er_cylinder_head extends DOHC_CylinderHead
{
	public Duhen_93er_cylinder_head( int id )
	{
		super( id );
		name = "Duhen SunStrip 2.2/Racing SunStrip 2.0 cylinder head";
		description = "";
		brand_new_prestige_value = 55.88;

		value = tHUF2USD(4*6+72);
		setMaxWear(kmToMaxWear(230000.0));

		intake_manifold_slot_ID = 27;
		exhaust_header_slot_ID = 49;
		camshaft_bearing_slot_ID = 31;
		cover_slot_ID = 29;

		exhaust_camshaft_slot_ID = 35;
		intake_camshaft_slot_ID = 33;

		intake_valve_diam = 31.5;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 28.5;
		number_of_exhaust_valves_per_cylinder = 2.0;

		intake_efficiency_tuning = 1.36;
		exhaust_efficiency_tuning = 1.36;

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
			if (the_car.model == Duhen_models.MODEL_SUNSTRIP_2_2_DVC)
			{
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000111r, "Duhen_CDVC_exhaust_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000112r, "Duhen_CDVC_intake_camshaft", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C4r, "Duhen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000CCr, "Duhen_D22V_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C7r, "Duhen_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C6r, "Duhen_SolarTunnel_intake_manifold", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C8r, "Duhen_SolarWind_exhaust_header", desc );
				if (desc.power <= 1.2 || desc.power > 1.6)
					addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C8r, "Duhen_SolarWind_exhaust_header", desc );
			}
			else
			if (the_car.model == Duhen_models.MODEL_RACING_SUNSTRIP_2_0_CDVC)
			{
				Camshaft cs;
				cs=
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000111r, "Duhen_CDVC_exhaust_camshaft", desc );
				cs.default_advance = cs.advance = +5.0;
				cs=
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x00000112r, "Duhen_CDVC_intake_camshaft", desc );
				cs.default_advance = cs.advance = -5.0;
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C4r, "Duhen_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000CEr, "Duhen_D20CV_cylinder_head_cover", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C7r, "Duhen_camshaft_drive_belt", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C6r, "Duhen_SolarTunnel_intake_manifold", desc );
				addPart( parts.engines.Einvagen_Duhen_Ishima_Focer:0x000000C8r, "Duhen_SolarWind_exhaust_header", desc );
			}
		}
	}
}
