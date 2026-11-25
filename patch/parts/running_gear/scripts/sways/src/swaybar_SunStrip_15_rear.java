package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_SunStrip_15_rear extends Swaybar
{
	public swaybar_SunStrip_15_rear( int id )
	{
		super( id );

		force = 3400.0;
		damping = 0.0;

		name_prefix = "Duhen SunStrip 1.5 DVC";
		brand_prestige_factor = 1.00;
		calcStuffs();
	}
}
