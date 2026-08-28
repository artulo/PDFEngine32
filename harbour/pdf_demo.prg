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
   // BUG REAL ENCONTRADO (Arturo: "el get no funciona"): un GET de
   // caracter en Clipper/FiveWin usa Len(uVar) como ancho editable del
   // buffer -- con "" (longitud 0) no hay NADA en que tipear, sin
   // importar el SIZE en pixels del control. Tiene que venir pre-rellenado
   // con Space().
   local cFindText := Space( 60 )
   local cPdf := "..\tests\enciclopedia de soldadura.pdf"   // TEMPORAL: probando seleccion/busqueda multi-pagina -- ver DESIGN.md 70.1

   PdfViewLogReset()   // diagnostico TEMPORAL de BuildComposite(), ver pdf_viewer.prg

   DEFINE WINDOW oWnd TITLE "PDFEngine32 - " + cPdf ;
      FROM 0, 0 TO 700, 1000 PIXEL
		
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

		// AcroForm (ver pdf_form.h/DESIGN.md): sobrescribe el archivo
		// ABIERTO (oPdf:cFile) con los campos editados -- Arturo eligio
		// explicitamente que Guardar pise el original (no "guardar como"),
		// igual que Acrobat; pdf_write.c preserva el original como
		// "<archivo>.bak" antes de reemplazarlo, asi que nunca se pierde
		// del todo aunque el escritor nuevo tenga un bug. Confirmacion
		// simple antes de la primera escritura, dado que es destructivo
		// sobre el archivo real del usuario.
		DEFINE BUTTON OF oBar ;
			 PROMPT "Guardar" ;
			 ACTION ( IIF( MsgYesNo( "Guardar sobrescribe " + oPdf:cFile + ;
			                          Chr(13) + "(el original queda como .bak). Continuar?", ;
			                          "PDFEngine32" ), ;
			               IIF( Pdf_FormSave( oPdf:pDoc, oPdf:cFile ), ;
			                    MsgInfo( "Guardado." ), ;
			                    MsgStop( "No se pudo guardar (sin cambios pendientes, o el documento esta encriptado)." ) ), NIL ) )

		// Impresion (ver METHOD PrintDocument() en pdf_viewer.prg):
		// dialogo nativo de Windows para elegir impresora y rango de
		// paginas (Todas/Desde-Hasta), manda cada pagina a 300 DPI via
		// WinAPI directo (StartDoc/StartPage/EndPage/DibDraw).
		DEFINE BUTTON OF oBar ;
			 PROMPT "Imprimir" ;
			 ACTION ( oPdf:PrintDocument() )

		// Rotar (Arturo: "necesito un proceso de rotar pagina 90 grados")
		// -- suma 90 a la rotacion de vista (::nUserRotate en
		// pdf_viewer.prg), afecta pantalla Y lo que se manda a imprimir.
		DEFINE BUTTON OF oBar ;
			 PROMPT "Rotar" ;
			 ACTION ( oPdf:RotatePage(), oSayPage:Refresh() )

		// Resaltado de texto (Arturo: "colocar anotaciones de resaltado de
		// textos", ver DESIGN.md) -- toma la seleccion de texto vigente
		// (::aSelRanges, arrastre de mouse) y agrega una anotacion real
		// /Highlight a la pagina (amarillo, 40% Multiply -- sin selector de
		// color en esta version). Igual que AcroForm: solo muta el
		// documento EN MEMORIA, el boton "Guardar" (arriba) persiste al
		// archivo.
		DEFINE BUTTON OF oBar ;
			 PROMPT "Resaltar" ;
			 ACTION ( oPdf:HighlightSelection(), oSayPage:Refresh() )


      oPdf := TPdfViewer():New( oWnd, ALTO_TOOLBAR, 0, ;
                                 oWnd:nWidth, oWnd:nHeight - ALTO_TOOLBAR )
      // TSay():New() -- say.prg linea 31: nRow,nCol,bText,oWnd,cPicture,
      // oFont,lCentered,lRight,lBorder,lPixels,nClrText,nClrBack,nWidth,
      // nHeight,... -- BUG REAL ENCONTRADO (la caja oscura de fondo que se
      // vio en pantalla): faltaban 2 comas vacias entre lPixels y nWidth
      // (saltando nClrText/nClrBack) para que 110/20 caigan en nWidth/
      // nHeight y no en nClrText/nClrBack.
      //
      // 16 botones ahora (<< < > >> Ajustar 100% Ancho - + Copiar Buscar
      // Sigue Guardar Imprimir Rotar Resaltar -- 45px cada uno, ver
      // DEFINE BUTTON arriba), la barra ocupa 16*45=720px -- se corre todo
      // lo que va DESPUES de ella para no solaparse (la ventana se agrando
      // a 1000px de ancho, "FROM 0,0 TO 700,1000 PIXEL", para que entre
      // con margen).
      oSayPage := TSay():New( 5, 730, ;
         {|| "" + hb_ntos( oPdf:nCurPage ) + " / " + hb_ntos( oPdf:nPageCount ) }, ;
         oWnd, , , , .F., .F., .T., , , 60, 15 )

      // PICTURE es necesaria aca no solo por el formato: FiveWin.ch define
      // TRES macros distintas para "@ nRow,nCol GET" (lineas 1233/1262/1320)
      // y sin PICTURE (que solo aceptan las ultimas dos) matcheaba la
      // PRIMERA (la que arma un TMultiGet, pensado para edicion
      // MULTILINEA/memo, no para un cuadro de busqueda de una linea) --
      // sumado al bug de Space() de arriba, entre las dos cosas el GET no
      // aceptaba texto en absoluto.
      @ 5, 800 GET oGetFind VAR cFindText PICTURE "@S30" SIZE 150, 20 OF oWnd PIXEL

   ACTIVATE WINDOW oWnd ;
      ON INIT ( IIF( oPdf:Open( cPdf ) , oSayPage:Refresh(), ;
                     MsgStop( "No se pudo abrir " + cPdf, "PDFEngine32" ) ) ) ;
      ON RESIZE ( oPdf:Resize( nWidth, nHeight - ALTO_TOOLBAR ) ) ;
      VALID ( oPdf:Close(), .T. )

return nil
