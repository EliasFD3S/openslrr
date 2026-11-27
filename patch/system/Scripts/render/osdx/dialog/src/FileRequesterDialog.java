package java.render.osdx.dialog;

import java.render.*;
import java.render.osdx.*;
import java.render.osdx.Osd;
import java.util.*;
import java.util.resource.*;
import java.io.*;



public class FileRequesterDialog extends TextDialog
{
	final static int	MAX_ENTRIES = 5;
	final static int	CMD_ENTRIES = 100;

	String		path;
	String		mask;
	Button[]	fileButtons;
	StringInput	nameField;

	public	String		fileName;
	Vector		fileNameList = new Vector();
	int			index;
	int			mode;	//0x00-load 0x01-save 0x02-don't show extension

	public FileRequesterDialog( Controller ctrl, int myflags, String mytitle, String OKButtonText, String path, String mask, int mode, int ffFlags, float cx, float cy, float refx, float refy )
	{
		super( ctrl, myflags, mytitle, OKButtonText + ";"+"$631|CANCEL", null, 0.6, 1.3, cx, cy, refx, refy );

		this.path = path;
		this.mask = mask;
		this.mode = mode;

		FindFile ff = new FindFile();
		String name=ff.first( path+mask, ffFlags );
		while( name )
		{
			if( validator( path + name ) )
			{
				String x = name;
				if( mode&0x02 )
				{
					x=x.append(8);
					x=x.append(8);
					x=x.append(8);
					x=x.append(8);
				}
				fileNameList.addElement( x );
			}

			name = ff.next();
		}
		ff.close();
	}

	public FileRequesterDialog( Controller ctrl, int myflags, String mytitle, String OKButtonText, String path, String mask, int mode, int ffFlags )
	{
		this( ctrl, myflags, mytitle, OKButtonText, path, mask, mode, ffFlags, 0.5, 0.5, -0.5, -0.5 );
	}

	//--------------------------------------
	public int validator( String filename )
	{
		return 1;
	}
	//--------------------------------------


	public void show()
	{
		Style	sty;
		Menu	m;
		int		itembgcolor = 0x40FFFFFF;

//		sty = new Style( 0.50, 0.12, Frontend.mediumFont, Text.ALIGN_CENTER, Dialog.RRT_DIALOGBG );
//		sty = new Style( 0.50, 0.12, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_NONE );
//		sty = new Style( 0.50, 0.10, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef(frontend:0x004Br) );

		sty = new Style( 0.80, 0.1, 4.0/3.0, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_WHITEALPHA, itembgcolor );
		m = osd.createMenu( sty, 0.0, -0.55, 0.11 );	//spacing=0.1 ettol kisse egymasralognak, ami vonalazott eredmenyt ad

		fileButtons = new Button[MAX_ENTRIES];

		for( int i=0; i< MAX_ENTRIES; i++ )
		{
			fileButtons[i] = m.addItem( "", CMD_ENTRIES + i );	//fontos az ures string!
		}

		//filenev input mezo
//		sty = new Style( 0.65, 0.12, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef(frontend:0x004Br) );
//		nameField = osd.createStringInput( sty, 0.10, 0.54, null, 200, "" );
//		sty = new Style( 0.50, 0.10, Frontend.mediumFont, Text.ALIGN_CENTER, new ResourceRef(frontend:0x004Br) );
//		sty = new Style( 0.50, 0.10, 4.0/3.0, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_WHITEALPHA, itembgcolor );
		sty = new Style( 0.80, 0.10, 4.0/3.0, Frontend.mediumFont, Text.ALIGN_CENTER, Osd.RRT_WHITEALPHA, itembgcolor );
		nameField = osd.createStringInput( sty, 0.0, 0.56, null, 200, "" );
		nameField.setLength(0.6);
		osd.createText( "$633|Filename:", Frontend.smallFont, Text.ALIGN_LEFT, -0.70, 0.34 ).changeColor( 0xFFFFae00 );

		//fel scroll gomb
		sty = new Style( 0.10, 0.10, 1.0, Frontend.smallFont, Text.ALIGN_CENTER, new ResourceRef( Osd.RID_ARROWUP ) );
		sty.customHL(new ResourceRef(Osd.RID_ARROWUP_HL));
		osd.createButton( sty, 0.8, -0.62, 201, null );

		//le scroll gomb
		sty = new Style( 0.10, 0.10, 1.0, Frontend.smallFont, Text.ALIGN_CENTER, new ResourceRef( Osd.RID_ARROWDN ) );
		sty.customHL(new ResourceRef(Osd.RID_ARROWDN_HL));
		osd.createButton( sty, 0.8,  0.22, 202, null );

		setButtonNames();

		super.show();

		if( mode&0x01 )
		{
			osd.changeFocusGadget( nameField );
			nameField.click();
		}
	}

	public void setButtonNames()
	{
		for( int i=0; i<MAX_ENTRIES; i++ )
			if( fileNameList.size() > index+i )
				fileButtons[i].changeLabelText( (String)(fileNameList.elementAt(index+i)) );
			else
				fileButtons[i].changeLabelText( null );
	}


	public void	handleEvent( Event e )
	{
		if( e.cmd >= CMD_ENTRIES && e.cmd < CMD_ENTRIES+MAX_ENTRIES )
		{
			e.cmd -= CMD_ENTRIES;
			fileName = fileButtons[e.cmd].getLabelText();
			nameField.setValue( fileName );

			handleEvent( new Event(0) );
		}
		else
		if( e.cmd==200 )
		{
			fileName = ((StringInput)e.gadget).inputString;
			if( !osd.actGadget )		//ha most veszti el a fokuszt
				handleEvent( new Event(0) );	//OK generated
		}
		else
		if( e.cmd==201 )
		{
			if( index )
			{
				index -= MAX_ENTRIES;
				setButtonNames();		
			}
		}
		else
		if( e.cmd==202 )
		{
			if( index+MAX_ENTRIES < fileNameList.size() )
			{
				index += MAX_ENTRIES;
				setButtonNames();		
			}
		}
		else
		{
			if( e.cmd == 0 )	//OK ready?
			{
				if( !fileName || fileName.length() == 0 )
					return;	//no filename - eating up OK

				if( !validator( path + fileName ) )
				{
					if( !(flags & FRF_SAVE) )
					{	//egyeb - csak letezo filenevvel engedjuk ki
						new WarningDialog( controller, DF_HIGHPRI|DF_MODAL|DF_DEFAULTBG, title, "$634|File system error." ).display();
						return;
					}
				}
				else
				{
					if( flags & FRF_SAVE )
					{	//save - feluliras megerositest kerunk
						if( new NoYesDialog( controller, DF_HIGHPRI|DF_MODAL|DF_DEFAULTBG, title, "$635|Overwrite existing file?" ).display() )
							return;	//cancel - eating up OK
					}
				}
			}

			super.handleEvent( e );
		}
	}
}
