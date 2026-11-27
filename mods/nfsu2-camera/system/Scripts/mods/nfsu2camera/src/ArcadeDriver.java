package java.mods.nfsu2camera;

import java.lang.*;

// Runs the ARCADE camera once per frame (animate callback) and receives its
// optional NEXT ARCADE VIEW key (a global hotkey calls osdCommand).
public class ArcadeDriver extends GameType
{
	ArcadeCamera owner;

	public ArcadeDriver( ArcadeCamera o )
	{
		createNativeInstance();
		owner = o;
	}

	public void animate()
	{
		owner.update();
	}

	public void osdCommand( int cmd )
	{
		if( cmd == ArcadeCamera.CMD_NEXTVIEW )
			owner.nextView();
	}
}
