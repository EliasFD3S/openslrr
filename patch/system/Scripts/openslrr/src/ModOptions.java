package java.openslrr;

import java.util.*;

// Registry of the settings a mod shows on the OPTIONS > MODS page (#488).
// A mod registers its rows from modInit(); the value of each row lives in
// ModSettings under the row's key ("<mod>.<name>"), so it is saved with the
// other options and read back by the mod with ModSettings.getInt/getFloat.
// Registering a key that is already registered replaces that row. There is
// no limit on the number of rows; the options screen pages them.
public class ModOptions
{
	public final static int	CHOICE = 0;
	public final static int	TOGGLE = 1;
	public final static int	SLIDER = 2;

	static Vector	rows = new Vector();

	// The row index of a key, or -1.
	public static int find( String key )
	{
		for( int i=0; i<rows.size(); i++ )
			if( ((ModOption)rows.elementAt( i )).key == key )
				return i;
		return -1;
	}

	static ModOption row( String key )
	{
		int i = find( key );
		if( i >= 0 )
			return (ModOption)rows.elementAt( i );
		ModOption o = new ModOption();
		o.key = key;
		rows.addElement( o );
		return o;
	}

	public static void addChoice( String key, String label, String[] choices, int def, String help )
	{
		if( key == null || choices == null || choices.length == 0 )
			return;
		ModOption o = row( key );
		o.kind = CHOICE;
		o.label = label;
		o.help = help;
		o.choices = choices;
		if( def < 0 )
			def = 0;
		if( def >= choices.length )
			def = choices.length - 1;
		o.defInt = def;
	}

	public static void addToggle( String key, String label, int def, String help )
	{
		if( key == null )
			return;
		ModOption o = row( key );
		o.kind = TOGGLE;
		o.label = label;
		o.help = help;
		o.choices = null;
		if( def != 0 )
			def = 1;
		o.defInt = def;
	}

	public static void addSlider( String key, String label, float min, float max, float def, String help )
	{
		if( key == null )
			return;
		ModOption o = row( key );
		o.kind = SLIDER;
		o.label = label;
		o.help = help;
		o.choices = null;
		if( max < min )
		{
			float t = min;
			min = max;
			max = t;
		}
		o.min = min;
		o.max = max;
		if( def < min )
			def = min;
		if( def > max )
			def = max;
		o.defFloat = def;
	}

	public static int count()						{ return rows.size(); }
	public static ModOption get( int i )			{ return (ModOption)rows.elementAt( i ); }

	// Current value of a CHOICE or TOGGLE row, clamped to its range.
	public static int getInt( int i )
	{
		ModOption o = get( i );
		int v = ModSettings.getInt( o.key, o.defInt );
		int top = 1;
		if( o.kind == CHOICE )
			top = o.choices.length - 1;
		if( v < 0 )
			v = 0;
		if( v > top )
			v = top;
		return v;
	}

	// Current value of a SLIDER row, clamped to its range.
	public static float getFloat( int i )
	{
		ModOption o = get( i );
		float v = ModSettings.getFloat( o.key, o.defFloat );
		if( v < o.min )
			v = o.min;
		if( v > o.max )
			v = o.max;
		return v;
	}

	public static void setInt( int i, int v )
	{
		ModSettings.setInt( get( i ).key, v );
	}

	public static void setFloat( int i, float v )
	{
		ModSettings.setFloat( get( i ).key, v );
	}
}
