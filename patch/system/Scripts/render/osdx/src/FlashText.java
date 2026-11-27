package java.render.osdx;

import java.game.*;	//khm.


public class FlashText implements Runnable
{
	Osd		osd;
	int		group;

	public FlashText( Osd o, int g )
	{
		osd = o;
		group = g;
	}

	public void run()
	{
		while(1)
		{
			//kameravaltas (vagy akarmi mas)
			//??mi ez a nagy patch??szebben nem lehetett volna??
			GameLogic.actualState.refresh( 0 );

			//szoveg villogtatasa
			osd.showGroup( group );
			Thread.sleep( 600 );

			osd.hideGroup( group );
			Thread.sleep( 600 );
		}
	}
}
