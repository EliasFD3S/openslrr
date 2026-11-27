package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_SuperDuty_750_front extends Swaybar
{
	public swaybar_SuperDuty_750_front( int id )
	{
		super( id );

		force = 107900.0;
		damping = 0.0;

		name_prefix = "Hauler's SuperDuty Extra 750";
		brand_prestige_factor = 1.75;
		calcStuffs();
	}
}

