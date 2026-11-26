import system:*;

public class Math
{
	// OpenSLRR (#463): constants, so scripts stop writing pi by hand.
	public final static float PI      = 3.14159265;
	public final static float HALF_PI = 1.57079633;
	public final static float TWO_PI  = 6.28318531;
	public final static float DEG2RAD = 0.0174532925;
	public final static float RAD2DEG = 57.2957795;
	public final static float E       = 2.71828183;

	public native static float random();
	public native static float sqrt(float  a);
	// OpenSLRR (#450): a to the power b, as in SLRR 2.3.1 (0 when undefined).
	public native static float pow(float  a, float  b);
	public native static void randomize();
	public native static void setrandseed( int seed );

	// OpenSLRR (#463): host natives, in radians. Like pow, they return 0
	// rather than a NaN or an infinity; asin and acos clamp their input
	// to [-1, 1].
	public native static float sin(float  a);
	public native static float cos(float  a);
	public native static float tan(float  a);
	public native static float asin(float  a);
	public native static float acos(float  a);
	public native static float atan(float  a);
	public native static float atan2(float  y, float  x);
	public native static float exp(float  a);
	// Natural logarithm; 0 for a <= 0.
	public native static float log(float  a);
	// The same angle in [-PI, PI].
	public native static float wrapAngle(float  a);

	// OpenSLRR (#463): helpers written in script.
	public static float abs(float a)
	{
		if (a < 0.0)
			return -a;
		return a;
	}

	public static float sign(float a)
	{
		if (a > 0.0)
			return 1.0;
		if (a < 0.0)
			return -1.0;
		return 0.0;
	}

	public static float min(float a, float b)
	{
		if (a < b)
			return a;
		return b;
	}

	public static float max(float a, float b)
	{
		if (a > b)
			return a;
		return b;
	}

	public static float clamp(float v, float lo, float hi)
	{
		if (v < lo)
			return lo;
		if (v > hi)
			return hi;
		return v;
	}

	// a when t is 0, b when t is 1 (t is not clamped).
	public static float lerp(float a, float b, float t)
	{
		return a+(b-a)*t;
	}

	// A random number in [lo, hi).
	public static float randomRange(float lo, float hi)
	{
		return lo+(hi-lo)*random();
	}

	// A random index in [0, n), 0 when n <= 0.
	public static int randomInt(int n)
	{
		if (n <= 0)
			return 0;
		int r = (int)(random()*n);
		if (r >= n)
			r = n-1;
		return r;
	}
}