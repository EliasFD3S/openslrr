package java.game.parts.enginepart.block;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.rgearpart.*;
import java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.*;

public class Block_Vee extends Block
{
	int	L_cylinder_head_slot_ID = 0;
	int	R_cylinder_head_slot_ID = 0;
	int	intake_manifold_slot_ID = 0;

	public Block_Vee(){}

	public Block_Vee( int id )
	{
		super( id );
	}

	public Part getLeftCylinderHead()
	{
		if (L_cylinder_head_slot_ID <= 0)
			return null;

		Part res = partOnSlot(L_cylinder_head_slot_ID);

		if (res && res instanceof CylinderHead)
			return res;
//		else
//			System.log("!!!Cylinder head required on slot!!!");

		return null;
	}

	public Part getRightCylinderHead()
	{
		if (R_cylinder_head_slot_ID <= 0)
			return null;

		Part res = partOnSlot(R_cylinder_head_slot_ID);

		if (res && res instanceof CylinderHead)
			return res;
//		else
//			System.log("!!!Cylinder head required on slot!!!");

		return null;
	}

	public Part getIntakeManifold()
	{
		if (intake_manifold_slot_ID <= 0)
			return null;

		Part res = partOnSlot(intake_manifold_slot_ID);

		if (res && res instanceof IntakeManifold)
			return res;
//		else
//			System.log("!!!Intake manifold required on slot!!!");

		return null;
	}

	public void updatevariables()
	{
		skip_end_of_update += 1;

		super.updatevariables();

		IntakeManifold lim = null;
		IntakeManifold rim = null;
		IntakeManifold im = getIntakeManifold();
		if (im)
		{
			im.updatevariables();
			im.the_car = the_car;
			im.fillDynoData(dynodata,slotIDOnSlot(intake_manifold_slot_ID));
		}

		float bore_area = bore*bore*0.01*0.25*Math.PI;
		float cylinder_volume = 1000000000000000.0;

		Piston piston=null;
		ConnectingRod connecting_rod=null;
		Crankshaft crank=getCrankshaft();
		if (crank)
		{
			connecting_rod=crank.getConnectingRod();
			if (connecting_rod)
				piston = connecting_rod.getPiston();

			cylinder_volume = bore_area*crank.stroke*0.1;
		}

		DynoData ldd = dynodata.clone();
		float lexhaust_efficiency = 1.0;
		float lintake_efficiency = 0.0;
		CylinderHead lch=getLeftCylinderHead();

		if (lch)
		{
			lch.updatevariables();
			lch.the_car = the_car;
			lch.fillDynoData(ldd,slotIDOnSlot(L_cylinder_head_slot_ID));
			total_sliction_loss *= lch.getSlictionLoss();
			inertia += lch.getInertia();

			if (piston)
			{
				float TDC_height_offset = ((crank_center_to_cylinder_top-crank.stroke*0.5-connecting_rod.length-piston.length_from_wrist_pin_center_to_crown_base)*0.1);

				float tvm2=bore_area*TDC_height_offset;
				float temp_vmin=(lch.Vmin-piston.crown_volume+tvm2);

				ldd.Vmin = temp_vmin*0.000001;
			}
			else
				ldd.Vmin = 1000000000000000.0;

			if (!im)
				lim = lch.getIntakeManifold();

			ExhaustHeader leh = lch.getExhaustHeader();
			if (leh)
				lexhaust_efficiency = leh.efficiency*lch.exhaust_efficiency_tuning;

			if (im)
				lintake_efficiency = im.efficiency*lch.intake_efficiency_tuning;
			else
			if (lim)
				lintake_efficiency = lim.efficiency*lch.intake_efficiency_tuning;
			else
			{
				ldd.mixture_ratio = 1.0;
				ldd.max_fuel_consumption = 0.0;
				ldd.max_air_consumption = 0.0;
				ldd.mixture_H = 1.0;
				ldd.time_burn = 0.1;
			}

			// #203: normalize each bank before the existing DynoData average.
			ldd.in_min = lch.portFlowCoefficient(lch.in_min, lch.intake_valve_diam, lch.intake_tunnel_area, cylinder_volume, lintake_efficiency, in_breather_magic);
			ldd.in_max = lch.portFlowCoefficient(lch.in_max, lch.intake_valve_diam, lch.intake_tunnel_area, cylinder_volume, lintake_efficiency, in_breather_magic);
			ldd.out_min = lch.portFlowCoefficient(lch.out_min, lch.exhaust_valve_diam, lch.exhaust_tunnel_area, cylinder_volume, lexhaust_efficiency, ex_breather_magic);
			ldd.out_max = lch.portFlowCoefficient(lch.out_max, lch.exhaust_valve_diam, lch.exhaust_tunnel_area, cylinder_volume, lexhaust_efficiency, ex_breather_magic);

			ldd.time_in_open = lch.time_in_open;
			ldd.time_in_close = lch.time_in_close;
			ldd.time_out_open = lch.time_out_open;
			ldd.time_out_close = lch.time_out_close;
		}
		else
			ldd.Vmin = 1000000000000000.0;

		DynoData rdd = dynodata.clone();
		float rexhaust_efficiency = 1.0;
		float rintake_efficiency = 0.0;
		CylinderHead rch=getRightCylinderHead();

		if (rch)
		{
			rch.updatevariables();
			rch.the_car = the_car;
			rch.fillDynoData(rdd,slotIDOnSlot(R_cylinder_head_slot_ID));
			total_sliction_loss *= rch.getSlictionLoss();
			inertia += rch.getInertia();

			if (piston)
			{
				float TDC_height_offset = ((crank_center_to_cylinder_top-crank.stroke*0.5-connecting_rod.length-piston.length_from_wrist_pin_center_to_crown_base)*0.1);

				float tvm2=bore_area*TDC_height_offset;
				float temp_vmin=(rch.Vmin-piston.crown_volume+tvm2);

				rdd.Vmin = temp_vmin*0.000001;
			}
			else
				rdd.Vmin = 1000000000000000.0;

			if (!im)
				rim = rch.getIntakeManifold();

			ExhaustHeader reh = rch.getExhaustHeader();
			if (reh)
				rexhaust_efficiency = reh.efficiency*rch.exhaust_efficiency_tuning;

			if (im)
				rintake_efficiency = im.efficiency*rch.intake_efficiency_tuning;
			else
			if (rim)
				rintake_efficiency = rim.efficiency*rch.intake_efficiency_tuning;
			else
			{
				rdd.mixture_ratio = 1.0;
				rdd.max_fuel_consumption = 0.0;
				rdd.max_air_consumption = 0.0;
				rdd.mixture_H = 1.0;
				rdd.time_burn = 0.1;
			}

			rdd.in_min = rch.portFlowCoefficient(rch.in_min, rch.intake_valve_diam, rch.intake_tunnel_area, cylinder_volume, rintake_efficiency, in_breather_magic);
			rdd.in_max = rch.portFlowCoefficient(rch.in_max, rch.intake_valve_diam, rch.intake_tunnel_area, cylinder_volume, rintake_efficiency, in_breather_magic);
			rdd.out_min = rch.portFlowCoefficient(rch.out_min, rch.exhaust_valve_diam, rch.exhaust_tunnel_area, cylinder_volume, rexhaust_efficiency, ex_breather_magic);
			rdd.out_max = rch.portFlowCoefficient(rch.out_max, rch.exhaust_valve_diam, rch.exhaust_tunnel_area, cylinder_volume, rexhaust_efficiency, ex_breather_magic);

			rdd.time_in_open = rch.time_in_open;
			rdd.time_in_close = rch.time_in_close;
			rdd.time_out_open = rch.time_out_open;
			rdd.time_out_close = rch.time_out_close;
		}
		else
			rdd.Vmin = 1000000000000000.0;

		if (lch && rch)
			dynodata.beAverageOf(ldd,rdd);

		if (skip_end_of_update)
			skip_end_of_update -= 1;
		else
		{
			if (total_sliction_loss < 0.001)
				total_sliction_loss = 0.001;

			if (total_sliction_loss > 1.0)
				total_sliction_loss = 1.0;

			// lubrication //
			OilPan oilpan = getOilPan();
			if (oilpan)
			{
				oilpan.updatevariables();

				friction_fwd = oilpan.friction_fwd/total_sliction_loss;
				friction_rev = oilpan.friction_rev/total_sliction_loss;
			}
			else
			{
				friction_fwd = 1.0;
				friction_rev = 1.0;
			}

//			System.log(">>"+name+", BLOCK_VEE.JAVA<<:");
//			System.log("  friction_fwd = "+friction_fwd);
//			System.log("  friction_rev = "+friction_rev);
//			System.log("  inertia = "+inertia);
//			System.log("-------------------------------");
		}
	}

	public void check4warnings()
	{
/*		super.check4warnings();
		if (!L_cylinder_head_slot_ID)
			System.log("   L_cylinder_head_slot_ID is 0");
		if (!R_cylinder_head_slot_ID)
			System.log("   R_cylinder_head_slot_ID is 0");
		if (!intake_manifold_slot_ID)
			System.log("   intake_manifold_slot_ID is 0");
*/	}

	public String isDynoable()
	{
		Part p;

		p = getLeftCylinderHead();
		if (!p && L_cylinder_head_slot_ID)
			return "It's missing the left cylinder head.";

		p = getRightCylinderHead();
		if (!p && R_cylinder_head_slot_ID)
			return "It's missing the right cylinder head.";

		p = getIntakeManifold();
		if (!p && intake_manifold_slot_ID)
			return "It's missing the intake.";

		return super.isDynoable();
	}
}
