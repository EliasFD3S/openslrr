package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.block.block_vee.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.cars.*;


public class SuperDuty_5_4_block extends Block_Vee_OHC
{
	public SuperDuty_5_4_block( int id )
	{
		super( id );
		name = "Hauler's SuperDuty 5.4 block";
		description = "";
		brand_new_prestige_value = 69.86;

		value = tHUF2USD(206.582);
		setMaxWear(kmToMaxWear(480000.0));

		bore = 90.0;

		cylinder_length_from_top = 133.43;
		crank_center_to_cylinder_top = 280.60;

		crankshaft_slot_ID = 8;
		transmission_slot_ID = 5;
		crankshaft_bearing_slot_ID = 99;
		oil_pan_slot_ID = 9;

		L_cylinder_head_slot_ID = 45;
		R_cylinder_head_slot_ID = 46;
		intake_manifold_slot_ID = 14;

		rpm_idle = 800.0;
		time_spark_min	=  sparkAngleTo4cycleTime(325.0);
		time_spark_inc	=  sparkAngleTo4cycleTime(340.0)-time_spark_min;
		time_spark_RPM0	= 2400;
		time_spark_RPM1	= 4000;
		RPM_limit	= 6500;

		cylinders	= 8;

		check4warnings();
	}

	public void updatevariables()
	{
		super.updatevariables();

		if (the_car)
		{
			SfxTable tab;
			if (tab = the_car.getSfxTable(0))
			{
				
				tab.clear();
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000A0r),  900.0,  500.0, 1800.0, 0.42, 0.42);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000A2r), 2000.0, 1000.0, 3000.0, 0.42, 0.52);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000A4r), 3000.0, 2000.0, 5500.0, 0.62, 0.72);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000A9r), 5500.0, 3500.0,18000.0, 0.82, 1.00);
			}

			if (tab = the_car.getSfxTable(1))
			{
				tab.clear();
			}

			if (tab = the_car.getSfxTable(2))
			{
				tab.clear();
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000ABr),  900.0*2,  750.0, 1350.0, 1.0, 1.0);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000ACr), 1500.0*2, 1000.0, 2300.0, 1.0, 1.0);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000AEr), 2500.0*2, 1600.0, 3500.0, 1.0, 1.0);
				tab.addItem(new ResourceRef(parts.engines.MC_Prime_SuperDuty:0x000000B1r), 3000.0*2, 2500.0,18000.0, 1.0, 1.0);
				the_car.setSfxExhaustMinVol(0.9);
			}
		}
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		getCar_LocalVersion();

		if (the_car.make == the_car.MAKE_HAULER_S_HEAVEN)
		{
			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_500)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000000Br, "SuperDuty_5_4_crankshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Br, "SuperDuty_crankshaft_bearing_bridge", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000095r, "SuperDuty_oil_pan", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000ADr, "SuperDuty_500_H4L_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000AFr, "SuperDuty_500_H4R_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Ar, "SuperDuty_DOHC_camshaft_drive_belt", desc );
				if (desc.power > 1.25)
				{
					addPart( parts.engines.MC_Prime_SuperDuty:0x00000088r, "supercharger driver pulley", desc );
					addPart( parts.engines.MC_Prime_SuperDuty:0x000000BEr, "SuperDuty_Extra_750_intake_manifold", desc );
				}
				else
					addPart( parts.engines.MC_Prime_SuperDuty:0x000000A1r, "SuperDuty_500_intake_manifold", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000096r, "SuperDuty_alternator", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000097r, "SuperDuty_alternator_drive_belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000098r, "SuperDuty_500_transmission", desc );
			}
                        
/*			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_600)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000011Br, "SuperDuty_600_crankshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Br, "SuperDuty_crankshaft_bearing_bridge", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000095r, "SuperDuty_oil_pan", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000ADr, "SuperDuty_500_H4L_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000AFr, "SuperDuty_500_H4R_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Ar, "SuperDuty_DOHC_camshaft_drive_belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000A1r, "SuperDuty_500_intake_manifold", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000096r, "SuperDuty_alternator", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000097r, "SuperDuty_alternator_drive_belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000198r, "SuperDuty_600_transmission", desc );
			}

			else
*/			if (the_car.model == Hauler_s_models.MODEL_SUPERDUTY_EXTRA_750)
			{
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000000Br, "SuperDuty_5_4_crankshaft", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Br, "SuperDuty_crankshaft_bearing_bridge", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000095r, "SuperDuty_oil_pan", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000ADr, "SuperDuty_500_H4L_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000AFr, "SuperDuty_500_H4R_cylinder_head", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x0000009Ar, "SuperDuty_DOHC_camshaft_drive_belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x000000BEr, "SuperDuty_Extra_750_intake_manifold", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000096r, "SuperDuty_alternator", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000097r, "SuperDuty_alternator_drive_belt", desc );
				addPart( parts.engines.MC_Prime_SuperDuty:0x00000099r, "SuperDuty_Extra_750_transmission", desc );
			}
		}
	}

	public String isDynoable()
	{
		Part p;

		p = partOnSlot( 20 );
		if (!p)
			return "the engine is missing the alternator.";

		return super.isDynoable();
	}
}
