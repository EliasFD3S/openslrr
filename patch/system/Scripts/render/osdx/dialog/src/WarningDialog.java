package java.render.osdx.dialog;

import java.io.*;

public class WarningDialog extends TextDialog
{
	public WarningDialog( Controller ctrl, int myflags, String mytitle, String mybody )
	{
		super( ctrl, myflags, mytitle, "$646|OK", mybody );
	}
	public WarningDialog( Controller ctrl, int myflags, String mytitle, String mybody, float size, float aspect )
	{
		super( ctrl, myflags, mytitle, "$647|OK", mybody, size, aspect );
	}
}

