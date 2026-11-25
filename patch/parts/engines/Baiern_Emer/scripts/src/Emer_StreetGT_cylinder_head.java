package java.game.parts.engines.Baiern_Emer;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.camshaft.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class Emer_StreetGT_cylinder_head extends DOHC_CylinderHead
{
	public Emer_StreetGT_cylinder_head( int id )
	{
		super( id );
		name = "Emer Nonus StreetGT cylinder head";
		description = "";
		brand_new_prestige_value = 80.02;

		value = tHUF2USD(6*13+43);
		setMaxWear(kmToMaxWear(317000.0));

		intake_manifold_slot_ID = 11;
		exhaust_header_slot_ID = 13;
		camshaft_bearing_slot_ID = 24;
		cover_slot_ID = 18;

		exhaust_camshaft_slot_ID = 4;
		intake_camshaft_slot_ID = 5;

		intake_efficiency_tuning = 1.18;
		exhaust_efficiency_tuning = 1.18;

		intake_valve_diam = 31.0;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 29.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		cc_dome_safe_clearance = 18.7;
		maxRPM = 20000.0;
		inertia = kgToInertia(6*0.22);

		T_loss = 80.0;
		Vmin = 86.8; // in cc //

		check4warnings();
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_EMER)
		{
			if (the_car.model == Emer_models.MODEL_NONUS_STREET_GT)
			{
				DOHC_Camshaft ec;
				DOHC_Camshaft ic;
				if (desc.power < 1.2)
				{
					ec = addPart( parts.engines.Baiern_Emer:0x00000089r, "Emer_StreetGT_exhaust_camshaft", desc );
					ic = addPart( parts.engines.Baiern_Emer:0x0000008Ar, "Emer_StreetGT_intake_camshaft", desc );
					if (desc.power > 1.1)
					{
						ec.advance = -15.0;
						ic.advance =  15.0;
					}
				}
				else
				{
					ec = addPart( parts.engines.Baiern_Emer:0x000000E0r, "Emer_GT2_exhaust_camshaft", desc );
					ic = addPart( parts.engines.Baiern_Emer:0x000000E1r, "Emer_GT2_intake_camshaft", desc );
					if (desc.power > 1.3)
					{
						ec.advance = -15.0;
						ic.advance =  15.0;
					}
				}
				addPart( parts.engines.Baiern_Emer:0x0000008Br, "Emer_camshaft_bearing_bridge", desc );
				addPart( parts.engines.Baiern_Emer:0x0000008Fr, "Emer_StreetGT_cylinder_head_cover", desc );
				addPart( parts.engines.Baiern_Emer:0x00000088r, "Emer_camshaft_drive_belt", desc );
				if (desc.power < 1.1)
					addPart( parts.engines.Baiern_Emer:0x00000090r, "Emer_StreetGT_intake_manifold", desc );
				else
					addPart( parts.engines.Baiern_Emer:0x00000092r, "Emer_MotorSport_GT2_intake_manifold", desc );
				if (desc.power < 1.1)
					addPart( parts.engines.Baiern_Emer:0x0000008Cr, "Emer_StreetGT_exhaust_header", desc );
				else
					addPart( parts.engines.Baiern_Emer:0x0000008Dr, "Emer_MotorSport_GT2_exhaust_header", desc );
			}
		}
	}

	public String isDynoable()
	{
		Part p;

		p = partOnSlot( 22 );
		if (!p)
			return "the engine is missing the camshaft drive belt.";

		return super.isDynoable();
	}
}
