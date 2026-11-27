package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_SuperDuty_500_front extends Swaybar
{
	public swaybar_SuperDuty_500_front( int id )
	{
		super( id );

		force = 55000.0;
		damping = 0.0;

		name_prefix = "Hauler's SuperDuty 500";
		brand_prestige_factor = 1.55;
		calcStuffs();
	}
}

