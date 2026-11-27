package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class SuperDuty_500_H4R_cylinder_head extends DOHC_CylinderHead
{
	public SuperDuty_500_H4R_cylinder_head( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 500 H4R right cylinder head";
		description = "";
		brand_new_prestige_value = 73.16;

		value = tHUF2USD(147.559);
		setMaxWear(kmToMaxWear(421000.0));

		intake_manifold_slot_ID = 0;
		exhaust_header_slot_ID = 16;
		camshaft_bearing_slot_ID = 34;
		cover_slot_ID = 23;

		exhaust_camshaft_slot_ID = 32;
		intake_camshaft_slot_ID = 31;

		intake_efficiency_tuning = 1.38;
		exhaust_efficiency_tuning = 1.5;

		intake_valve_diam = 42.5;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 31.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		cc_dome_safe_clearance = 18.7;

		T_loss = 75.0;
		Vmin = 86.26; // in cc //

		maxRPM = 9000.0;
		inertia = kgToInertia(4*0.55);

		check4warnings();
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_HAULER_S_HEAVEN)
		{
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_500)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B2r, "SuperDuty_R_exhaust_header", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B5r, "SuperDuty_R_intake_camshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B7r, "SuperDuty_R_exhaust_camshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000BAr, "SuperDuty_R_camshaft_bearing_bridge", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000BCr, "SuperDuty_R_cylinder_head_cover", desc );
			}
			else
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B2r, "SuperDuty_R_exhaust_header", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B5r, "SuperDuty_R_intake_camshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000B7r, "SuperDuty_R_exhaust_camshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000BAr, "SuperDuty_R_camshaft_bearing_bridge", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000BCr, "SuperDuty_R_cylinder_head_cover", desc );
			}
		}
	}
}
