package java.render.osdx;

import java.render.*;
import java.util.resource.*;

public class Button extends Gadget
{
	public Button( Osd o, Style sty, float x, float y, int pri, String string, int command )
	{
		// -0.007 -es offset egy patch mert a szovegek lenn voltak a gombokon
		//nem jo, pl elrontja a control options-t!
		super( o, sty, x, y  /*- 0.007*/, pri, string, command );
	}

	public void click()
	{
		sendEvent(Event.F_GAD_LCLICK);
	}
}
