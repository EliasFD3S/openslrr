package java.render.osdx.dialog;

import java.io.*;


public class OKCancelDialog extends TextDialog
{
	public	final static String STR_OK = "$642|OK";
	public	final static String STR_CANCEL = "$643|Cancel";

	public	final static String okcancel = STR_OK+";"+STR_CANCEL;

	public OKCancelDialog( Controller ctrl, int myflags, String mytitle, String mybody )
	{
		super( ctrl, myflags, mytitle, okcancel, mybody );
	}
	public OKCancelDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect )
	{
		super( ctrl, myflags, mytitle, okcancel, mybody, size, aspect );
	}
	public OKCancelDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect, float cx, float cy, float refx, float refy )
	{
		super( ctrl, myflags, mytitle, okcancel, mybody, size, aspect, cx, cy, refx, refy );
	}
}
