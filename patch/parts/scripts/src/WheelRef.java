package java.game.parts;

import java.io.*;
import java.util.*;
import java.util.resource.*;
import java.game.cars.*;

public class WheelRef extends Native
{
//	public native void finalize( );	//ha _ref lesz

	public native Vector3 getPos( );
	public native Ypr getYpr( );
	public native float getDrive( );
	public native float getSteer( );
	public native float getRadius( );

	public native void setPos( Vector3 val );
	public native void setYpr( Ypr val );
	public native void setDrive( float val );
	public native void setSteer( float val );

	public native void setRadius( float val );
	public native void setWidth( float val );
	public native void setCPatch( float halfwidth, float angle, float offset );
	public native void setFriction( float val );
	public native void setFrictn_x( float val );
	public native void setSliction( float val );
	public native void setStiffness( float val );
	public native void setRollRes( float val );
	public native void setBearing( float val );
	public native void setMaxLoad( float val );
	public native void setLoadSmooth( float val );
	public native void setPacejka( int i, float val );

	public native void setForce( float val );
	public native void setDamping( float val );
	public native void setDamping( float bound, float rebound );
	public native void setRestLen( float val );
	public native void setMinLen( float val );
	public native void setMaxLen( float val );

	public native void setInstantCenter( float Hx, float Hy, float Hz, float Lx, float Ly, float Lz );

	public native void setBrake( float val );
	public native void setHBrake( float val );

	public native void setArm( float len, float px, float py, float pz, float nx, float ny, float nz );
	public native void setHub( float len, float p1x, float p1y, float p1z, float p2x, float p2y, float p2z, float pcx, float pcy, float pcz );

	// OpenSLRR: lateral relaxation length of the tyre in metres (Tyre.SetInflation).
	public native void setRelaxation( float val );

	// OpenSLRR (#446): brake hardware for the rotor temperature and pad fade:
	// pad compound (0 street, 1 fast road, 2 race), rotor type (0 solid,
	// 1 grooved, 2 vented, 3 carbon) and rotor diameter in metres.
	public native void setBrakeSpec( float pad, float rotor, float diameter );

	// OpenSLRR (#446): the tyre's compound for its temperature and grip
	// (Tyre.tcSTREET 0, tcSPORT 1, tcSUPERSPORT 2, tcDRAGRADIAL 3).
	public native void setTyreSpec( float compound );

	// OpenSLRR (#469): the share of the axle's longitudinal load transfer the
	// suspension's arms carry, 0 to 1.5: braking (front anti-dive, rear
	// anti-lift) and driving (front anti-lift, rear anti-squat). Without it
	// the host uses typical shares for the suspension type.
	public native void setAnti( float braking, float driving );

	// OpenSLRR (#481): the camber curve about the static ride height: the gain
	// in degrees per 25 mm of bump there (negative: the top leans inboard)
	// and its change per 25 mm of bump (negative: progressive, as short-long
	// arms; positive: fading, as struts). Without it the host uses typical
	// values for the suspension type.
	public native void setCamberGain( float gain, float progression );

	// OpenSLRR (#484): bump steer about the static ride height: degrees of
	// toe-in per 25 mm of bump there (negative: toe-out) and its change per
	// 25 mm of bump. Without it the host uses typical values for the
	// suspension type.
	public native void setBumpSteer( float toe, float progression );

}
