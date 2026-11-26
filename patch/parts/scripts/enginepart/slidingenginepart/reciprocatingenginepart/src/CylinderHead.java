package java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart;

import java.render.osd.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;
import java.game.parts.*;
import java.game.parts.bodypart.*;
import java.game.parts.enginepart.*;
import java.game.parts.enginepart.slidingenginepart.*;
import java.game.parts.enginepart.airfueldeliverysystem.*;
import java.game.parts.rgearpart.*;

public class CylinderHead extends ReciprocatingEnginePart
{
	// actual //
	float	T_loss = 0.0;
	float	Vmin = 0.0;

	float	in_min = 0.0;
	float	out_min = 0.0;
	float	in_max = 0.0;
	float	out_max = 0.0;

	float	time_in_open = 0.0;
	float	time_in_close = 0.0;
	float	time_out_open = 0.0;
	float	time_out_close = 0.0;

	float	time_spark_min = 0.0;
	float	time_spark_inc = 0.0;
	float	time_spark_RPM0	= 0.0;
	float	time_spark_RPM1	= 0.0;

	float	intake_efficiency_tuning = 1.0;
	float	exhaust_efficiency_tuning = 1.0;

	int	intake_manifold_slot_ID = 0;
	int	exhaust_header_slot_ID = 0;
	int	camshaft_bearing_slot_ID = 0;
	int	cover_slot_ID = 0;

	// misc //
	float	cc_dome_safe_clearance = 0.0; // in milimeters //
	float	intake_tunnel_area = 0.0; // in square centimeters //
	float	exhaust_tunnel_area = 0.0; // in square centimeters //
	float	intake_valve_diam = 0.0; // in milimeters //
	float	number_of_intake_valves_per_cylinder = 0.0;
	float	exhaust_valve_diam = 0.0; // in milimeters //
	float	number_of_exhaust_valves_per_cylinder = 0.0;
	float	bore = 1.0;
	float	no_exhaust_police_fine = 150.0;

	public CylinderHead(){}

	public CylinderHead( int id )
	{
		super( id );

		name = "Cylinder head";

		prestige_calc_weight = 30.0;
	}

	public void updatevariables()
	{
		in_min = 0.0;
		in_max = 0.0;
		out_min = 0.0;
		out_max = 0.0;

		float w = 0.9+0.1*getWear();

		if (!getExhaustHeader())
			police_check_fine_value = no_exhaust_police_fine;
		else
			police_check_fine_value = 0;

		intake_tunnel_area = w*(intake_valve_diam*intake_valve_diam*0.25*0.01*Math.PI*number_of_intake_valves_per_cylinder);
		exhaust_tunnel_area = w*(exhaust_valve_diam*exhaust_valve_diam*0.25*0.01*Math.PI*number_of_exhaust_valves_per_cylinder);
	}

	// #203: opening_volume is the stock disk-area * lift proxy (cm^3).
	// Recover curtain area, limited by the available disk/throat proxy (cm^2).
	// Reference exchange volume in 1 ms uses ideal-air choked capacity at
	// 293.15 K: U*=198.629616711 m/s = 19.8629616711 cm/ms (NASA mflchk).
	// This normalizes the existing pressure-relaxation model, not a full
	// pressure-dependent valve-flow solver. discharge retains the stock
	// empirical breather factor; it is not a measured discharge coefficient.
	public float portFlowCoefficient( float opening_volume, float valve_diameter, float tunnel_area, float cylinder_volume, float efficiency, float discharge )
	{
		if (opening_volume <= 0.0 || valve_diameter <= 0.0 || tunnel_area <= 0.0 || cylinder_volume <= 0.0 || efficiency <= 0.0 || discharge <= 0.0)
			return 0.0;

		float area = 4.0*opening_volume/(valve_diameter*0.1);
		if (area > tunnel_area)
			area = tunnel_area;
		float coefficient = area*19.8629616711*efficiency*discharge/cylinder_volume;
		if (coefficient > 1.0)
			return 1.0;
		return coefficient;
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

	public Part getExhaustHeader()
	{
		if (exhaust_header_slot_ID <= 0)
			return null;

		Part res = partOnSlot(exhaust_header_slot_ID);

		if (res && res instanceof ExhaustHeader)
			return res;
//		else
//			System.log("!!!Exhaust header required on slot!!!");

		return null;
	}

	public Part getCamshaftBearing()
	{
		if (camshaft_bearing_slot_ID <= 0)
			return null;

		Part res = partOnSlot(camshaft_bearing_slot_ID);

		if (res && res instanceof SlidingEnginePart)
			return res;
//		else
//			System.log("!!!Camshaft bearing bridge required on slot!!!");

		return null;
	}

	public Part getCover()
	{
		if (cover_slot_ID <= 0)
			return null;

		Part res = partOnSlot(cover_slot_ID);

		return res;
	}

	public float getInertia()
	{
		return super.getInertia()*2.0; // the valves, springs, ... 2x because it rotates twice faster than the crankshaft //
	}

	public float getSlictionLoss()
	{
		float res = super.getSlictionLoss();
		SlidingEnginePart cam_bearing = getCamshaftBearing();

		if (cam_bearing)
			res *= cam_bearing.getSlictionLoss();

		return res;
	}

	public void fillDynoData( DynoData dd, int parentSlot )
	{
		dd.T_loss = T_loss;
		super.fillDynoData( dd, parentSlot );
	}

	public void check4warnings()
	{
/*		super.check4warnings();
		if (!intake_manifold_slot_ID)
			System.log("   intake_manifold_slot_ID is 0");
		if (!exhaust_header_slot_ID)
			System.log("   exhaust_header_slot_ID is 0");
		if (!camshaft_bearing_slot_ID)
			System.log("   camshaft_bearing_slot_ID is 0");
*/	}

	public String isDynoable()
	{
		Part p;

		p = getIntakeManifold();
		if (!p && intake_manifold_slot_ID)
			return "It's missing the intake.";

		p = getCamshaftBearing();
		if (!p && camshaft_bearing_slot_ID)
			return "It's missing the camshaft bearing bridge.";

		p = getCover();
		if (!p && cover_slot_ID)
			return "It's missing the cylinder head cover.";

		return super.isDynoable();
	}
}
