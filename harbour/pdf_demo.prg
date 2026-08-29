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

function Main()

   local oWnd,oBar
   local oPdf
   local oSayPage
   local oGetFind
   local cFindText := Space( 60 )
   local cPdf := "..\tests\hot corrocion.pdf"   // TEMPORAL: probando seleccion/busqueda multi-pagina -- ver DESIGN.md 70.1

   PdfViewLogReset()   // diagnostico TEMPORAL de BuildComposite(), ver pdf_viewer.prg

   DEFINE WINDOW oWnd TITLE "PDFEngine32 - " + cPdf ;
      FROM 0, 0 TO 700, 1270 PIXEL
		
		DEFINE BUTTONBAR oBar OF oWnd SIZE 45, 24 3D 2007
		
		DEFINE BUTTON OF oBar ;
			 PROMPT "<<" ;
			 ACTION ( oPdf:FirstPage(), oSayPage:Refresh()   )
		DEFINE BUTTON OF oBar ;
			 PROMPT "<" ;
			 ACTION ( oPdf:PrevPage(),  oSayPage:Refresh()   ) 
			 
		DEFINE BUTTON OF oBar ;
			 PROMPT ">" ;
			 ACTION (  oPdf:NextPage(),  oSayPage:Refresh() ) 
		DEFINE BUTTON OF oBar ;
			 PROMPT ">>" ;
			 ACTION ( oPdf:LastPage(),  oSayPage:Refresh()   )
		
		DEFINE BUTTON OF oBar ;
			 PROMPT "Ajustar" ;
			 ACTION ( oPdf:SetZoomFitHeight()  )

		DEFINE BUTTON OF oBar ;
			 PROMPT "100%" ;
			 ACTION ( oPdf:SetZoom100()   )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Ancho" ;
			 ACTION ( oPdf:SetZoomFitWidth()   )

		DEFINE BUTTON OF oBar ;
			 PROMPT "-" ;
			 ACTION ( oPdf:ZoomOut()   )

		DEFINE BUTTON OF oBar ;
			 PROMPT "+" ;
			 ACTION ( oPdf:ZoomIn()   )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Copiar" ;
			 ACTION ( oPdf:CopySelection()   )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Buscar" ;
			 ACTION ( oPdf:Find( AllTrim( cFindText ), .F. ), oSayPage:Refresh() )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Sigue" ;
			 ACTION ( oPdf:FindNext(), oSayPage:Refresh() )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Guardar" ;
			 ACTION ( IIF( MsgYesNo( "Guardar sobrescribe " + oPdf:cFile + ;
			                          Chr(13) + "(el original queda como .bak). Continuar?", ;
			                          "PDFEngine32" ), ;
			               IIF( Pdf_FormSave( oPdf:pDoc, oPdf:cFile ), ;
			                    MsgInfo( "Guardado." ), ;
			                    MsgStop( "No se pudo guardar (sin cambios pendientes, o el documento esta encriptado)." ) ), NIL ) )
	
		DEFINE BUTTON OF oBar ;
			 PROMPT "Imprimir" ;
			 ACTION ( oPdf:PrintDocument() )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Rotar" ;
			 ACTION ( oPdf:RotatePage(), oSayPage:Refresh() )

		DEFINE BUTTON OF oBar ;
			 PROMPT "Resaltar" ;
			 ACTION ( IIF( oPdf:HighlightSelection(), oSayPage:Refresh(), ;
			               MsgStop( "No se pudo resaltar (no hay texto seleccionado, o el documento esta encriptado).", ;
			                        "PDFEngine32" ) ) )

			// Formas libres (Arturo: "formas libres (linea/flecha, rectangulo,
			// circulo, tinta)" -- ver DESIGN.md). Cada boton solo ARMA el modo
			// de dibujo (::oViewer:cDrawMode, ver TPdfViewer) -- el dibujo en si
			// pasa por LButtonDown/MouseMove/LButtonUp de TPdfBitmap
			// (pdf_viewer.prg), que se desarma solo al terminar una forma. Sin
			// tildes, misma convencion de todo este archivo (evita problemas de
			// codificacion entre el editor y el toolchain Harbour/Borland).
			DEFINE BUTTON OF oBar ;
				 PROMPT "Linea" ;
				 ACTION ( oPdf:StartDrawMode( "LINE" ) )

			DEFINE BUTTON OF oBar ;
				 PROMPT "Rectangulo" ;
				 ACTION ( oPdf:StartDrawMode( "RECT" ) )

			DEFINE BUTTON OF oBar ;
				 PROMPT "Circulo" ;
				 ACTION ( oPdf:StartDrawMode( "CIRCLE" ) )

			DEFINE BUTTON OF oBar ;
				 PROMPT "Tinta" ;
				 ACTION ( oPdf:StartDrawMode( "INK" ) )

			// Globo de tip (Arturo: "esquema tipo balloon que permita colocar
			// mensajes tipo tip" -- ver DESIGN.md). Arma el modo "TIP" -- un
			// clic en la pagina (no un arrastre, ver LButtonDown de
			// TPdfBitmap) abre un cuadro de texto ahi mismo para escribir el
			// mensaje.
			DEFINE BUTTON OF oBar ;
				 PROMPT "Tip" ;
				 ACTION ( oPdf:StartDrawMode( "TIP" ) )

			DEFINE BUTTON OF oBar ;
				 PROMPT "Normal" ;
				 ACTION ( oPdf:StopDrawMode() )


      oPdf := TPdfViewer():New( oWnd, ALTO_TOOLBAR, 0, ;
                                 oWnd:nWidth, oWnd:nHeight - ALTO_TOOLBAR )

      // 22 botones x 45px = 990 -- oSayPage/oGetFind corridos a la derecha
      // y ventana ensanchada (mismo ajuste ya hecho varias veces antes):
      // oSayPage_col=990+10, oGetFind_col=oSayPage_col+70,
      // ancho_ventana=oGetFind_col+200.
      oSayPage := TSay():New( 5, 1000, ;
         {|| "" + hb_ntos( oPdf:nCurPage ) + " / " + hb_ntos( oPdf:nPageCount ) }, ;
         oWnd, , , , .F., .F., .T., , , 60, 15 )

      @ 5, 1070 GET oGetFind VAR cFindText PICTURE "@S30" SIZE 150, 20 OF oWnd PIXEL

   ACTIVATE WINDOW oWnd ;
      ON INIT ( IIF( oPdf:Open( cPdf ) , oSayPage:Refresh(), ;
                     MsgStop( "No se pudo abrir " + cPdf, "PDFEngine32" ) ) ) ;
      ON RESIZE ( oPdf:Resize( nWidth, nHeight - ALTO_TOOLBAR ) ) ;
      VALID ( oPdf:Close(), .T. )

return nil
