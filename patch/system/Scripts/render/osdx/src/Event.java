package java.render.osdx;

import java.util.*;


public class Event extends Message
{
	public	final static int F_KEY_PRESS	= 0x00000001;
	public	final static int F_KEY_RELEASE	= 0x00000002;

	public	final static int F_GAD_CHANGE	= 0x00000004;	

	public	final static int F_GAD_FOCUS	= 0x00000008;
	public	final static int F_GAD_UNFOCUS	= 0x00000010;

	public	final static int F_GAD_LCLICK	= 0x00000020;
	public	final static int F_GAD_RCLICK	= 0x00000040;

	public	final static int F_GAD_LDOWN	= 0x00000080;
	public	final static int F_GAD_LUP		= 0x00000100;
	public	final static int F_GAD_RDOWN	= 0x00000200;
	public	final static int F_GAD_RUP		= 0x00000400;

	public	final static int F_GAD_DROP		= 0x00000800;

	public	final static int F_GAD_DRAGBEGIN= 0x00001000;
	public	final static int F_GAD_DRAGEND	= 0x00002000;

	public	final static int F_ALL			= 0xFFFFFFFF;

	public	int	flags;

	public	Gadget	gadget;

	public Event( int cmd )
	{
		super( Message.MT_EVENT, cmd );
	}

	public Event( int cmd, int flags )
	{
		super( Message.MT_EVENT, cmd );
		this.flags=flags;
	}
}
