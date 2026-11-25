package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;

public class swaybar_Einvagen_GTA_rear extends Swaybar
{
	public swaybar_Einvagen_GTA_rear( int id )
	{
		super( id );

		force = 13700.0;
		damping = 0.0;

		name_prefix = "Einvagen 140 GTA";
		brand_prestige_factor = 1.20;
		calcStuffs();
	}
}
