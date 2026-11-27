package java.render.osdx;

import java.render.*;
import java.io.*;
import java.util.*;
import java.util.resource.*;


public class ScrollGroup extends Group implements Runnable
{
	Dummy		d;
	Viewport	vp;
	Camera		cam;
	Vector3		campos;
	Vector3		destpos, destposX;
	Thread		mover;

	Vector3		limitUL, limitBR;

	public ScrollGroup( Osd o, float x, float y, float w, float h )
	{
		super( o );

		d = new Dummy( WORLDTREEROOT );
		setParent( d );

		vp = new Viewport( osd.getViewport().pri+1, (x+1.0)/2.0, (y+1.0)/2.0, w/2.0, h/2.0 );
		campos = new Vector3(0.0, 0.0, Osd.CAM_OFFSET*vp.getHeight());
		cam = new Camera( d, vp, 2, Osd.CAM_AOV, 0.1, 10.0 );
		cam.setMatrix( campos, null );
		destpos = new Vector3( campos );
		destposX = new Vector3( campos );

		limitUL = convertTextCoordinates( 0.0, 0.0, 0 );
		limitBR = convertTextCoordinates( 0.0, 4.0, 0 );	//1x2 display

		osd.groups.addElement( this );

		activate();
	}

	public void setLimits( float x0, float y0, float x1, float y1 )
	{
		limitUL = convertTextCoordinates( x0, y0, 0 );
		limitBR = convertTextCoordinates( x1, y1, 0 );
	}

	public Vector3 convertTextCoordinates( float x, float y, int pri )
	{
		return new Vector3( x*Osd.SCALE_FS*vp.getAspect()*vp.getWidth(), -y*Osd.SCALE_FS*vp.getHeight(), pri*Osd.LAYER_FACTOR );
		//return osd.convertTextCoordinates( x, y, pri );
	}

	public void activate()
	{
		if( vp )	//superconstr altal +ba hivott activate() hatastalanitasa
			super.activate();
	}

	public Vector3 getScroll()
	{
		return destpos;
	}

	public void setScroll( Vector3 pos )
	{
		destpos = new Vector3(pos);
		chkScroll();
		destposX = new Vector3(destpos);
		campos = new Vector3(destpos);
		cam.setMatrix( campos, null );
	}

	public int vScroll( float p )
	{
		destpos.add( convertTextCoordinates( 0, -p, 0 ) );

		chkScroll();

		notifyAll();

		return 1;
	}

	private void chkScroll()
	{
		if( destpos.y > limitUL.y )
		{
			destpos.y = limitUL.y;
		}
		else
		if( destpos.y < limitBR.y )
		{
			destpos.y = limitBR.y;
		}


		//untested:
		if( destpos.x > limitUL.x )
		{
			destpos.x = limitUL.x;
		}
		else
		if( destpos.x < limitBR.x )
		{
			destpos.x = limitBR.x;
		}

		destpos.z = campos.z;
	}

	public void run()
	{
		while( 1 )
		{
			float l;
			float s;
			Vector3	dlta;
			float i;
			wait();
			do{
				if( new Vector3( destposX ).sub( destpos ).length() )
				{
					s=0.05;
					destposX = new Vector3( destpos );
				}

				dlta = new Vector3( destpos ).sub( campos ).mul( s );

				if( (l = dlta.length()) >= 0.001 )
					s += 0.0005/l;
				campos.add( dlta );
				cam.setMatrix( campos, null );
				Thread.sleep( 20 );
			}while( l > 0.001 );
		}
	}

	//osd show/hide eseten hivodik
	public void show()
	{
		vp.activate( Viewport.RENDERFLAG_CLEARDEPTH | Viewport.RENDERFLAG_CLEARTARGET );

		mover = new Thread(this, "ScrollGroup");
		mover.start();
	}
	public void hide()
	{
		mover.stop();
		mover = null;

		vp.deactivate();
	}

}
