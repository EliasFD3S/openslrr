package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Ishima_WRS_rear extends Swaybar
{
	public swaybar_Ishima_WRS_rear( int id )
	{
		super( id );

		force = 10900.0;
		damping = 0.0;

		name_prefix = "Duhen SunStrip 1.5 DVC";
		brand_prestige_factor = 3.30;
		calcStuffs();
	}
}

