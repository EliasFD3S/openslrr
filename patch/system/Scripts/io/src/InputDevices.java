package java.io;

// OpenSLRR (#345): the controllers SDL3 sees, for Options > Controls.
// Index i is the device ControlSet and Input call i + 3. A device that is
// unplugged keeps its index (connected() returns 0).
public class InputDevices
{
	public final static int	FIRST_DEVICE = 3;

	public static native int	refresh();	// asks SDL again; returns count()
	public static native int	count();
	public static native String	name( int i );
	public static native String	type( int i );		// "WHEEL", "PS5 PAD", "JOYSTICK"...
	public static native String	power( int i );		// "WIRED", "BATTERY", "CHARGING", "CHARGED" or ""
	public static native int	connected( int i );
	public static native int	axes( int i );
	public static native int	buttons( int i );
	public static native int	hats( int i );
	public static native int	battery( int i );	// percent, -1 when unknown
	public static native int	canRumble( int i );
	public static native float	activity( int i );	// 0..1, strongest input right now
	public static native int	rumble( int i, float strength, int milliseconds );
	// Force feedback (SDL haptics): a constant force on the steering axis.
	public static native int	canForceFeedback( int i );
	public static native int	testForceFeedback( int i );	// 0.6 s, left then right

	// Axis range calibration: move every axis to both ends, then end it.
	public static native void	calibrateBegin( int i );
	public static native int	calibrateEnd( int i );	// 0 when no axis moved
	public static native void	calibrateReset( int i );
	public static native int	calibrated( int i );
	public static native int	calibrating( int i );

	// Device class of a ControlSet device id (not an index i): each class
	// gets its own filters.
	public final static int	KIND_NONE = -1;
	public final static int	KIND_KEYBOARD = 0;
	public final static int	KIND_MOUSE = 1;
	public final static int	KIND_PAD = 2;
	public final static int	KIND_WHEEL = 3;	// wheel, pedals, shifter, button box...
	public static native int	kind( int device );

	// Rebind listener over every device at once. It waits until the controls
	// settle, ignores what stays held, takes the channel that moves farthest
	// from its rest and waits for its release. The left mouse button cancels.
	public final static int	CAPTURE_IDLE = 0;
	public final static int	CAPTURE_RELEASING = 1;	// "release every control"
	public final static int	CAPTURE_LISTENING = 2;
	public final static int	CAPTURE_HOLDING = 3;		// found, waiting for its release
	public final static int	CAPTURE_DONE = 4;
	public final static int	CAPTURE_CANCELLED = 5;
	public static native void	captureBegin();
	// Only the device kinds in the mask (1 << KIND_*) may be taken (#405);
	// Esc and the left mouse button still clear and cancel.
	public static native void	captureBeginKinds( int mask );
	public static native void	captureCancel();		// back to idle
	public static native int	captureState();
	public static native int	captureDevice();	// ControlSet device id
	public static native int	captureAxis();
	public static native int	captureAnalog();	// 1 for an axis, 0 for a key or button
	public static native int	captureHeld();		// channels ignored because still held
	public static native float	captureRest();		// channel value at rest
	public static native float	capturePeak();		// farthest value from the rest
	public static native float	captureProgress();	// 0..1 while releasing

	// Writes a finished capture into control i of cs. Returns 0 when Esc
	// cleared the binding, 1 when bound, 2 when the other steering half (TURN
	// LEFT <-> TURN RIGHT, same column) was bound to the opposite half too.
	//   key or button:            0..1
	//   axis resting at an end:   rest..far end (a pedal, a trigger)
	//   centred axis:             the half it moved into
	public static int applyCapture( ControlSet cs, int i )
	{
		int device = captureDevice();
		int axis = captureAxis();
		if( device == 0 && axis == Input.RCDIK_ESCAPE )
		{
			cs.change( i, -1, -1 );
			return 0;
		}
		ensureDevice( cs, device );
		cs.change( i, device, axis );
		float rest = captureRest();
		float dir = 1.0;
		if( capturePeak() < rest )
			dir = -1.0;
		if( captureAnalog() == 0 )
		{
			cs.from_min[i] = 0.0;
			cs.from_max[i] = 1.0;
			return 1;
		}
		if( rest > 0.6 || rest < -0.6 )
		{
			cs.from_min[i] = rest;
			cs.from_max[i] = dir;
			return 1;
		}
		cs.from_min[i] = 0.0;
		cs.from_max[i] = dir;
		// One half of a wheel or stick axis is no use without the other: TURN
		// LEFT and TURN RIGHT are next to each other in every column.
		int c = ControlSet.carControl( i );
		int other = -1;
		if( c == 2 )
			other = i + 1;
		else
		if( c == 3 )
			other = i - 1;
		// Device 2 splits each stick into two positive channels already.
		if( other < 0 || device < FIRST_DEVICE )
			return 1;
		cs.change( other, device, axis );
		cs.from_min[other] = 0.0;
		cs.from_max[other] = -dir;
		return 2;
	}

	// ControlSet saves the device names it saw when it was made; a device
	// plugged in later must be added or its bindings are lost on save.
	public static void ensureDevice( ControlSet cs, int device )
	{
		if( device < cs.nDevices )
			return;
		String[] names = new String[device + 1];
		for( int j = 0; j <= device; j++ )
		{
			if( j < cs.nDevices )
				names[j] = cs.deviceName[j];
			else
				names[j] = Input.getDeviceName( j );
		}
		cs.deviceName = names;
		cs.nDevices = device + 1;
	}

	// The device kinds of a ControlSet.CLASS_* for captureBeginKinds.
	public static int classKinds( int deviceClass )
	{
		if( deviceClass == ControlSet.CLASS_KEYBOARD )
			return 3;	// 1 << KIND_KEYBOARD | 1 << KIND_MOUSE
		if( deviceClass == ControlSet.CLASS_PAD )
			return 4;	// 1 << KIND_PAD
		if( deviceClass == ControlSet.CLASS_WHEEL )
			return 8;	// 1 << KIND_WHEEL
		return -1;
	}

	// Connected devices of a ControlSet.CLASS_* (keyboard and mouse: 1).
	public static int classCount( int deviceClass )
	{
		if( deviceClass == ControlSet.CLASS_KEYBOARD )
			return 1;
		int n = 0;
		for( int i = 0; i < count(); i++ )
			if( connected( i ) && ControlSet.deviceClass( i + FIRST_DEVICE ) == deviceClass )
				n++;
		return n;
	}

	// The first connected device with force feedback, -1 none.
	public static int forceFeedbackIndex()
	{
		for( int i = 0; i < count(); i++ )
			if( connected( i ) && canForceFeedback( i ) )
				return i;
		return -1;
	}

	public static String kindName( int device )
	{
		int k = kind( device );
		if( k == KIND_KEYBOARD )
			return "KEYBOARD";
		if( k == KIND_MOUSE )
			return "MOUSE";
		if( k == KIND_PAD )
			return "PAD";
		if( k == KIND_WHEEL )
			return "WHEEL / SIM";
		return "";
	}
}
