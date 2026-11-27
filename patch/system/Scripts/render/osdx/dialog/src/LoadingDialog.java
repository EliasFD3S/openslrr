package java.render.osdx.dialog;

import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.io.*;
import java.util.resource.*;


class LoadingDialog extends Dialog
{
	String			loadingString = "$636|LOADING...";
	String			loadingReady = "$3169|PRESS THROTTLE TO CONTINUE...";

	Text			loadingText;

//	int[]			color = {0xFFFF0000, 0xFFFFFFFF};
//	int				frame;

	int				bgGroup;
	Thread			flashTextThread;

	public LoadingDialog( int myflags )
	{
		super( null, myflags, null, null );
	}

	public void show()
	{
		show(Osd.FADE_DONOTWAIT, 0.25);
	}

	public void show(int fade, float time)
	{
		if (osd)
		{
			//button
			Style buttonStyle = new Style( 2.0, 2.0, Frontend.mediumFont, Text.ALIGN_CENTER, null /*Osd.RRT_TEST*/ );

			osd.createButton( buttonStyle, 0.0, 0.0, " ", 0 );
			osd.createHotkey( Input.AXIS_MENU, Input.VIRTUAL, 0 );
			osd.createHotkey( Input.AXIS_THROTTLE, Input.VIRTUAL, 0 );

			//visuals
			Frontend.mediumFont.cache();

			loadingText = osd.createText( loadingString, Frontend.mediumFont, Text.ALIGN_CENTER, 0.75, 0.91 );
			loadingText.changeColor( Osd.COLOR_UNSELECTED2 );
		}

		super.show(fade,time);
		GfxEngine.forceRendering();

//		enableAnimateHook();
	}

	//kesz a toltes, tegyen ki uzenetet ha akar
	public void waitMode()
	{
		osd.hideGroup( bgGroup );

		loadingText.finalize();

		osd.endGroup();
		loadingText = osd.createText( loadingReady, Frontend.mediumFont, Text.ALIGN_RIGHT, 0.9, 0.91 );

		osd.createRectangle( 1.0, 1.0, 1.2, 0.20, 4/3.0, -0.4, -0.3, 0, new ResourceRef(frontend:0x000Er));

		flashTextThread = new Thread( new FlashText( osd, osd.endGroup() ), "Dialog text flasher" );
		flashTextThread.start();
	}

	public void hide()
	{
		if( flashTextThread )
		{
			flashTextThread.stop();
			flashTextThread = null;
		}

//		disableAnimateHook();
		super.hide();
	}

/*//debug:
	public void animate()
	{
		frame++;
		if (loadingText)
			loadingText.changeColor(color[frame%2]);
	}
//*/
}
