// pdf_demo.prg
//
// ============================================================================
// Demo de TPdfViewer (ver pdf_viewer.prg) -- ventana con barra de
// navegacion de paginas y dos modos de zoom (ajuste al alto / 100%).
//
// El layout es: una franja de botones fija arriba (ALTO_TOOLBAR pixels) y el
// area de despliegue del PDF ocupando el resto de la ventana, wireado a
// ON RESIZE para que se reacomode si el usuario cambia el tamanio de la
// ventana (y, si el modo activo es "ajuste al alto", para recalcular la
// escala).
// ============================================================================

#include "FiveWin.ch"

#define ALTO_TOOLBAR   26

static oWnd 
static oPdf,oSayPage 
static obar,npage,opage,nPageFin,oPageFin,ofont,cPdf

function Main()
	local oIco
	Local nrow,ncol,wsearch:=space(50),osearch
	
	DEFINE FONT oFont NAME "MS Sans Serif" SIZE 0, -9 
	DEFINE ICON oIco RESOURCE "LOGO"
	 
	PdfViewLogReset()   // diagnostico de BuildComposite(), opt-in via PDFVIEW_DEBUG_LOG -- ver pdf_viewer.prg
	nPage:=0
	nPageFin:=0
	DEFINE WINDOW oWnd TITLE "PDFEngine32 - "   ;
		FROM 0, 0 TO 700, 1270 PIXEL;
			MENU BuildMenu() ;
			ICON oIco
		
		DEFINE BUTTONBAR oBar OF oWnd SIZE 30, 30 3D 2007
			DEFINE BUTTON OF obar;
				RESOURCE "ICON_16_OPEN";
				ACTION (cargapdf()); //,Npage:=1,nPageFin:=oPdf:nPageCount,Opage:refresh(),OpageFin:refresh());
				NOBORDER //;

		DEFINE BUTTON OF oBar ;
			RESOURCE "ICON_16_SAVE";
			ACTION ( IIF( MsgYesNo( "Guardar sobrescribe " + oPdf:cFile + ;
			                          Chr(13) + "(el original queda como .bak). Continuar?", ;
			                          "PDFEngine32" ), ;
			               IIF( Pdf_FormSave( oPdf:pDoc, oPdf:cFile ), ;
			                    MsgInfo( "Guardado." ), ;
			                    MsgStop( "No se pudo guardar (sin cambios pendientes, o el documento esta encriptado)." ) ), NIL ) )
		
		DEFINE BUTTON OF oBar ;
			RESOURCE "ICON_16_PRINT";
			ACTION ( oPdf:PrintDocument() )
	    
		nrow := Obar:nTop +.5
		ncol := oBar:nLeft + 15
		
		@ nRow, nCol SAY "P�gina:" OF oBar SIZE 35,20 FONT oFont
 
		@ nRow, nCol+1.5 GET opage VAR nPage OF obar PICTURE "9999" SIZE 35,20 FONT oFont;
						 VALID (Getpagina(@npage,oPage,@nPageFin,oPdf),opdf:refresh(),.t.)

		@ nRow, nCol+13 SAY "/" OF oBar SIZE 5,20 FONT oFont

		@ nRow, nCol+15.5 SAY opagefin VAR nPagefin OF obar SIZE 35,20 FONT oFont

		DEFINE BUTTON OF oBar GROUP ;
			RESOURCE "A_04"; 
			ACTION ( oPdf:FirstPage(), ( nPage:=oPdf:nCurPage, oPage:Refresh() )   )
		
		DEFINE BUTTON OF oBar ;
			RESOURCE "A_02";
			ACTION ( oPdf:PrevPage(),  ( nPage:=oPdf:nCurPage, oPage:Refresh() )   ) 
			 
		DEFINE BUTTON OF oBar ;
			RESOURCE "A_03";
			ACTION (  oPdf:NextPage(),  ( nPage:=oPdf:nCurPage, oPage:Refresh() ) ) 
		DEFINE BUTTON OF oBar ;
			RESOURCE "A_05";
			ACTION ( oPdf:LastPage(), ( nPage:=oPdf:nCurPage, oPage:Refresh() )   )
		
		DEFINE BUTTON OF oBar GROUP;
			RESOURCE "W2";
			ACTION ( oPdf:SetZoomFitHeight()  )

		DEFINE BUTTON OF oBar ;
			RESOURCE "Z7" ;
			ACTION ( oPdf:SetZoom100()   )

		DEFINE BUTTON OF oBar ;
			RESOURCE "W1" ;
			ACTION ( oPdf:SetZoomFitWidth()   )

		DEFINE BUTTON OF oBar GROUP;
			RESOURCE "ICON_16_ZOOOUT" ;
			ACTION ( oPdf:ZoomOut()   )

		DEFINE BUTTON OF oBar ;
			 RESOURCE "ICON_16_ZOOIN";
			 ACTION ( oPdf:ZoomIn()   )
		

		DEFINE BUTTON OF oBar group ;
			RESOURCE "Z1" ;
			ACTION ( oPdf:RotatePage( -90 ), ( nPage:=oPdf:nCurPage, oPage:Refresh() ) )

		DEFINE BUTTON OF oBar ;
			RESOURCE "Z2";
			ACTION ( oPdf:RotatePage( 90 ), ( nPage:=oPdf:nCurPage, oPage:Refresh() ) )
  
		@ nRow, nCol+80 SAY "Buscar:" OF oBar SIZE 35,20 FONT oFont
	 	@ nRow, nCol+62 GET oSearch VAR wSearch OF obar  SIZE 80,20 FONT oFont

		DEFINE BUTTON OF oBar ;
			RESOURCE "Z5" ;
			ACTION ( oPdf:SearchPrev( AllTrim( wSearch ), .F. ), ( nPage:=oPdf:nCurPage, oPage:Refresh() ) )

		DEFINE BUTTON OF oBar ;
			RESOURCE "Z6" ;
			ACTION ( oPdf:SearchNext( AllTrim( wSearch ), .F. ), ( nPage:=oPdf:nCurPage, oPage:Refresh() ) )

		DEFINE BUTTON OF oBar GROUP;
			RESOURCE "ICON_16_COPY";
			ACTION ( oPdf:CopySelection()   )



	
		DEFINE BUTTON OF oBar ;
			RESOURCE "Resaltar" ;
			ACTION ( IIF( oPdf:HighlightSelection(), ( nPage:=oPdf:nCurPage, oPage:Refresh() ), ;
			               MsgStop( "No se pudo resaltar (no hay texto seleccionado, o el documento esta encriptado).", ;
			                        "PDFEngine32" ) ) )


			DEFINE BUTTON OF oBar ;
				 RESOURCE "Linea" ;
				 ACTION ( oPdf:StartDrawMode( "LINE" ) )

			DEFINE BUTTON OF oBar ;
				 RESOURCE "Rectangulo" ;
				 ACTION ( oPdf:StartDrawMode( "RECT" ) )

			DEFINE BUTTON OF oBar ;
				 RESOURCE "Circulo" ;
				 ACTION ( oPdf:StartDrawMode( "CIRCLE" ) )

			DEFINE BUTTON OF oBar ;
				 RESOURCE "Tinta" ;
				 ACTION ( oPdf:StartDrawMode( "INK" ) )

			DEFINE BUTTON OF oBar ;
				 RESOURCE "Tip" ;
				 ACTION ( oPdf:StartDrawMode( "TIP" ) )

			DEFINE BUTTON OF oBar ;
				 PROMPT "Normal" ;
				 ACTION ( oPdf:StopDrawMode() )


      oPdf := TPdfViewer():New( oWnd, ALTO_TOOLBAR, 0, ;
                                 oWnd:nWidth, oWnd:nHeight - ALTO_TOOLBAR )

      oPdf:bOnPageChange := {| n | ( nPage := n, oPage:Refresh() ) }


   ACTIVATE WINDOW oWnd MAXIMIZED;
      ON RESIZE ( oPdf:Resize( nWidth, nHeight - ALTO_TOOLBAR ) ) ;
      VALID ( oPdf:Close(), .T. )
	  
return nil
//----------------------------------------------------------------------------//
Static Function BuildMenu()

   Local oMenu

   MENU oMenu
      MENUITEM "&Abrir" ACTION ( cargapdf() )
      MENUITEM "&Salir" ACTION (ownd:End())
   ENDMENU

Return oMenu
//--------------------------------------//
Function CargaPdf(nfile) 
    
	if empty(nfile)
		cPdf=cGetFile( "*.pdf", "Favor selecione un pdf file" )
		if empty(cPdf)
			Return .f.
		endif
	endif
	if !File(cPdf)
		Return .f.
	endif
	if !empty(oPdf)
		oPdf:Close()
	endif
	oPdf:Open( cPdf )
	oPdf:SetZoomFitHeight() 
	//opdf:refresh()
	Npage:=1
	nPageFin:=Opdf:nPageCount
	Opage:refresh()
	OpageFin:refresh()
return .t.
//----------------------------------------------------------------------------//
Function Getpagina(npage,oPage,nPageFin,oPdf)

   iF nPage > nPageFin
      npage:=npagefin
   endif
   iF nPage < 1
      npage:=1
   endif
   opdf:gotopage(npage)
   opage:refresh()

return .t.
