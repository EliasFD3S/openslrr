package java.openslrr;

import java.lang.*;
import java.io.*;

// OpenSLRR (#488): key bindings for script mods. A mod registers its action
// once (from modInit) and listens to the virtual axis it gets back like a
// stock one:
//
//	int axis = ModKeys.register( "nfsu2.nextview", "NEXT ARCADE VIEW", Input.RCDIK_G );
//	if( axis >= 0 )
//		osd.createHotkey( axis, Input.VIRTUAL|Osd.HK_STATIC, CMD_NEXT, this );
//
// Each key lives in one of the 16 reserved ControlSet slots (MOD_FIRST..
// NCONTROLS-1), so Options > Controls > Game controls can rebind it and the
// controls file saves it. The controls file records which key id owns each
// slot (ControlSet.save/load), so a binding follows its id across boots and
// whatever order the mods register in. The binding is live in the player's
// controller after the next controller reset, which every section entry
// does; the slots are in the DEFAULTSET group, so they act in driving,
// garage and menus, not inside modal dialogs.
public class ModKeys
{
	public final static int	MAX = 16;				// ControlSet.MOD_SLOTS
	// Virtual axis of key i: AXIS_BASE + i, far above the stock Input.AXIS_*.
	public final static int	AXIS_BASE = 0x4D4B0000;

	static int			n;
	static String[]		ids;
	static String[]		labels;
	static int[]		keys;
	static int[]		slots;

	static void init()
	{
		if( ids == null )
		{
			ids = new String[MAX];
			labels = new String[MAX];
			keys = new int[MAX];
			slots = new int[MAX];
		}
	}

	// Registers a key: id is stable and unique, by convention
	// "<mod>.<name>"; label is the row text in the options; defaultKey is an
	// Input.RCDIK_* keyboard scan code bound while the key's slot is unbound
	// in the controls file (<= 0: none). Returns the virtual axis id, the same
	// one again for an id already registered, or -1 for an empty id or when
	// all MAX keys are taken (nothing is added then).
	public static int register( String id, String label, int defaultKey )
	{
		init();
		int empty = 1;
		if( id != null )
		{
			if( id.length() > 0 )
				empty = 0;
		}
		if( empty )
		{
			System.log( "[modkeys] register: a key needs an id; ignored" );
			return -1;
		}
		int i = find( id );
		if( i >= 0 )
			return AXIS_BASE + i;
		if( n >= MAX )
		{
			System.log( "[modkeys] " + id + ": all " + MAX + " mod key slots are taken; not registered" );
			return -1;
		}
		ids[n] = id;
		if( label == null )
			labels[n] = id;
		else
			labels[n] = label;
		keys[n] = defaultKey;
		// Until a ControlSet resolves the slots against a controls file: the
		// slot a set without a file gives it.
		slots[n] = ControlSet.MOD_FIRST + n;
		n++;
		System.log( "[modkeys] " + id + ": axis " + (AXIS_BASE + n - 1) + ", default key " + defaultKey );
		return AXIS_BASE + n - 1;
	}

	// Registered keys, in registration order (at most MAX).
	public static int count()
	{
		return n;
	}

	// Index of a registered id, -1 when none.
	public static int find( String id )
	{
		if( id == null )
			return -1;
		init();
		for( int i = 0; i < n; i++ )
		{
			if( ids[i] == id )
				return i;
		}
		return -1;
	}

	public static String id( int i )
	{
		if( i < 0 || i >= n )
			return null;
		return ids[i];
	}

	public static String label( int i )
	{
		if( i < 0 || i >= n )
			return null;
		return labels[i];
	}

	public static int defaultKey( int i )
	{
		if( i < 0 || i >= n )
			return -1;
		return keys[i];
	}

	// The virtual axis of key i, -1 when there is no such key.
	public static int axis( int i )
	{
		if( i < 0 || i >= n )
			return -1;
		return AXIS_BASE + i;
	}

	// The ControlSet slot of key i as the last ControlSet that loaded or
	// built its defaults resolved it; -1 when there is no such key. A screen
	// that holds its own ControlSet can ask it: cs.modSlot( i ).
	public static int control( int i )
	{
		if( i < 0 || i >= n )
			return -1;
		return slots[i];
	}

	// Called by ControlSet when it resolves the mod slots.
	public static void setControl( int i, int slot )
	{
		if( i >= 0 && i < n )
			slots[i] = slot;
	}
}