package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Prime_front extends Swaybar
{
	public swaybar_Prime_front( int id )
	{
		super( id );

		force = 42000.0;
		damping = 0.0;

		name_prefix = "Prime DLH 500";
		brand_prestige_factor = 3.25;
		calcStuffs();
	}
}

