package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_MC_GTLE_front extends Swaybar
{
	public swaybar_MC_GTLE_front( int id )
	{
		super( id );

		force = 71300.0;
		damping = 0.0;

		name_prefix = "MC GTLE";
		brand_prestige_factor = 2.65;
		calcStuffs();
	}
}

