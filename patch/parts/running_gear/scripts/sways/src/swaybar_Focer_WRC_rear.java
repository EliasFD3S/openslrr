package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Focer_WRC_rear extends Swaybar
{
	public swaybar_Focer_WRC_rear( int id )
	{
		super( id );

		force = 15300.0;
		damping = 0.0;

		name_prefix = "Shimutshibu Focer WRC";
		brand_prestige_factor = 3.50;
		calcStuffs();
	}
}

