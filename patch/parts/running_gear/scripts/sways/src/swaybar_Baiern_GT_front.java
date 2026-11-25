package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Baiern_GT_front extends Swaybar
{
	public swaybar_Baiern_GT_front( int id )
	{
		super( id );

		force = 64300.0;
		damping = 0.0;

		name_prefix = "Baiern CoupeSport GT III";
		brand_prestige_factor = 4.00;
		calcStuffs();
	}
}

