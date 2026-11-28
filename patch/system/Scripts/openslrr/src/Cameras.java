package java.openslrr;

// Registry of mod driving cameras (#487). The selection (-1 = the classic
// cameras) and each provider's current view are kept here for the session;
// the mod settings (#488) persist them by name ("camera.provider",
// "camera.<name>.view").
public class Cameras
{
	static CameraProvider[]	list;
	static int[]			views;
	static int				n;
	static int				selection;	// 0: classic, else index + 1

	// Registering the same provider again does nothing.
	public static void register( CameraProvider p )
	{
		if( !p )
			return;
		for( int i = 0; i < n; i++ )
			if( list[i] == p )
				return;
		if( !list || n >= list.length )
		{
			CameraProvider[] bigger = new CameraProvider[n * 2 + 4];
			int[] biggerViews = new int[n * 2 + 4];
			for( int j = 0; j < n; j++ )
			{
				bigger[j] = list[j];
				biggerViews[j] = views[j];
			}
			list = bigger;
			views = biggerViews;
		}
		list[n] = p;
		views[n] = 0;
		n++;
	}

	public static int count()
	{
		return n;
	}

	public static CameraProvider get( int i )
	{
		if( i < 0 || i >= n )
			return null;
		return list[i];
	}

	// The selected provider's index, or -1 for the classic cameras.
	public static int selected()
	{
		return selection - 1;
	}

	// An index out of range selects the classic cameras.
	public static void select( int i )
	{
		if( i < 0 || i >= n )
			selection = 0;
		else
			selection = i + 1;
	}

	// The provider's current view, within 0..views()-1.
	public static int view( CameraProvider p )
	{
		int i = indexOf( p );
		if( i < 0 )
			return 0;
		int v = views[i];
		if( v < 0 || v >= p.views() )
			v = 0;
		return v;
	}

	public static void setView( CameraProvider p, int v )
	{
		int i = indexOf( p );
		if( i >= 0 )
			views[i] = v;
	}

	static int indexOf( CameraProvider p )
	{
		for( int i = 0; i < n; i++ )
			if( list[i] == p )
				return i;
		return -1;
	}
}
