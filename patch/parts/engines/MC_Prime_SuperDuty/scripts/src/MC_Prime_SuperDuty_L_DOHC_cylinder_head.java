package java.game.parts.engines.MC_Prime_SuperDuty;

import java.util.*;
import java.util.resource.*;
import java.game.*;
import java.game.parts.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.cylinderhead.*;
import java.game.cars.*;


public class MC_Prime_SuperDuty_L_DOHC_cylinder_head extends DOHC_CylinderHead
{
	public MC_Prime_SuperDuty_L_DOHC_cylinder_head( int id )
	{
		super( id );
		name = "Prime DLH left cylinder head";
		description = "";
		brand_new_prestige_value = 73.16;

		value = tHUF2USD(147.559);
		setMaxWear(kmToMaxWear(411000.0));

		intake_manifold_slot_ID = 0;
		exhaust_header_slot_ID = 37;
		camshaft_bearing_slot_ID = 28;
		cover_slot_ID = 22;

		exhaust_camshaft_slot_ID = 26;
		intake_camshaft_slot_ID = 25;

		intake_efficiency_tuning = 1.32;
		exhaust_efficiency_tuning = 1.43;

		intake_valve_diam = 42.5;
		number_of_intake_valves_per_cylinder = 2.0;
		exhaust_valve_diam = 31.0;
		number_of_exhaust_valves_per_cylinder = 2.0;

		cc_dome_safe_clearance = 18.7;

		T_loss = 75.0;
		Vmin = 86.26; // in cc //

		inertia = kgToInertia(4*0.55);

		check4warnings();
	}

	public void addStockParts( Descriptor desc )
	{
		super.addStockParts( desc );

		addPart( parts.engines.MC_Prime_SuperDuty:0x00000046r, "L exhaust header", desc );
		addPart( parts.engines.MC_Prime_SuperDuty:0x0000004Dr, "L DOHC intake camshaft", desc );
		addPart( parts.engines.MC_Prime_SuperDuty:0x0000004Er, "L DOHC exhaust camshaft", desc );
		addPart( parts.engines.MC_Prime_SuperDuty:0x0000004Fr, "L DOHC camshaft bearing bridge", desc );
		addPart( parts.engines.MC_Prime_SuperDuty:0x00000042r, "L DOHC cylinder head cover", desc );
	}
}
