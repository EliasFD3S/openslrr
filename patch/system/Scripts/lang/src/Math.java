import system:*;

public class Math
{
	public native static float random();
	public native static float sqrt(float  a);
	// OpenSLRR (#450): a to the power b, as in SLRR 2.3.1 (0 when undefined).
	public native static float pow(float  a, float  b);
	public native static void randomize();
	public native static void setrandseed( int seed );
}