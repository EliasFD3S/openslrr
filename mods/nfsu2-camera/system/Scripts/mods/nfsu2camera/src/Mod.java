package java.mods.nfsu2camera;

import java.lang.*;
import java.openslrr.*;

// NFSU2 camera mod (#470): entry point. Registers the ARCADE camera
// provider, its settings (Options > Mods) and an optional key.
public class Mod
{
	public static void modInit()
	{
		if( OpenSLRR.apiVersion() < 1 )
		{
			System.log( "[nfsu2camera] needs mod API 1; not installed" );
			return;
		}

		String[] views = new String[ArcadeMath.VIEWS];
		for( int i = 0; i < ArcadeMath.VIEWS; i++ )
			views[i] = ArcadeMath.viewName( i );
		// The same key as Cameras' saved view, so the option and the V cycle
		// always show and change one value.
		ModOptions.addChoice( ArcadeCamera.KEY_VIEW, "ARCADE VIEW", views, ArcadeMath.FAR,
			"The view of the ARCADE camera; V cycles the views while driving." );
		ModOptions.addSlider( ArcadeCamera.KEY_SHAKE, "ARCADE SHAKE (%)", 0.0, 200.0, 100.0,
			"Strength of the ARCADE camera's shakes: road vibration, nitrous and crashes. 0 turns them off." );

		// Cameras.register reads the saved view with a default of 0 (BUMPER):
		// a new profile starts in FAR.
		if( ModSettings.getInt( ArcadeCamera.KEY_VIEW, -1 ) == -1 )
			ModSettings.setInt( ArcadeCamera.KEY_VIEW, ArcadeMath.FAR );

		ArcadeCamera.nextAxis = ModKeys.register( "nfsu2camera.nextview", "NEXT ARCADE VIEW", 0 );

		Cameras.register( ArcadeCamera.instance() );
		System.log( "[nfsu2camera] registered " + ArcadeCamera.NAME + ", next view axis " + ArcadeCamera.nextAxis );
	}
}
