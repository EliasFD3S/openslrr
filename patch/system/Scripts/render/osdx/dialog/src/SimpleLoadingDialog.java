package java.render.osdx.dialog;

import java.render.*;
import java.util.resource.*;

//csak egy hatterkep, plusz a felirat
class SimpleLoadingDialog extends LoadingDialog
{
	ResourceRef		pic;

	public SimpleLoadingDialog( int myflags, ResourceRef pic )
	{
		super( myflags );
		this.pic = pic;
	}

	public void show()
	{
		show(osd.FADE_DONOTWAIT, 0.25);
	}

	public void show(int fade, float time)
	{
		//visuals
		ResourceRef loadingTexture = pic;
		loadingTexture.cache();

		osd.createBG( loadingTexture );
		bgGroup = osd.endGroup();
		
		super.show(fade,time);
	}
}

