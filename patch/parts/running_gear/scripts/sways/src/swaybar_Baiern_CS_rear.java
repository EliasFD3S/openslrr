package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;

public class swaybar_Baiern_CS_rear extends Swaybar
{
	public swaybar_Baiern_CS_rear( int id )
	{
		super( id );

		force = 8300.0;
		damping = 0.0;

		name_prefix = "Baiern CoupeSport 2.5";
		brand_prestige_factor = 1.20;
		calcStuffs();
	}
}

