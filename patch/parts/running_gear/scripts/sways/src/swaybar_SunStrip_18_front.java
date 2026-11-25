package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_SunStrip_18_front extends Swaybar
{
	public swaybar_SunStrip_18_front( int id )
	{
		super( id );

		force = 15900.0;
		damping = 0.0;

		name_prefix = "Duhen SunStrip 1.5 DVC";
		brand_prestige_factor = 1.10;
		calcStuffs();
	}
}
