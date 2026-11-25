package java.game.parts.running_gear.sways;

import java.game.parts.rgearpart.*;


public class swaybar_Baiern_DS_front extends Swaybar
{
	public swaybar_Baiern_DS_front( int id )
	{
		super( id );

		force = 64300.0;
		damping = 0.0;

		name_prefix = "Baiern DevilSport";
		brand_prestige_factor = 1.60;
		calcStuffs();
	}
}

