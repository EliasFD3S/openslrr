package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Nonus_SGT_rear extends Swaybar
{
	public swaybar_Nonus_SGT_rear( int id )
	{
		super( id );

		force = 18500.0;
		damping = 0.0;

		name_prefix = "Emer Nonus StreetGT";
		brand_prestige_factor = 1.75;
		calcStuffs();
	}
}

