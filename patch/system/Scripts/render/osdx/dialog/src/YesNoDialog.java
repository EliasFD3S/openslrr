package java.render.osdx.dialog;

import java.io.*;


public class YesNoDialog extends TextDialog
{
	public YesNoDialog( Controller ctrl, int myflags, String mytitle, String mybody )
	{
		super( ctrl, myflags, mytitle, "$648|YES"+";"+"$649|NO", mybody );
		escapeCmd = 1;	//NO
	}

	public YesNoDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect )
	{
		super( ctrl, myflags, mytitle, "$650|YES"+";"+"$651|NO", mybody, size, aspect );
		escapeCmd = 1;	//NO
	}
}
