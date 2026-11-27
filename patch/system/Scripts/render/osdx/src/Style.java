package java.render.osdx;

import java.render.*;
import java.util.resource.*;

public class Style
{
	float		width, height, aspect;
	ResourceRef charset;
	int			align;	//hova kerul a gadget a megadott poziciooz kepest
	int			la1;	//hova kerul a label 'box' a gombhoz viszonyitva (ra, balra, jobbra)
	int			la2;	//hova igazitodik a felirat a label boxon belul
	float		labelwidth;
	ResourceRef background, disabled, highlight;
	int			usecolor;
	int			color;

	float		rWidth, rHeight;
	RectangleTemplate	rt;

	public Style( float w, float h, ResourceRef c, int a, ResourceRef b )
	{
		this( w, h, 4.0/3.0, c, a, b );
	}

	//alapvetoen bugos, mivel az Width, rHeight ertekeket meg a viewport szelessegevel is szorozni kellene
	//de a Style alapvetoen vp fuggetlen dolog!
	public Style( float w, float h, float as, ResourceRef c, int a, ResourceRef b )
	{
		width = w;
		height = h;
		charset = c;
		align = a;

		labelwidth = width;
		la1 = Text.ALIGN_CENTER;
		la2 = Text.ALIGN_CENTER;

		background = b;
		aspect=as;

		rWidth = width*Osd.SCALE_FS*aspect;
		rHeight = height*Osd.SCALE_FS;

		width/=aspect;

		if( b )
			rt = new RectangleTemplate( rWidth, rHeight, background );
	}

	//alapvetoen bugos, mivel az Width, rHeight ertekeket meg a viewport szelessegevel is szorozni kellene
	//de a Style alapvetoen vp fuggetlen dolog!
	// szinezheto rectangleje lesz
	public Style( float w, float h, float as, ResourceRef c, int a, ResourceRef b, int color )
	{
		width = w;
		height = h;
		charset = c;
		align = a;

		labelwidth = width;

		la1 = Text.ALIGN_CENTER;
		la2 = Text.ALIGN_CENTER;

		background = b;
		aspect=as;

		rWidth = width*Osd.SCALE_FS*aspect;
		rHeight = height*Osd.SCALE_FS;

		width/=aspect;

		if( b )
		{
			rt = new RectangleTemplate( rWidth, rHeight, background, color );
			usecolor=1;
			this.color=color;
		}
	}

	public Style setLabelAlign( int a1, int a2 ){ la1=a1; la2=a2; return this; }
	public Style customHL( ResourceRef hlTex ) { highlight = hlTex; return this; }
	public Style setLabelWidth( float lw ) { labelwidth = lw; return this; }
	public Style setDisabled( ResourceRef d ){ disabled = d; return this; }
}
