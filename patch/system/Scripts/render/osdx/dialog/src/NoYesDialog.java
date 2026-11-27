package java.render.osdx.dialog;

import java.io.*;


public class NoYesDialog extends TextDialog
{
	final static String	yesno = "$640|YES"+";"+"$641|NO";

	public NoYesDialog( Controller ctrl, int myflags, String mytitle, String mybody )
	{
		super( ctrl, myflags, mytitle, yesno, mybody );
		osd.defSelection=1;
	}
	public NoYesDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect )
	{
		super( ctrl, myflags, mytitle, yesno, mybody, size, aspect );
		osd.defSelection=1;
	}
	public NoYesDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect, float cx, float cy, float refx, float refy )
	{
		super( ctrl, myflags, mytitle, yesno, mybody, size, aspect, cx, cy, refx, refy );
		osd.defSelection=1;
	}
}
