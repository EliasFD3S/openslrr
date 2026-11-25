package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Focer_WRC_front extends Swaybar
{
	public swaybar_Focer_WRC_front( int id )
	{
		super( id );

		force = 36500.0;
		damping = 0.0;

		name_prefix = "Shimutshibu Focer WRC";
		brand_prestige_factor = 3.50;
		calcStuffs();
	}
}

