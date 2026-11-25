package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Nonus_GT2_front extends Swaybar
{
	public swaybar_Nonus_GT2_front( int id )
	{
		super( id );

		force = 22100.0;
		damping = 0.0;

		name_prefix = "Emer Motorsport Nonus GT2";
		brand_prestige_factor = 3.85;
		calcStuffs();
	}
}

