// pdf_viewer.prg
//
// ============================================================================
// TPdfViewer -- clase FiveWin para mostrar y navegar documentos PDF via
// PDFEngine32 (envuelve PDF_OPEN/PDF_CLOSE/PDF_PAGECOUNT/PDF_RENDERTOHBITMAP,
// expuestas en pdf_hbfunc.c).
//
// A diferencia de pdf_hbfunc.c (que no se pudo compilar/probar en el sandbox
// donde se escribio -- ver advertencia al inicio de ese archivo), ESTE
// archivo SI se escribio con acceso directo al source real de FWH2603
// instalado en esta maquina (d:\prgsmio\FWH2603\source\classes\), asi que
// cada API de FiveWin que se usa aca esta confirmada contra el .prg real,
// no de memoria:
//   - TBitmap:New()          -- bitmap.prg linea 50 (firma), ya usada y
//                                confirmada en pdf_demo.prg.
//   - TBitmap lScroll/nWidth()/nHeight()/ScrollAdjust() -- bitmap.prg lineas
//     38, 107-130, 1071-1110: con lScroll:=.T. el control trae SU PROPIO
//     manejo de scrollbars (::oVScroll/::oHScroll) -- nWidth()/nHeight() son
//     METODOS que devuelven el tamanio REAL del bitmap asignado
//     (GetBmpWidth/GetBmpHeight * nZoom), mientras que el area VISIBLE del
//     control (lo que ocupa en la ventana) queda fija en ::Super:nWidth()/
//     ::Super:nHeight() (los valores pasados al constructor). O sea: create
//     el control UNA sola vez con el tamanio del area de despliegue
//     disponible: si el bitmap asignado despues es mas grande, aparecen
//     scrollbars solos -- no hace falta recrear el control ni manejar
//     scroll a mano.
//   - TControl:Move()        -- control.prg linea 199/851: reposiciona/
//     redimensiona el AREA VISIBLE del control (no el bitmap).
//   - TButton():New(), TSay():New() -- button.prg linea 22, say.prg linea 31.
//   - ACTIVATE WINDOW ... ON RESIZE -- FiveWin.ch linea 1896/1917: el bloque
//     recibe (nSizeType, nWidth, nHeight) ya en pixels del area cliente.
//   - DeleteObject() -- usada tal cual en bitmap.prg lineas 786/1182 para
//     liberar HBITMAPs propios, mismo patron que se usa aca para liberar el
//     HBITMAP anterior antes de asignar uno nuevo (PDF_RENDERTOHBITMAP
//     entrega un HBITMAP nuevo e independiente en cada llamada -- sin
//     liberar el anterior se pierde un handle GDI por cada cambio de
//     pagina/zoom).
//
// Zoom soportado:
//   PDFVIEWER_ZOOM_FITHEIGHT -- la pagina se escala para que su ALTO
//      coincida con el alto del area de despliegue.
//   PDFVIEWER_ZOOM_100       -- escala 1.0 (72 DPI, el "tamanio nativo" del
//      motor -- ver comentario de 'scale' en HB_FUNC(PDF_RENDERTOHBITMAP),
//      pdf_hbfunc.c).
//
// PDF_RENDERTOHBITMAP no informa el tamanio de pagina en puntos PDF por
// separado del render ya escalado -- se deriva la PRIMERA vez que se
// necesita para cada pagina, pidiendo un render a escala 1.0 (ahi
// ancho_px/alto_px devueltos COINCIDEN con el tamanio en puntos, ya que a
// escala 1.0 es 1 pixel de pantalla por punto PDF) y cacheando el resultado
// (::nPageWidthPt/::nPageHeightPt) para no repetir ese render extra al
// cambiar de modo de zoom sobre la MISMA pagina.
// ============================================================================

#include "FiveWin.ch"

// BUG REAL ENCONTRADO (crash dentro de BitBlt() en BuildComposite(),
// diagnosticado con logging linea por linea contra un documento real de
// 6 paginas -- el crash caia justo DESPUES de "SelectObject OK, BitBlt"
// y ANTES de "BitBlt OK"): SRCCOPY NO es una constante expuesta por
// FiveWin.ch -- bcc32 avisaba "Warning W0001 Ambiguous reference
// 'SRCCOPY'" al compilar este archivo (una referencia a un simbolo sin
// #define ni variable declarada se resuelve como NIL), asi que
// BitBlt(...,SRCCOPY) mandaba NIL como codigo de operacion rasterizada
// en vez de un ROP valido. `source\function\c5lib.prg:110` (el mismo
// patron de compositing que se siguio aca) define su PROPIO
// "#define SRCCOPY 13369376" -- NO es global, cada .prg que usa BitBlt
// necesita definirlo. Valor = 0x00CC0020 (SRCCOPY real de Windows).
#define SRCCOPY 13369376

// Cursor de "mano" para arrastrar la vista con el boton central del mouse
// (ver METHOD MButtonDown/MButtonUp/MouseMove de TPdfBitmap mas abajo) --
// mismos valores que usa el propio FiveWin en otros archivos que tampoco
// los exponen via .ch (navpanels.prg, scrolimg.prg): cada .prg que los
// necesita los define localmente.
#define IDC_HAND  32649
#define IDC_ARROW 32512

// GetDeviceCaps() -- impresion (METHOD PrintDocument mas abajo). Mismo
// criterio: no vienen expuestos por FiveWin.ch, cada .prg los define
// como necesita (printer.prg de FWH2603 tiene su propia copia interna,
// no accesible desde afuera de esa clase).
#define LOGPIXELSX 88
#define LOGPIXELSY 90

// Diagnostico de BuildComposite() -- agrega una linea a pdfview_debug.log
// en el directorio de trabajo (se borra/reinicia cada vez que arranca
// pdf_demo.exe, ver PdfViewLogReset() en Main -- pdf_demo.prg). Nacio
// para diagnosticar por que el modo continuo no entraba contra un
// documento real (Arturo: "no hace lo que decis" -- termino siendo el
// gate de tiempo/dimensiones funcionando bien, y despues un crash real
// por SRCCOPY sin definir, ver DESIGN.md 70.2) -- se dejaron solo las
// lineas de nivel "gate" (una por llamada a BuildComposite(), no una
// por pagina) porque siguen siendo utiles para diagnosticar sin
// esfuerzo por que un documento puntual no entra en modo continuo.
FUNCTION PdfViewLogReset()
   LOCAL hFile := FCreate( "pdfview_debug.log" )
   IF hFile != -1
      FClose( hFile )
   ENDIF
RETURN nil

FUNCTION PdfViewLogDebug( cMsg )
   LOCAL hFile := FOpen( "pdfview_debug.log", 1 )   // FO_WRITE = 1
   IF hFile == -1
      hFile := FCreate( "pdfview_debug.log" )
   ENDIF
   IF hFile != -1
      FSeek( hFile, 0, 2 )                          // FS_END = 2
      FWrite( hFile, cMsg + Chr( 13 ) + Chr( 10 ) )
      FClose( hFile )
   ENDIF
RETURN nil

#define PDFVIEWER_ZOOM_FITHEIGHT   1
#define PDFVIEWER_ZOOM_100         2
#define PDFVIEWER_ZOOM_FITWIDTH    3   // vista continua (fase 3 del roadmap de potencialidad MuPDF): el default natural es ajustar al ANCHO, no al alto, para que las paginas apiladas se vean bien
#define PDFVIEWER_ZOOM_CUSTOM      4   // seteado por ZoomIn()/ZoomOut()

#define PDFVIEWER_ZOOM_PERCENT_MIN   25
#define PDFVIEWER_ZOOM_PERCENT_MAX   400
#define PDFVIEWER_ZOOM_PERCENT_STEP  25

// Vista continua (scroll entre paginas sin Siguiente/Anterior, Arturo:
// "mostrar hojas continuas") -- gates de seguridad de BuildComposite()
// (ver METHOD mas abajo, y el plan de esta fase: pdf_document_get_page
// recorre el arbol /Pages COMPLETO en cada llamada, sin cache -- armar
// TODO el documento sin medir primero puede colgar la ventana varios
// minutos en documentos largos/con texto denso). Constantes
// deliberadamente conservadoras -- ajustables si Arturo confirma que un
// documento razonable las esta pisando de mas.
#define PDFVIEW_CONTINUOUS_MAX_PAGES    300         // Pdf_PageCount() es O(1), gate mas barato, va primero -- solo de resguardo, el gate de tiempo de abajo es el que realmente protege
#define PDFVIEW_CONTINUOUS_MAX_SECONDS  40.0        // proyeccion tiempo_pagina1 * nPageCount -- subido de 6s a pedido de Arturo (un documento real de 103 paginas media ~32s, prefirio esperar antes que perder el scroll continuo)
#define PDFVIEW_CONTINUOUS_MAX_BYTES    (180 * 1024 * 1024)  // proceso de 32 bits sin /LARGEADDRESSAWARE, ver win32/Build.bat -- presupuesto conservador
#define PDFVIEW_CONTINUOUS_MAX_DIM_PX   30000       // limite practico de SetScrollRange/SetScrollPos (pierden precision pasado ~32767)
#define PDFVIEW_PAGE_GAP_PX             12          // separacion vertical entre paginas en el compuesto
#define PDFVIEW_SIDE_MARGIN_PX          16          // margen gris a los costados

// nX/nY de TBitmap (bitmap.prg linea 35): "IMPORTANT: nX is Vertical and nY
// is horizontal" (comentario textual del propio fuente FWH2603, bitmap.prg
// linea 732). CONVENCION DE SIGNO (leida de ScrollUp/ScrollDown/ScrollLeft/
// ScrollRight y de Paint()->PalBmpDraw(hDC,::nX,::nY,...), bitmap.prg lineas
// 893-976 y 743-783): nX/nY son el offset de DESTINO (pixels de control)
// donde queda el pixel (0,0) del bitmap -- SIEMPRE <= 0, mas negativos
// cuanto mas se scrollea abajo/derecha (0 = tope). O sea: control_pixel =
// bitmap_pixel + nX/nY, y a la inversa bitmap_pixel = control_pixel - nX/nY.
// ADVERTENCIA: el propio bitmap.prg NO es 100% consistente con esto --
// VScroll()/HScroll() en los casos SB_TOP/SB_BOTTOM asignan ::nX/::nY en
// POSITIVO (::nXExtra()/::nYExtra(), probablemente un bug latente ajeno a
// este archivo, esos casos solo se disparan si el usuario hace Home/End
// sobre la scrollbar) -- se prioriza la convencion de ScrollUp/ScrollDown/
// Paint() por ser la que corre en el camino normal (rueda del mouse, drag
// del thumb, flechas). Verificar empiricamente con una pagina scrolleada
// (ver Etapa 4 del plan) y ajustar el signo aca si hace falta.

// Subido de 192 a 512 (Arturo, en vivo -- mismo patron que el umbral de
// tiempo de BuildComposite en la fase 3, DESIGN.md seccion 70.2).
// ACTUALIZACION (DESIGN.md seccion 74): en su momento parecia que
// tests/mupdf_bug.cgiid=701945-slow.rendering.pdf necesitaba mas de
// 192MB para una pagina -- pero la causa real era un bug de verdad en
// finish_path() (pdf_render.c), que tiraba una mascara de clip de
// pagina completa a la basura en CADA "re W n" (clip rectangular
// simple, el caso mas comun) en vez de aprovechar el fast-path barato
// ya existente. Arreglado: el mismo archivo ahora renderiza completo
// usando apenas 165.8MB, MENOS que el limite viejo de 192MB. Subir
// este numero no fue lo que arreglo el archivo -- se deja en 512
// igual, como margen razonable (sigue siendo conservador para un
// proceso de 32 bits sin /LARGEADDRESSAWARE, ver win32/Build.bat), no
// porque haga falta para este caso puntual.
#define PDFVIEWER_DEFAULT_BUDGET_MB  512

//----------------------------------------------------------------------------//
// TPdfFormGet -- subclase chica de TGet (AcroForm, ver pdf_form.h/
// DESIGN.md) usada SOLO para editar campos de texto: TPdfBitmap crea
// una instancia dinamica, posicionada en pixels sobre el campo
// clickeado (ver TPdfBitmap:StartFieldEdit mas abajo), y la destruye
// al confirmar/cancelar. TGet:LostFocus() (tget.prg linea 3064) YA
// vuelca el buffer editado al bSetGet vinculado sin necesidad de un
// READ activo (confirmado leyendo el fuente real) -- alcanza con
// interceptarla aca para disparar el commit hacia PDF_FORM_SETFIELDVALUE
// + destruir el control + re-renderizar. Enter hace lo mismo sin
// esperar a perder el foco.
//----------------------------------------------------------------------------//

CLASS TPdfFormGet FROM TGet

   DATA oPdfBmp     // TPdfBitmap dueño, para el callback de commit

   METHOD LostFocus( hWndGetFocus )
   METHOD KeyDown( nKey, nFlags )

ENDCLASS

//----------------------------------------------------------------------------//

METHOD LostFocus( hWndGetFocus ) CLASS TPdfFormGet
   ::Super:LostFocus( hWndGetFocus )
   if ::oPdfBmp != nil
      ::oPdfBmp:CommitFieldEdit()
   endif
return nil

//----------------------------------------------------------------------------//

METHOD KeyDown( nKey, nFlags ) CLASS TPdfFormGet
   if nKey == VK_RETURN
      if ::oPdfBmp != nil
         ::oPdfBmp:CommitFieldEdit()
      endif
      return 0
   endif
return ::Super:KeyDown( nKey, nFlags )

//----------------------------------------------------------------------------//
// TPdfBitmap -- subclase de TBitmap (fase 2 del roadmap de potencialidad
// MuPDF: seleccion de texto por arrastre de mouse + copiar, ver DESIGN.md
// seccion 70 y pdf_hbfunc.c: Pdf_ExtractText/Pdf_FindText/Pdf_GlyphsInRect/
// Pdf_GetGlyphText). Sobreescribe Paint() para dibujar los rectangulos de
// seleccion/resaltado de busqueda DESPUES del bitmap de la pagina (via
// InvertRect, mismo mecanismo que splitter.prg/xbrowse.prg ya usan en este
// framework para barras de seleccion -- no hace falta alpha blending real)
// -- preferido sobre "hornear" los rectangulos en el HBITMAP en si porque
// (a) copiar el bitmap completo en cada MouseMove de un arrastre seria caro
// a resoluciones de 2-4x, y (b) Paint() siempre corre con ::nX/::nY
// vigentes, asi que el overlay queda sincronizado con el scroll gratis.
//----------------------------------------------------------------------------//

CLASS TPdfBitmap FROM TBitmap

   DATA oViewer          // TPdfViewer dueño (para ::nScale/::pDoc/::nCurPage)

   // seleccion de texto activa (arrastre de mouse) y resaltado de busqueda
   // (Etapa 5, extendido en la Etapa de vista continua para cruzar
   // paginas) -- fila de 7 columnas: { nPage, nStartIdx, nEndIdx, nX0,
   // nY0, nX1, nY1 } (puntos PDF LOCALES a esa pagina). nEndIdx == -1 es
   // un sentinel especial "pagina COMPLETA seleccionada" (ver
   // BuildSelRangesBetween()/SelectedText() mas abajo) -- se usa para
   // paginas totalmente cubiertas por un arrastre multi-pagina, para no
   // tener que pedirle a Pdf_GlyphsBetweenPoints el detalle linea por
   // linea de una pagina entera (podrian ser miles de filas en una
   // seleccion larga).
   DATA aSelRanges  INIT {}
   DATA aHighlight  INIT {}

   DATA lSelecting     INIT .F.
   DATA nSelStartRow
   DATA nSelStartCol

   // Arrastrar la vista con el boton CENTRAL del mouse (cursor de mano
   // mientras dura el arrastre) -- no choca con la seleccion de texto de
   // arriba porque usa un boton distinto, asi que no hace falta ningun
   // modo/toggle: click izquierdo sigue seleccionando texto exactamente
   // igual que siempre. ::nPanStartX/::nPanStartY guardan ::nX/::nY tal
   // como estaban al apretar el boton -- el arrastre se calcula como
   // delta desde ahi, mismo criterio que MouseWheel()/ScrollToPagePoint()
   // mas abajo (asignar ::nX/::nY directo y sincronizar las scrollbars a
   // mano, en vez de simular mensajes WM_?SCROLL).
   DATA lPanning       INIT .F.
   DATA nPanStartRow
   DATA nPanStartCol
   DATA nPanStartX
   DATA nPanStartY

   METHOD MButtonDown( nRow, nCol, nKeyFlags )
   METHOD MButtonUp( nRow, nCol, nKeyFlags )

   // AcroForm (ver pdf_form.h/DESIGN.md) -- edicion de campos de texto
   // via un TGet dinamico creado/destruido por click (::oFormGet, NIL
   // = ninguno activo); ::aFormEditField es la fila de
   // Pdf_FormListFields que se esta editando (para saber a que
   // obj_num/obj_gen guardar al confirmar); ::cFormEditValue es el
   // buffer que el TGet edita en vivo (bSetGet no puede apuntar a una
   // variable Harbour comun sin MEMVAR, asi que se usa un DATA propio).
   DATA oFormGet
   DATA aFormEditField
   DATA cFormEditValue

   METHOD StartFieldEdit( aField, nPage )    // crea el TGet dinamico sobre el campo de texto 'aField'
   METHOD CommitFieldEdit()                  // vuelca ::cFormEditValue a Pdf_FormSetFieldValue, destruye el TGet, re-renderiza
   METHOD ClearFormEdit() INLINE ::CommitFieldEdit()  // alias -- llamado desde puntos que cambian de pagina/zoom/scroll grande

   METHOD Paint()
   METHOD ScrollAdjust()                     // centrado horizontal tipo Acrobat, ver implementacion
   METHOD LButtonDown( nRow, nCol, nKeyFlags )
   METHOD LButtonUp( nRow, nCol, nKeyFlags )
   METHOD MouseMove( nRow, nCol, nKeyFlags )
   METHOD MouseWheel( nKeys, nDelta, nXPos, nYPos )  // rueda del mouse -- ver comentario en la implementacion, TBitmap NO la maneja nativa
   METHOD VScroll( nWParam, nLParam )        // sincroniza ::oViewer:nCurPage con el scroll libre (thumb/flechas/track)
   METHOD HScroll( nWParam, nLParam )        // idem, por si acaso -- no cambia de pagina pero no cuesta nada
   METHOD KeyDown( nKey, nFlags )            // Ctrl+C -- atajo best-effort, ver plan de esta fase

   METHOD BmpToPagePoint( nRow, nCol )       // -> { nPage, x, y } en puntos PDF LOCALES a esa pagina
   METHOD PagePointToBmp( nPage, x, y )      // -> { nRow, nCol } de control
   METHOD ScrollToPagePoint( nPage, x, y )   // centra (x,y) de 'nPage' (puntos PDF) en el area visible
   METHOD SyncCurPage()                      // recalcula ::oViewer:nCurPage segun el scroll vigente (solo modo continuo)
   METHOD BuildSelRangesBetween( nPageA, xA, yA, nPageB, xB, yB )  // arma aSelRanges para un arrastre, cruzando paginas si hace falta

   METHOD ClearSelection()
   METHOD SelectedText()                     // -> cString UTF-8 (puede ser "")
   METHOD CopySelection()                    // -> .T./.F. (via portapapeles)

ENDCLASS

//----------------------------------------------------------------------------//

// Vista continua (fase 3): con varias paginas compuestas en UN bitmap,
// un punto de control ya no es "un punto de la pagina actual" -- primero
// hay que ubicar EN QUE PAGINA cae (via ::oViewer:PageAtOffsetY sobre el
// offset dentro del compuesto), y RECIEN AHI convertir a puntos PDF
// LOCALES a esa pagina (restando su offset dentro del compuesto antes de
// dividir por la escala). Devuelve { nPage, x, y }. Fuera de modo
// continuo, nPage es siempre ::oViewer:nCurPage y los offsets son 0 --
// mismo comportamiento de siempre.
METHOD BmpToPagePoint( nRow, nCol ) CLASS TPdfBitmap

   local nScale  := if( ::oViewer != nil .and. ::oViewer:nScale > 0, ::oViewer:nScale, 1.0 )
   local nBmpRow := nRow - ::nX
   local nBmpCol := nCol - ::nY
   local nPage   := 1
   local nOffY   := 0
   local nOffX   := 0

   if ::oViewer != nil
      if ::oViewer:lContinuousMode
         nPage := ::oViewer:PageAtOffsetY( nBmpRow )
         if nPage >= 1 .and. nPage <= Len( ::oViewer:aPageOffsetY )
            nOffY := ::oViewer:aPageOffsetY[ nPage ]
            nOffX := ::oViewer:aPageOffsetX[ nPage ]
         endif
      else
         nPage := ::oViewer:nCurPage
      endif
   endif

return { nPage, ( nBmpCol - nOffX ) / nScale, ( nBmpRow - nOffY ) / nScale }   // { nPage, x, y }

//----------------------------------------------------------------------------//
// Inversa de BmpToPagePoint -- recibe un punto EN PUNTOS PDF LOCAL a
// 'nPage' y devuelve la posicion en pixeles de CONTROL (ya con el offset
// de esa pagina dentro del compuesto sumado, si aplica).
METHOD PagePointToBmp( nPage, x, y ) CLASS TPdfBitmap

   local nScale := if( ::oViewer != nil .and. ::oViewer:nScale > 0, ::oViewer:nScale, 1.0 )
   local nOffY  := 0
   local nOffX  := 0

   if ::oViewer != nil .and. ::oViewer:lContinuousMode .and. ;
      nPage >= 1 .and. nPage <= Len( ::oViewer:aPageOffsetY )
      nOffY := ::oViewer:aPageOffsetY[ nPage ]
      nOffX := ::oViewer:aPageOffsetX[ nPage ]
   endif

return { y * nScale + nOffY + ::nX, x * nScale + nOffX + ::nY }                 // { nRow, nCol }

//----------------------------------------------------------------------------//
// Fondo gris + hoja centrada tipo Acrobat (Arturo: "deberia mostrar una
// hoja con un fondo gris... permitiendo hacer zoom y mostrar hojas
// continuas"). El gris de fondo sale gratis (TBitmap:Paint() ya llena
// TODO el area no cubierta por el bitmap con ::oWnd:oBrush:hBrush --
// ver bitmap.prg:704-724 -- alcanza con poner ese brush en gris, ver
// TPdfViewer:New()). El centrado horizontal NO sale gratis: TBitmap
// ancla el bitmap en (::nX,::nY) sin ningun concepto de margen aparte.
// En vez de agregar un campo de offset SEPARADO (que habria que sumar a
// mano en Paint()/BmpToPagePoint/PagePointToBmp/ScrollToPagePoint, con
// riesgo real de que alguno se quede desincronizado -- exactamente el
// error que se evito acá), se dobla el offset de centrado DENTRO de
// ::nY mismo: TBitmap ya pone ::nY=0 cuando la pagina entra completa en
// el ancho visible (bitmap.prg:1079-1093, "sin scroll horizontal
// posible"); esta version corre eso a un valor POSITIVO (centrado) en
// vez de 0 en ese mismo caso. Como BmpToPagePoint/PagePointToBmp/
// ScrollToPagePoint YA leen ::nY como "cuanto se corrio el dibujo del
// bitmap" (ver comentario de convencion de signo arriba), el centrado
// queda automaticamente correcto en los tres SIN tocarlos -- el
// centrado es, en los hechos, un caso mas de "offset de dibujo", no un
// concepto nuevo.
//----------------------------------------------------------------------------//

METHOD ScrollAdjust() CLASS TPdfBitmap

   local aRect
   local nVisWidth
   local nMinY

   ::Super:ScrollAdjust()

   // BUG REAL ENCONTRADO (Arturo: "debe estar al centro de la ventana no
   // a la izquierda"): ::Super:nWidth() NO devuelve el ancho VISIBLE del
   // control como parecia -- TBitmap ya sobreescribe nWidth() para
   // devolver el ancho del BITMAP (bitmap.prg:126-128,
   // "GetBmpWidth(::hBitmap)*::nZoom"), asi que ::Super:nWidth() desde
   // ACA (un metodo de TPdfBitmap, donde ::Super es TBitmap) ejecuta ESA
   // version, no la de TControl -- la formula de centrado terminaba
   // comparando ancho-del-bitmap contra si mismo (::nWidth() <=
   // ::Super:nWidth() siempre verdadero) y el offset siempre daba
   // (W-W)/2=0. GetClientRect(::hWnd) esquiva por completo esa cadena de
   // "Super" y da el ancho VISIBLE real del control.
   aRect     := GetClientRect( ::hWnd )
   nVisWidth := aRect[ 4 ] - aRect[ 2 ]

   if ::nWidth() <= nVisWidth
      ::nY := Int( ( nVisWidth - ::nWidth() ) / 2 )
   else
      // BUG REAL ENCONTRADO (Arturo: "deberia centrar la pagina al
      // iniciar cuando es mas grande" -- plano tecnico apaisado mas
      // ancho que la ventana, aparecia con la mitad izquierda fuera de
      // vista sin haber scrolleado nunca): TBitmap:ScrollAdjust() (el
      // ::Super: de arriba) SOLO resetea ::nY a 0 en la rama "entra
      // completo" -- en la rama "no entra" (bitmap.prg, "else lHor=.t.")
      // NO TOCA ::nY en absoluto, asi que un offset de CENTRADO viejo
      // (dejado por la rama de arriba, en otra pagina/zoom que SI
      // entraba) queda pisado como si fuera scroll real sobre un
      // bitmap mas ancho -- exactamente lo que causaba el corrimiento.
      // Fix: si ::nY quedo FUERA del rango valido para el bitmap
      // ACTUAL (0 arriba, -nYExtra() abajo), es un valor viejo/no
      // inicializado -- arrancar centrado (mostrar la mitad de la
      // pagina, no el borde izquierdo a secas) en vez de arrastrar ese
      // valor. Si ::nY ya esta en rango (el usuario scrolleo de
      // verdad), se respeta -- no pisar un scroll legitimo en cada
      // resize de ventana (ScrollAdjust corre en cada uno).
      nMinY := -::nYExtra()
      if ::nY > 0 .or. ::nY < nMinY
         ::nY := Int( -( ::nWidth() - nVisWidth ) / 2 )
      endif
   endif

return nil

//----------------------------------------------------------------------------//
// Rueda del mouse (Arturo: "debera poder moverse usando la rueda del
// mouse"). BUG REAL ENCONTRADO: TBitmap (bitmap.prg) no implementa
// MouseWheel en absoluto -- ademas, WM_MOUSEWHEEL en Windows se manda a
// la ventana que tiene el FOCO (normalmente la ventana TOP-LEVEL, oWnd),
// no al control bajo el cursor como el resto de los mensajes de mouse,
// asi que ni alcanzaba con agregar el metodo aca: hace falta que oWnd
// delegue explicitamente via ::bMouseWheel (DATA de TWindow,
// window.prg:206, evaluado por TWindow:MouseWheel() si esta seteado --
// mismo patron que usa TScrollPanel de FWH2603, scrlpanl.prg:126,
// comentado en ESE archivo porque ahi lo setea quien lo USA, no la
// clase en si). El wiring esta en TPdfViewer:New() mas abajo.
// nDelta > 0 = rueda hacia el usuario/arriba = contenido sube = ::nX se
// acerca a 0 (mismo signo que ScrollUp() de TBitmap, ver convencion al
// principio del archivo). WheelScroll() (FWH2603, function\valtostr.prg)
// lee la config de Windows "lineas por click de rueda".
//----------------------------------------------------------------------------//

METHOD MouseWheel( nKeys, nDelta, nXPos, nYPos ) CLASS TPdfBitmap

   local nStepPx := WheelScroll() * 40
   local nMinX   := -::nXExtra()
   local aRect, nVisHeight

   ( nKeys )
   ( nXPos )
   ( nYPos )

   // Salto automatico de pagina en modo de UNA pagina (Arturo: cuando el
   // documento es demasiado grande/pesado para vista continua -- ver
   // ::lContinuousMode/BuildComposite() -- llegar al principio o al
   // final de la pagina actual con la rueda debe saltar a la pagina
   // anterior/siguiente si existe, en vez de quedarse trabado en el
   // borde). ::nX ya llega EXACTO a 0 (arriba) o nMinX (abajo) gracias
   // al clamp de mas abajo (mismo mecanismo en cada llamada), asi que
   // '::nX >= 0'/'::nX <= nMinX' detecta "ya estaba en el borde" sin
   // ambiguedad de redondeo. En vista continua no aplica -- todas las
   // paginas ya estan en el mismo compuesto scrolleable, no hay "borde
   // de pagina" que saltar.
   if ::oViewer != nil .and. !::oViewer:lContinuousMode
      if nDelta > 0 .and. ::nX >= 0 .and. ::oViewer:nCurPage > 1
         ::oViewer:PrevPage()                   // re-renderiza la pagina anterior (ApplyRender->ScrollAdjust ya ajusta rango/nX=0 si entra completa)
         aRect      := GetClientRect( ::hWnd )
         nVisHeight := aRect[ 3 ] - aRect[ 1 ]
         if ::nHeight() > nVisHeight             // la pagina anterior SI tiene de donde scrollear verticalmente
            ::nX := -::nXExtra()                 // arrancar en su PARTE DE ABAJO -- continuidad natural al hojear hacia atras
            if ::oVScroll != nil .and. ::nVStep > 0
               ::oVScroll:SetPos( Int( -::nX / ::nVStep ) )
            endif
         endif
         ::SyncCurPage()
         ::Refresh()
         return 0
      endif
      if nDelta < 0 .and. ::nX <= nMinX .and. ::oViewer:nCurPage < ::oViewer:nPageCount
         ::oViewer:NextPage()                    // arranca en su PARTE DE ARRIBA (::nX=0, default de ApplyRender/GoToPage)
         ::SyncCurPage()
         ::Refresh()
         return 0
      endif
   endif

   if nDelta > 0
      ::nX += nStepPx
      if ::nX > 0
         ::nX := 0
      endif
   else
      ::nX -= nStepPx
      if ::nX < nMinX
         ::nX := nMinX
      endif
   endif

   if ::oVScroll != nil .and. ::nVStep > 0
      ::oVScroll:SetPos( Int( -::nX / ::nVStep ) )
   endif

   ::SyncCurPage()
   ::Refresh()

return 0

//----------------------------------------------------------------------------//
// VScroll/HScroll (WM_VSCROLL/WM_HSCROLL -- flechas de la scrollbar,
// arrastre del thumb, click en el track) son el UNICO otro camino de
// scroll ademas de la rueda del mouse (ScrollUp/ScrollDown/PageUp/
// PageDown de TBitmap se disparan DESDE aca, ver bitmap.prg) -- hay que
// sincronizar ::oViewer:nCurPage aca tambien, si no el contador de
// pagina y Find() ("buscar desde la pagina actual") quedan desactualizados
// en cuanto el usuario scrollea con la scrollbar en vez de la rueda.
//----------------------------------------------------------------------------//

METHOD VScroll( nWParam, nLParam ) CLASS TPdfBitmap
   local nRes := ::Super:VScroll( nWParam, nLParam )
   ::SyncCurPage()
return nRes

//----------------------------------------------------------------------------//

METHOD HScroll( nWParam, nLParam ) CLASS TPdfBitmap
   local nRes := ::Super:HScroll( nWParam, nLParam )
   ::SyncCurPage()
return nRes

//----------------------------------------------------------------------------//

METHOD SyncCurPage() CLASS TPdfBitmap
   if ::oViewer != nil .and. ::oViewer:lContinuousMode
      ::oViewer:nCurPage := ::oViewer:PageAtOffsetY( -::nX )
   endif
return nil

//----------------------------------------------------------------------------//
// Centra el punto de pagina (x,y) en el area visible (usado por
// TPdfViewer:FindNext(), ver mas abajo, para llevar el match encontrado a
// la vista). Asigna ::nX/::nY DIRECTO (en vez de confiar en el mensaje
// WM_VSCROLL/WM_HSCROLL que dispara SetPos -- el caso SB_THUMBPOSITION que
// lo procesaria esta comentado en bitmap.prg, ver advertencia de convencion
// de signo al principio de este archivo) y despues sincroniza la posicion
// visual de las scrollbars a mano.
//----------------------------------------------------------------------------//

METHOD ScrollToPagePoint( nPage, x, y ) CLASS TPdfBitmap

   local nScale     := if( ::oViewer != nil .and. ::oViewer:nScale > 0, ::oViewer:nScale, 1.0 )
   local aRect      := GetClientRect( ::hWnd )
   local nVisHeight := aRect[ 3 ] - aRect[ 1 ]
   local nVisWidth  := aRect[ 4 ] - aRect[ 2 ]
   local nOffY      := 0
   local nOffX      := 0
   local nBmpRow, nBmpCol
   local nMinX      := -::nXExtra()
   local nMinY      := -::nYExtra()
   local nNewX, nNewY

   // BUG REAL ENCONTRADO (mismo error que ::Super:nWidth() en
   // ScrollAdjust() -- ver ese comentario): ::Super:nHeight()/nWidth()
   // ACA devolvian el tamanio del BITMAP, no el area VISIBLE del
   // control, asi que "::nHeight() <= nVisHeight" comparaba el bitmap
   // contra si mismo (siempre verdadero) y esta funcion NUNCA scrolleaba
   // verticalmente -- no se habia notado porque el modo "Ajustar" (el
   // usado en las pruebas de busqueda hasta ahora) hace que la pagina
   // SIEMPRE entre completa verticalmente por definicion, enmascarando
   // el bug. GetClientRect(::hWnd) da el tamanio visible real.
   if ::oViewer != nil .and. ::oViewer:lContinuousMode .and. ;
      nPage >= 1 .and. nPage <= Len( ::oViewer:aPageOffsetY )
      nOffY := ::oViewer:aPageOffsetY[ nPage ]
      nOffX := ::oViewer:aPageOffsetX[ nPage ]
   endif

   nBmpRow := y * nScale + nOffY
   nBmpCol := x * nScale + nOffX

   if ::nHeight() <= nVisHeight       // TODO el compuesto/pagina entra vertical -- nada que scrollear
      nNewX := 0
   else
      nNewX := -( nBmpRow - nVisHeight / 2 )
      if nNewX > 0        ; nNewX := 0        ; endif
      if nNewX < nMinX     ; nNewX := nMinX    ; endif
   endif

   if ::nWidth() <= nVisWidth
      // entra completo horizontal -- centrado (mismo criterio que
      // ScrollAdjust(), no simplemente 0/flush-left).
      nNewY := Int( ( nVisWidth - ::nWidth() ) / 2 )
   else
      nNewY := -( nBmpCol - nVisWidth / 2 )
      if nNewY > 0        ; nNewY := 0        ; endif
      if nNewY < nMinY     ; nNewY := nMinY    ; endif
   endif

   ::nX := nNewX
   ::nY := nNewY

   if ::oVScroll != nil .and. ::nVStep > 0
      ::oVScroll:SetPos( Int( -::nX / ::nVStep ) )
   endif
   if ::oHScroll != nil .and. ::nHStep > 0
      ::oHScroll:SetPos( Int( -::nY / ::nHStep ) )
   endif

return nil

//----------------------------------------------------------------------------//

METHOD ClearSelection() CLASS TPdfBitmap

   local lHad := Len( ::aSelRanges ) > 0

   ::aSelRanges := {}
   if lHad
      ::Refresh()
   endif

return nil

//----------------------------------------------------------------------------//

METHOD LButtonDown( nRow, nCol, nKeyFlags ) CLASS TPdfBitmap

   local aP, aField

   ::ClearFormEdit()

   // AcroForm: un click sobre un campo de texto/checkbox editable tiene
   // prioridad sobre el arranque de seleccion de texto normal -- si no
   // cae en ningun campo (el caso de siempre, la enorme mayoria de los
   // PDFs no tienen AcroForm), sigue exactamente el comportamiento de
   // antes.
   if ::oViewer != nil .and. ::oViewer:pDoc != nil
      aP := ::BmpToPagePoint( nRow, nCol )
      aField := ::oViewer:HitTestField( aP[ 1 ], aP[ 2 ], aP[ 3 ] )
      if aField != nil
         if aField[ 1 ] == 1                       // texto
            ::StartFieldEdit( aField, aP[ 1 ] )
            return 0
         elseif aField[ 1 ] == 2 .and. !Empty( aField[ 11 ] )  // checkbox con estado "on" resoluble
            Pdf_FormSetFieldValue( ::oViewer:pDoc, aField[ 9 ], aField[ 10 ], 2, ;
               if( aField[ 2 ] == aField[ 11 ], "Off", aField[ 11 ] ) )
            ::oViewer:RefreshRender()
            return 0
         endif
      endif
   endif

   ::ClearSelection()
   ::lSelecting   := .T.
   ::nSelStartRow := nRow
   ::nSelStartCol := nCol
   ::Capture()

return ::Super:LButtonDown( nRow, nCol, nKeyFlags )

//----------------------------------------------------------------------------//
// Arrastrar la vista con el boton central (ver DATA lPanning arriba). No se
// llama ::ClearSelection() aca -- a diferencia del click izquierdo, arrastrar
// con el boton central no tiene por que descartar una seleccion de texto ya
// hecha (mismo criterio que un visor real: el pan no es una accion de
// seleccion).
//----------------------------------------------------------------------------//

METHOD MButtonDown( nRow, nCol, nKeyFlags ) CLASS TPdfBitmap

   ::ClearFormEdit()
   ::lPanning     := .T.
   ::nPanStartRow := nRow
   ::nPanStartCol := nCol
   ::nPanStartX   := ::nX
   ::nPanStartY   := ::nY
   ::Capture()
   SetCursor( LoadCursor( 0, IDC_HAND ) )

return ::Super:MButtonDown( nRow, nCol, nKeyFlags )

//----------------------------------------------------------------------------//

METHOD MButtonUp( nRow, nCol, nKeyFlags ) CLASS TPdfBitmap

   if ::lPanning
      ::lPanning := .F.
      ReleaseCapture()
      SetCursor( LoadCursor( 0, IDC_ARROW ) )
   endif

return ::Super:MButtonUp( nRow, nCol, nKeyFlags )

//----------------------------------------------------------------------------//

METHOD MouseMove( nRow, nCol, nKeyFlags ) CLASS TPdfBitmap

   local aP0, aP1
   local aRect, nVisHeight, nVisWidth
   local nMinX, nMinY, nNewX, nNewY

   // Arrastre con el boton central en curso -- mueve ::nX/::nY directo
   // (mismo mecanismo que MouseWheel()/ScrollToPagePoint() mas abajo) por
   // el delta desde donde se apreto el boton, con el mismo signo que ya
   // esta confirmado ahi ("arrastrar hacia abajo" = mismo sentido que
   // "rueda hacia el usuario" = ::nX se acerca a 0). Si una dimension
   // entra completa en el area visible (nXExtra()/nYExtra() no tienen un
   // rango util para esa dimension, ver ScrollToPagePoint) NO se toca esa
   // coordenada -- si no, un arrastre horizontal en un documento
   // centrado horizontalmente "saltaria" a los bordes en vez de quedarse
   // quieto (nada que scrollear en esa direccion).
   if ::lPanning
      aRect      := GetClientRect( ::hWnd )
      nVisHeight := aRect[ 3 ] - aRect[ 1 ]
      nVisWidth  := aRect[ 4 ] - aRect[ 2 ]

      if ::nHeight() > nVisHeight
         nMinX := -::nXExtra()
         nNewX := ::nPanStartX + ( nRow - ::nPanStartRow )
         if nNewX > 0     ; nNewX := 0     ; endif
         if nNewX < nMinX ; nNewX := nMinX ; endif
         ::nX := nNewX
      endif

      if ::nWidth() > nVisWidth
         nMinY := -::nYExtra()
         nNewY := ::nPanStartY + ( nCol - ::nPanStartCol )
         if nNewY > 0     ; nNewY := 0     ; endif
         if nNewY < nMinY ; nNewY := nMinY ; endif
         ::nY := nNewY
      endif

      if ::oVScroll != nil .and. ::nVStep > 0
         ::oVScroll:SetPos( Int( -::nX / ::nVStep ) )
      endif
      if ::oHScroll != nil .and. ::nHStep > 0
         ::oHScroll:SetPos( Int( -::nY / ::nHStep ) )
      endif

      ::SyncCurPage()
      SetCursor( LoadCursor( 0, IDC_HAND ) )
      ::Refresh()

      return 0
   endif

   if ::lSelecting .and. ::oViewer != nil .and. ::oViewer:pDoc != nil
      aP0 := ::BmpToPagePoint( ::nSelStartRow, ::nSelStartCol )
      aP1 := ::BmpToPagePoint( nRow, nCol )

      // BUG REAL ENCONTRADO (Arturo: "la seleccion no deberia ser al azar
      // en la pantalla sino que vaya de acuerdo con la secuencia de
      // letras y palabras") -- Pdf_GlyphsInRect selecciona por
      // INTERSECCION GEOMETRICA de rectangulo (recorta cada linea a la
      // misma columna de X), lo que NO es como funciona la seleccion de
      // texto en cualquier editor/visor real: arrastrar de la mitad de
      // la linea 1 a la mitad de la linea 3 deberia traer el RESTO de la
      // linea 1, TODA la linea 2, y el PRINCIPIO de la linea 3 -- no solo
      // la columna angosta donde el mouse paso. Pdf_GlyphsBetweenPoints
      // mapea los dos puntos del arrastre a POSICIONES en la secuencia
      // de texto (ver pdf_text_nearest_glyph) y selecciona el rango
      // CONTIGUO entre ellas, siguiendo el orden natural de lectura --
      // BuildSelRangesBetween extiende esto para cuando el arrastre
      // cruza el borde entre paginas (vista continua).
      ::aSelRanges := ::BuildSelRangesBetween( aP0[ 1 ], aP0[ 2 ], aP0[ 3 ], aP1[ 1 ], aP1[ 2 ], aP1[ 3 ] )
      ::Refresh()
   endif

return ::Super:MouseMove( nRow, nCol, nKeyFlags )

//----------------------------------------------------------------------------//

METHOD LButtonUp( nRow, nCol, nKeyFlags ) CLASS TPdfBitmap

   if ::lSelecting
      ::lSelecting := .F.
      ReleaseCapture()
   endif

return ::Super:LButtonUp( nRow, nCol, nKeyFlags )

//----------------------------------------------------------------------------//

METHOD KeyDown( nKey, nFlags ) CLASS TPdfBitmap

   // 'C' == Asc( "C" ) == 67 -- los codigos de tecla virtual de Windows
   // para letras COINCIDEN con el ASCII mayuscula (mismo idioma que
   // control.prg/fget.prg de este mismo framework, ver p.ej. fget.prg
   // linea 1855: "GetKeyState( VK_CONTROL )" junto al chequeo de nKey).
   if nKey == 67 .and. GetKeyState( VK_CONTROL )
      ::CopySelection()
      return 0
   endif

return ::Super:KeyDown( nKey, nFlags )

//----------------------------------------------------------------------------//
// Arma ::aSelRanges (formato de 7 columnas, ver DATA aSelRanges arriba)
// para un arrastre entre el punto (nPageA,xA,yA) y (nPageB,xB,yB) --
// misma pagina (el caso de siempre, fuera de vista continua tambien cae
// aca ya que nPageA siempre == nPageB en ese modo) o cruzando varias.
// Para las paginas TOTALMENTE cubiertas (ni el inicio ni el fin del
// arrastre caen ahi) usa un marcador "pagina completa" (nEndIdx=-1) en
// vez de pedirle a Pdf_GlyphsBetweenPoints el detalle esquina-a-esquina
// (que devolveria una fila por cada linea de texto -- miles en una
// seleccion larga); solo las DOS paginas de borde piden el detalle real.
//----------------------------------------------------------------------------//

METHOD BuildSelRangesBetween( nPageA, xA, yA, nPageB, xB, yB ) CLASS TPdfBitmap

   local aResult := {}
   local nLo, nHi, xLo, yLo, xHi, yHi
   local nPage, aRows, j, aSize

   if ::oViewer == nil .or. ::oViewer:pDoc == nil
      return {}
   endif

   if nPageA <= nPageB
      nLo := nPageA ; xLo := xA ; yLo := yA
      nHi := nPageB ; xHi := xB ; yHi := yB
   else
      nLo := nPageB ; xLo := xB ; yLo := yB
      nHi := nPageA ; xHi := xA ; yHi := yA
   endif

   if nLo == nHi
      aRows := Pdf_GlyphsBetweenPoints( ::oViewer:pDoc, nLo, xLo, yLo, xHi, yHi )
      for j := 1 to Len( aRows )
         AAdd( aResult, { nLo, aRows[ j ][ 1 ], aRows[ j ][ 2 ], aRows[ j ][ 3 ], aRows[ j ][ 4 ], aRows[ j ][ 5 ], aRows[ j ][ 6 ] } )
      next
      return aResult
   endif

   // primera pagina (parcial): desde el punto de arranque hasta el
   // final de ESA pagina (esquina inferior derecha como proxy -- el
   // motor lo resuelve al glyph mas cercano, ver pdf_text_nearest_glyph).
   aSize := ::oViewer:PageSizePt( nLo )
   aRows := Pdf_GlyphsBetweenPoints( ::oViewer:pDoc, nLo, xLo, yLo, aSize[ 1 ], aSize[ 2 ] )
   for j := 1 to Len( aRows )
      AAdd( aResult, { nLo, aRows[ j ][ 1 ], aRows[ j ][ 2 ], aRows[ j ][ 3 ], aRows[ j ][ 4 ], aRows[ j ][ 5 ], aRows[ j ][ 6 ] } )
   next

   // paginas intermedias: COMPLETAS, un solo marcador cada una.
   for nPage := nLo + 1 to nHi - 1
      aSize := ::oViewer:PageSizePt( nPage )
      AAdd( aResult, { nPage, 1, -1, 0, 0, aSize[ 1 ], aSize[ 2 ] } )
   next

   // ultima pagina (parcial): desde el principio de ESA pagina hasta el
   // punto donde termino el arrastre.
   aRows := Pdf_GlyphsBetweenPoints( ::oViewer:pDoc, nHi, 0, 0, xHi, yHi )
   for j := 1 to Len( aRows )
      AAdd( aResult, { nHi, aRows[ j ][ 1 ], aRows[ j ][ 2 ], aRows[ j ][ 3 ], aRows[ j ][ 4 ], aRows[ j ][ 5 ], aRows[ j ][ 6 ] } )
   next

return aResult

//----------------------------------------------------------------------------//
// Recorre ::aSelRanges AGRUPANDO por pagina (ya vienen en orden de
// pagina, ver BuildSelRangesBetween) -- por cada grupo resuelve el
// sentinel "pagina completa" (nEndIdx=-1, ver DATA aSelRanges) contra
// Pdf_ExtractText para saber la cantidad real de glyphs, y llama
// Pdf_GetGlyphText UNA vez por pagina (que ya concatena con salto de
// linea las filas de esa misma pagina) -- concatena los resultados de
// paginas DISTINTAS con otro salto de linea de por medio.
//----------------------------------------------------------------------------//

METHOD SelectedText() CLASS TPdfBitmap

   local cResult := ""
   local lFirst := .T.
   local i := 1
   local nPage, aPageRanges, cPageText, nGlyphs

   if ::oViewer == nil .or. ::oViewer:pDoc == nil .or. Len( ::aSelRanges ) == 0
      return ""
   endif

   while i <= Len( ::aSelRanges )
      nPage := ::aSelRanges[ i ][ 1 ]
      aPageRanges := {}

      while i <= Len( ::aSelRanges ) .and. ::aSelRanges[ i ][ 1 ] == nPage
         if ::aSelRanges[ i ][ 3 ] == -1
            nGlyphs := Pdf_ExtractText( ::oViewer:pDoc, nPage )
            if nGlyphs > 0
               AAdd( aPageRanges, { 1, nGlyphs } )
            endif
         else
            AAdd( aPageRanges, { ::aSelRanges[ i ][ 2 ], ::aSelRanges[ i ][ 3 ] } )
         endif
         i++
      enddo

      if Len( aPageRanges ) > 0
         cPageText := Pdf_GetGlyphText( ::oViewer:pDoc, nPage, aPageRanges )
         if ! lFirst
            cResult += Chr( 10 )
         endif
         cResult += cPageText
         lFirst := .F.
      endif
   enddo

return cResult

//----------------------------------------------------------------------------//

METHOD CopySelection() CLASS TPdfBitmap

   local cText := ::SelectedText()

   if Empty( cText )
      return .F.
   endif

return TClipBoard():New( , ::oWnd ):SetText( cText )

//----------------------------------------------------------------------------//
// AcroForm: crea el TGet dinamico sobre el campo de texto 'aField' (fila
// de Pdf_FormListFields), ubicado con PagePointToBmp -- MISMA formula
// que ya usan Paint()/BmpToPagePoint, asi que queda alineado al
// bitmap sin importar zoom/scroll/centrado vigentes. 'aField[4]'/
// 'aField[6]' (Y) ya vienen en la convencion top-down (ver
// pdf_hb_resolve_page_box, pdf_hbfunc.c) -- no hace falta invertir
// nada aca, a diferencia de si fueran /Rect crudo.
//----------------------------------------------------------------------------//

METHOD StartFieldEdit( aField, nPage ) CLASS TPdfBitmap

   local aP0 := ::PagePointToBmp( nPage, aField[ 3 ], aField[ 4 ] )  // esquina superior-izquierda
   local aP1 := ::PagePointToBmp( nPage, aField[ 5 ], aField[ 6 ] )  // esquina inferior-derecha
   local nTop, nLeft, nW, nH

   ::ClearFormEdit()

   nTop  := Min( aP0[ 1 ], aP1[ 1 ] )
   nLeft := Min( aP0[ 2 ], aP1[ 2 ] )
   nW    := Abs( aP1[ 2 ] - aP0[ 2 ] )
   nH    := Abs( aP1[ 1 ] - aP0[ 1 ] )
   if nW < 12 ; nW := 12 ; endif
   if nH < 12 ; nH := 12 ; endif

   ::cFormEditValue := PadR( AllTrim( aField[ 2 ] ), 250 )
   ::aFormEditField := aField

   ::oFormGet := TPdfFormGet():New( nTop, nLeft, ;
      {| u | if( u == nil, ::cFormEditValue, ::cFormEditValue := u ) }, ;
      ::oWnd, nW, nH, NIL, NIL, NIL, NIL, NIL, NIL, NIL, .T. )
   ::oFormGet:oPdfBmp := Self
   ::oFormGet:SetFocus()

return nil

//----------------------------------------------------------------------------//
// AcroForm: vuelca ::cFormEditValue (ya actualizado por TGet:Assign(),
// disparado desde LostFocus/Enter -- ver TPdfFormGet arriba) al motor
// via Pdf_FormSetFieldValue, destruye el control, y re-renderiza (mismo
// patron que ZoomIn/ZoomOut). Idempotente: si no hay edicion activa
// (::oFormGet ya es NIL, p.ej. una segunda llamada disparada por Enter
// Y por la perdida de foco subsiguiente de :End()), no hace nada.
//----------------------------------------------------------------------------//

METHOD CommitFieldEdit() CLASS TPdfBitmap

   local oGet   := ::oFormGet
   local aField := ::aFormEditField
   local cNewVal

   if oGet == nil
      return nil
   endif

   ::oFormGet       := nil
   ::aFormEditField := nil

   cNewVal := AllTrim( ::cFormEditValue )
   oGet:End()

   if aField != nil .and. ::oViewer != nil .and. ::oViewer:pDoc != nil
      Pdf_FormSetFieldValue( ::oViewer:pDoc, aField[ 9 ], aField[ 10 ], 1, cNewVal )
      ::oViewer:RefreshRender()
   endif

return nil

//----------------------------------------------------------------------------//

METHOD Paint() CLASS TPdfBitmap

   local i, aRow, aTL, aBR

   ::Super:Paint()

   for i := 1 to Len( ::aHighlight )
      aRow := ::aHighlight[ i ]
      aTL  := ::PagePointToBmp( aRow[ 1 ], aRow[ 4 ], aRow[ 5 ] )
      aBR  := ::PagePointToBmp( aRow[ 1 ], aRow[ 6 ], aRow[ 7 ] )
      InvertRect( ::hDC, { aTL[ 1 ], aTL[ 2 ], aBR[ 1 ], aBR[ 2 ] } )
   next

   for i := 1 to Len( ::aSelRanges )
      aRow := ::aSelRanges[ i ]
      aTL  := ::PagePointToBmp( aRow[ 1 ], aRow[ 4 ], aRow[ 5 ] )
      aBR  := ::PagePointToBmp( aRow[ 1 ], aRow[ 6 ], aRow[ 7 ] )
      InvertRect( ::hDC, { aTL[ 1 ], aTL[ 2 ], aBR[ 1 ], aBR[ 2 ] } )
   next

return nil

//----------------------------------------------------------------------------//

CLASS TPdfViewer

   DATA oWnd                                     // ventana FiveWin donde vive el control de despliegue
   DATA oBmp                                     // TBitmap: control (con scroll propio) que muestra la pagina actual
   DATA pDoc                                     // puntero opaco devuelto por Pdf_Open()
   DATA cFile                                    // ruta del archivo PDF actualmente abierto

   DATA nPageCount    INIT 0                     // total de paginas del documento (0 = nada abierto)
   DATA nCurPage      INIT 0                     // pagina actual, 1-based

   DATA nZoomMode     INIT PDFVIEWER_ZOOM_FITHEIGHT
   DATA nScale        INIT 1.0                   // escala efectiva (puntos PDF -> pixeles) del ultimo render aplicado
   DATA nZoomPercent  INIT 100                    // solo tiene sentido bajo PDFVIEWER_ZOOM_CUSTOM, ver ZoomIn()/ZoomOut()

   // Rotacion pedida por el usuario (Arturo: "necesito un proceso de
   // rotar pagina 90 grados") -- 0/90/180/270, SUMADA al /Rotate propio
   // de cada pagina (que el motor ya respeta solo) via el 4to parametro
   // nuevo de Pdf_RenderToHBitmap. Se resetea a 0 en Open() (rotacion es
   // una preferencia de vista de ESTE documento, no global de la app) y
   // se usa tanto para pantalla (RenderCurrentPage/BuildComposite) como
   // para PrintDocument() -- si el usuario endereza la vista, tiene
   // sentido que lo impreso tambien salga derecho.
   DATA nUserRotate   INIT 0

   DATA nDispTop      INIT 0                     // area de despliegue dentro de oWnd (pixels) -- limites del control TBitmap
   DATA nDispLeft     INIT 0
   DATA nDispWidth    INIT 0
   DATA nDispHeight   INIT 0

   DATA nPageWidthPt  INIT 0                     // tamanio de la pagina ACTUAL en puntos PDF (cache, ver comentario arriba)
   DATA nPageHeightPt INIT 0

   DATA hBitmap       INIT 0                     // HBITMAP actualmente asignado a ::oBmp (para liberarlo antes de reemplazarlo)

   // vista continua (fase 3 del roadmap de potencialidad MuPDF, ver
   // METHOD BuildComposite() mas abajo) -- .T. solo si el documento paso
   // todos los gates de seguridad y ::hBitmap es un COMPUESTO de varias
   // paginas, no una sola. aPageOffsetY/aPageOffsetX/aPageWidthPx/
   // aPageHeightPx son paralelos, 1 entrada por pagina (1-based, o sea
   // el indice de array es nPage).
   DATA lContinuousMode  INIT .F.
   DATA aPageOffsetY     INIT {}
   DATA aPageOffsetX     INIT {}
   DATA aPageWidthPx     INIT {}
   DATA aPageHeightPx    INIT {}

   // busqueda de texto (Etapa 5 del roadmap de potencialidad MuPDF) --
   // ver METHOD Find()/FindNext() mas abajo.
   DATA cFindNeedle                              // NIL = no hay busqueda activa
   DATA lFindCaseSensitive INIT .F.
   DATA nFindPage          INIT 0                // pagina (1-based) donde estan ::aFindMatches
   DATA aFindMatches       INIT {}               // matches de ::nFindPage (formato de Pdf_FindText)
   DATA nFindMatchIdx      INIT 0                // indice 1-based dentro de aFindMatches, 0 = ninguno

   METHOD New( oWnd, nTop, nLeft, nWidth, nHeight ) CONSTRUCTOR

   METHOD Open( cFile, nBudgetMB )                // abre (o reemplaza) el documento mostrado. .T./.F.
   METHOD Close()                                 // cierra el documento actual (si habia) y libera el HBITMAP en pantalla
   METHOD End() INLINE ::Close()                  // alias -- algunos callers/containers FiveWin esperan :End()

   METHOD GoToPage( nPage )                       // .T./.F.
   METHOD NextPage()   INLINE ::GoToPage( ::nCurPage + 1 )
   METHOD PrevPage()   INLINE ::GoToPage( ::nCurPage - 1 )
   METHOD FirstPage()  INLINE ::GoToPage( 1 )
   METHOD LastPage()   INLINE ::GoToPage( ::nPageCount )

   METHOD SetZoomFitHeight()                      // cambia a modo "ajuste al alto" y vuelve a renderizar
   METHOD SetZoom100()                            // cambia a modo "100%" y vuelve a renderizar
   METHOD SetZoomFitWidth()                       // cambia a modo "ajuste al ancho" y vuelve a renderizar
   METHOD ZoomIn()                                // +25% (PDFVIEWER_ZOOM_PERCENT_STEP), tope PDFVIEWER_ZOOM_PERCENT_MAX
   METHOD ZoomOut()                                // -25%, piso PDFVIEWER_ZOOM_PERCENT_MIN

   METHOD RotatePage()                             // ::nUserRotate += 90 (mod 360) y vuelve a renderizar

   METHOD Resize( nWidth, nHeight )                // el caller lo invoca desde ON RESIZE de oWnd (ver pdf_demo.prg)
   METHOD Refresh() INLINE ::RenderCurrentPage()   // fuerza volver a renderizar la pagina actual con el modo/escala vigente

   // vista continua (fase 3): intenta armar el compuesto de todo el
   // documento, con gates de seguridad -- .T. si quedo en modo continuo,
   // .F. si algun gate lo freno (::lContinuousMode queda en .F., el
   // visor sigue funcionando en modo de una pagina de siempre).
   METHOD BuildComposite()
   METHOD PageAtOffsetY( nOffsetY )                // busqueda binaria sobre aPageOffsetY -- -> nPage (1-based)
   METHOD PageSizePt( nPage )                      // -> { anchoPt, altoPt } de 'nPage' (aPageWidthPx/HeightPx / nScale, o ::nPageWidthPt/HeightPt fuera de modo continuo)

   // seleccion de texto (fase 2 del roadmap de potencialidad MuPDF, ver
   // TPdfBitmap arriba) -- delegan al control real, que es quien conoce el
   // arrastre de mouse vigente.
   METHOD CopySelection()   INLINE if( ::oBmp != NIL, ::oBmp:CopySelection(), .F. )
   METHOD ClearSelection()  INLINE if( ::oBmp != NIL, ::oBmp:ClearSelection(), NIL )
   METHOD SelectedText()    INLINE if( ::oBmp != NIL, ::oBmp:SelectedText(), "" )

   // busqueda (Etapa 5): Find() arranca una busqueda nueva desde la pagina
   // actual; FindNext() avanza al siguiente match (dentro de la misma
   // pagina si quedan mas, si no a la proxima pagina que tenga alguno, con
   // wrap al llegar al final -- ver implementacion mas abajo).
   METHOD Find( cNeedle, lCaseSensitive )          // .T. si encontro algo
   METHOD FindNext()                               // .T. si encontro algo
   METHOD ClearFind()                              // cancela la busqueda activa y el resaltado

   // Impresion (Arturo: "un proceso para lanzar el documento a
   // impresion") -- pregunta rango (pagina actual/todo/rango, via
   // AskPrintRange() mas abajo) y despues el dialogo NATIVO de Windows
   // para elegir impresora/copias (TPrinter():New(...,.T.)). Cada
   // pagina se renderiza a 300 DPI (independiente del zoom en pantalla
   // -- Pdf_RenderToHBitmap toma su propio factor de escala) y se
   // manda tal cual, a su tamanio fisico real en pulgadas (sin
   // "ajustar a la hoja" -- si la pagina del PDF es mas grande que el
   // area imprimible, GDI la recorta en el borde, mismo comportamiento
   // por defecto que la mayoria de los visores en "tamanio real").
   METHOD PrintDocument()

   // AcroForm (ver pdf_form.h/DESIGN.md) -- lectura/edicion de campos
   // de texto/checkbox. HitTestField consulta Pdf_FormListFields FRESCO
   // en cada llamada (sin cache: son pocos widgets por pagina, no vale
   // la pena arriesgar un cache viejo) y devuelve la fila que contiene
   // (x,y) -- espacio de pagina, puntos PDF locales a 'nPage' -- o NIL.
   // RefreshRender rehace el render tras una edicion (mismo patron que
   // ZoomIn/ZoomOut: RenderCurrentPage + BuildComposite, sirve para
   // ambos modos sin duplicar logica).
   METHOD HitTestField( nPage, x, y )
   METHOD RefreshRender() INLINE ( ::RenderCurrentPage(), ::BuildComposite() )

   PROTECTED:

   METHOD RenderCurrentPage()                     // logica central: calcula escala segun ::nZoomMode, llama Pdf_RenderToHBitmap
   METHOD ApplyRender( aRender, nScale )           // asigna el render a ::oBmp, ajusta scroll, libera el HBITMAP anterior
   METHOD ShowCurrentFindMatch()                   // posiciona/resalta/scrollea a ::aFindMatches[ ::nFindMatchIdx ]

ENDCLASS

//----------------------------------------------------------------------------//

METHOD New( oWnd, nTop, nLeft, nWidth, nHeight ) CLASS TPdfViewer

   DEFAULT nTop := 0, nLeft := 0, nWidth := 100, nHeight := 100

   ::oWnd        := oWnd
   ::nDispTop    := nTop
   ::nDispLeft   := nLeft
   ::nDispWidth  := nWidth
   ::nDispHeight := nHeight

   // Fondo gris tipo Acrobat: TBitmap:Paint() llena TODA el area no
   // cubierta por el bitmap con ::oWnd:oBrush:hBrush (bitmap.prg:704-724)
   // -- alcanza con poner el brush de la ventana en gris, sin reescribir
   // Paint() a mano. Mismo tono que usa TPreview (source\classes\
   // rpreview.prg) para el "escritorio" del preview de impresion de
   // FWH2603.
   if oWnd != NIL
      oWnd:SetBrush( TBrush():New( , CLR_LIGHTGRAY ) )
   endif

   ::oBmp := TPdfBitmap():New( nTop, nLeft, nWidth, nHeight, ;
                             NIL, NIL, ;     // cResName, cBmpFile: nada que cargar de archivo/recurso
                             .T., ;          // lNoBorder
                             oWnd, ;         // oWnd
                             NIL, NIL, ;     // bLClicked, bRClicked
                             .T., .F., ;     // lScroll (.T. -- scrollbars propios si la pagina no entra), lStretch
                             NIL, NIL, ;     // oCursor, cMsg
                             NIL, NIL, ;     // lUpdate, bWhen
                             .T. )           // lPixel: coordenadas en pixels reales
   ::oBmp:oViewer := Self

   // Rueda del mouse (ver comentario grande en TPdfBitmap:MouseWheel()
   // mas arriba) -- WM_MOUSEWHEEL le llega a oWnd, no al control, hay
   // que delegar a mano.
   if oWnd != NIL
      oWnd:bMouseWheel := {| nKeys, nDelta, nXPos, nYPos | ::oBmp:MouseWheel( nKeys, nDelta, nXPos, nYPos ) }
   endif

return Self

//----------------------------------------------------------------------------//

METHOD Open( cFile, nBudgetMB ) CLASS TPdfViewer

   local pNewDoc

   DEFAULT nBudgetMB := PDFVIEWER_DEFAULT_BUDGET_MB

   pNewDoc := Pdf_Open( cFile, nBudgetMB )
   if pNewDoc == NIL
      return .F.
   endif

   ::Close()                       // cierra el documento anterior (si habia) SOLO despues de confirmar que el nuevo abrio bien

   ::pDoc          := pNewDoc
   ::cFile         := cFile
   ::nPageCount    := Pdf_PageCount( ::pDoc )
   ::nCurPage      := 0
   ::nPageWidthPt  := 0
   ::nPageHeightPt := 0
   ::nUserRotate   := 0            // rotacion es preferencia de vista de ESTE documento, no global -- ver METHOD RotatePage()
   ::ClearFind()                   // una busqueda activa pertenecia al documento ANTERIOR

   if ::nPageCount > 0
      ::GoToPage( 1 )          // deja un fallback de una pagina ya andando ANTES de intentar el compuesto
      ::BuildComposite()       // vista continua -- si algun gate de seguridad frena, ::lContinuousMode queda .F. y sigue en modo de una pagina
   endif

return .T.

//----------------------------------------------------------------------------//

METHOD Close() CLASS TPdfViewer

   if ::pDoc != NIL
      Pdf_Close( ::pDoc )
      ::pDoc := NIL
   endif

   if ::hBitmap != NIL .and. ::hBitmap != 0
      DeleteObject( ::hBitmap )
      ::hBitmap := 0
   endif

   ::cFile         := NIL
   ::nPageCount    := 0
   ::nCurPage      := 0
   ::nPageWidthPt  := 0
   ::nPageHeightPt := 0

return nil

//----------------------------------------------------------------------------//

METHOD GoToPage( nPage ) CLASS TPdfViewer

   if ::oBmp != NIL
      ::oBmp:ClearFormEdit()   // AcroForm: no dejar un TGet de otra pagina flotando encima del bitmap nuevo
   endif

   if ::pDoc == NIL .or. ::nPageCount <= 0
      return .F.
   endif

   if nPage < 1
      nPage := 1
   elseif nPage > ::nPageCount
      nPage := ::nPageCount
   endif

   // Vista continua: "ir a una pagina" es SCROLLEAR dentro del compuesto
   // que ya esta armado -- no hay nada que re-renderizar (el compuesto
   // ya tiene TODAS las paginas). Re-renderizar ademas perderia el
   // compuesto (::ApplyRender lo reemplazaria por una sola pagina).
   // A diferencia del modo de una pagina, NO se limpia la seleccion/
   // resaltado -- scrollear (via Siguiente/Anterior o Find) no tiene por
   // que descartar una seleccion que el usuario dejo hecha en otra
   // pagina del mismo compuesto.
   if ::lContinuousMode
      ::nCurPage := nPage
      if ::oBmp != NIL
         ::oBmp:ScrollToPagePoint( nPage, 0, 0 )
         ::oBmp:Refresh()
      endif
      return .T.
   endif

   if nPage == ::nCurPage .and. ::hBitmap != 0
      return .T.                  // ya estamos en esa pagina con algo ya renderizado -- nada que hacer
   endif

   ::nCurPage      := nPage
   ::nPageWidthPt  := 0            // pagina nueva: invalidar el cache de tamanio en puntos (cada pagina puede tener un MediaBox distinto)
   ::nPageHeightPt := 0

   // los indices de ::aSelRanges/::aHighlight son por-pagina (ver
   // pdf_text_extract.h) -- si sobrevivieran al cambio de pagina, Paint()
   // dibujaria rectangulos en cualquier lugar contra el bitmap de la pagina
   // NUEVA. Sin Refresh() aca: RenderCurrentPage() ya fuerza un repintado
   // completo al asignar el bitmap nuevo (::ApplyRender -> ::oBmp:Refresh()).
   if ::oBmp != NIL
      ::oBmp:aSelRanges := {}
      ::oBmp:aHighlight := {}
   endif

return ::RenderCurrentPage()

//----------------------------------------------------------------------------//

METHOD SetZoomFitHeight() CLASS TPdfViewer
   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif
   ::nZoomMode := PDFVIEWER_ZOOM_FITHEIGHT
   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//

METHOD SetZoom100() CLASS TPdfViewer
   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif
   ::nZoomMode := PDFVIEWER_ZOOM_100
   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//

METHOD SetZoomFitWidth() CLASS TPdfViewer
   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif
   ::nZoomMode := PDFVIEWER_ZOOM_FITWIDTH
   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//
// Zoom +/- (Arturo: "permitiendo ademas hacer zoom + o -"). Al entrar en
// modo custom desde OTRO modo, arranca desde el porcentaje EFECTIVO
// vigente (::nScale*100), no desde el ultimo ::nZoomPercent guardado --
// evita un salto brusco si el usuario venia de "Ajustar"/"Ancho"/"100%"
// con una escala bien distinta a la que haya quedado guardada de una
// sesion de zoom anterior.
//----------------------------------------------------------------------------//

METHOD ZoomIn() CLASS TPdfViewer

   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif

   if ::nZoomMode != PDFVIEWER_ZOOM_CUSTOM
      ::nZoomPercent := Int( ::nScale * 100 )
   endif

   ::nZoomMode    := PDFVIEWER_ZOOM_CUSTOM
   ::nZoomPercent += PDFVIEWER_ZOOM_PERCENT_STEP
   if ::nZoomPercent > PDFVIEWER_ZOOM_PERCENT_MAX
      ::nZoomPercent := PDFVIEWER_ZOOM_PERCENT_MAX
   endif

   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//

METHOD ZoomOut() CLASS TPdfViewer

   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif

   if ::nZoomMode != PDFVIEWER_ZOOM_CUSTOM
      ::nZoomPercent := Int( ::nScale * 100 )
   endif

   ::nZoomMode    := PDFVIEWER_ZOOM_CUSTOM
   ::nZoomPercent -= PDFVIEWER_ZOOM_PERCENT_STEP
   if ::nZoomPercent < PDFVIEWER_ZOOM_PERCENT_MIN
      ::nZoomPercent := PDFVIEWER_ZOOM_PERCENT_MIN
   endif

   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//
// Rotar (Arturo: "necesito un proceso de rotar pagina 90 grados") -- suma
// 90 grados (mod 360) a ::nUserRotate, invalida el cache de tamanio en
// puntos (::nPageWidthPt/HeightPt, igual que GoToPage() -- rotar 90/270
// intercambia ancho/alto, asi que el valor viejo quedaria mal) y vuelve
// a renderizar. Afecta pantalla Y PrintDocument() por igual (mismo
// ::nUserRotate, mismo Pdf_RenderToHBitmap) -- si el usuario endereza la
// vista, tiene sentido que lo impreso tambien salga derecho.
//----------------------------------------------------------------------------//

METHOD RotatePage() CLASS TPdfViewer

   if ::oBmp != NIL ; ::oBmp:ClearFormEdit() ; endif

   ::nUserRotate   := ( ::nUserRotate + 90 ) % 360
   ::nPageWidthPt  := 0
   ::nPageHeightPt := 0

   ::RenderCurrentPage()
return ::BuildComposite()

//----------------------------------------------------------------------------//

METHOD Resize( nWidth, nHeight ) CLASS TPdfViewer

   ::nDispWidth  := nWidth
   ::nDispHeight := nHeight

   if ::oBmp != NIL
      ::oBmp:Move( ::nDispTop, ::nDispLeft, ::nDispWidth, ::nDispHeight, .F. )
   endif

   // En modo continuo NO se reconstruye el compuesto completo en cada
   // evento de resize -- ON RESIZE puede dispararse decenas de veces por
   // segundo mientras se arrastra el borde de la ventana, y
   // BuildComposite() esta pensado para un click de usuario puntual (ver
   // gate de tiempo), no para eso. El compuesto queda con el tamanio de
   // cuando se armo hasta el proximo cambio de zoom manual -- limitacion
   // documentada de esta pasada (agregar debounce si hace falta mas
   // adelante).
   if ! ::lContinuousMode .and. ;
      ( ::nZoomMode == PDFVIEWER_ZOOM_FITHEIGHT .or. ::nZoomMode == PDFVIEWER_ZOOM_FITWIDTH ) .and. ;
      ::pDoc != NIL
      ::RenderCurrentPage()        // el alto/ancho disponible cambio -- recalcular la escala de "ajuste"
   endif

return nil

//----------------------------------------------------------------------------//

METHOD RenderCurrentPage() CLASS TPdfViewer

   local aRender
   local nScale
   local nNeedHeight
   local nNeedWidth

   if ::pDoc == NIL .or. ::nCurPage < 1
      return .F.
   endif

   // Si todavia no conocemos el tamanio de ESTA pagina en puntos PDF, y el
   // modo de zoom no es 100% (que no lo necesita, usa escala fija 1.0), se
   // hace un primer render a escala 1.0 SOLO para conocer el tamanio real de
   // la pagina (ver comentario grande al principio del archivo) -- se
   // aprovecha ese mismo render si el modo pedido resulta ser justamente
   // 100%, para no renderizar dos veces.
   if ( ::nPageWidthPt <= 0 .or. ::nPageHeightPt <= 0 )
      aRender := Pdf_RenderToHBitmap( ::pDoc, ::nCurPage, 1.0, ::nUserRotate )
      if aRender == NIL
         return .F.
      endif
      ::nPageWidthPt  := aRender[ 2 ]
      ::nPageHeightPt := aRender[ 3 ]

      if ::nZoomMode == PDFVIEWER_ZOOM_100
         return ::ApplyRender( aRender, 1.0 )
      endif
   endif

   do case
      case ::nZoomMode == PDFVIEWER_ZOOM_100
         nScale := 1.0

      case ::nZoomMode == PDFVIEWER_ZOOM_FITHEIGHT
         nNeedHeight := ::nDispHeight
         if nNeedHeight < 1
            nNeedHeight := 1
         endif
         nScale := nNeedHeight / ::nPageHeightPt

      case ::nZoomMode == PDFVIEWER_ZOOM_FITWIDTH
         nNeedWidth := ::nDispWidth
         if nNeedWidth < 1
            nNeedWidth := 1
         endif
         nScale := nNeedWidth / ::nPageWidthPt

      case ::nZoomMode == PDFVIEWER_ZOOM_CUSTOM
         nScale := ::nZoomPercent / 100

      otherwise
         nScale := 1.0
   endcase

   if nScale <= 0
      nScale := 1.0
   endif

   aRender := Pdf_RenderToHBitmap( ::pDoc, ::nCurPage, nScale, ::nUserRotate )
   if aRender == NIL
      return .F.
   endif

return ::ApplyRender( aRender, nScale )

//----------------------------------------------------------------------------//

METHOD ApplyRender( aRender, nScale ) CLASS TPdfViewer

   local hOldBitmap := ::hBitmap

   ::hBitmap := aRender[ 1 ]
   ::nScale  := nScale

   if ::oBmp != NIL
      ::oBmp:hBitmap := ::hBitmap
      ::oBmp:ScrollAdjust()        // recalcula rango de scrollbars contra el tamanio real del nuevo bitmap (ver bitmap.prg:1071)
      ::oBmp:Refresh()
   endif

   if hOldBitmap != NIL .and. hOldBitmap != 0 .and. hOldBitmap != ::hBitmap
      DeleteObject( hOldBitmap )   // el HBITMAP anterior es independiente del nuevo (PDF_RENDERTOHBITMAP entrega uno propio en cada llamada) -- liberarlo antes de perder la referencia
   endif

return .T.

//----------------------------------------------------------------------------//
// Vista continua (fase 3 del roadmap de potencialidad MuPDF, DESIGN.md
// seccion 70.1 -- Arturo: "deberia mostrar una hoja con un fondo gris...
// permitiendo hacer zoom... y mostrar hojas continuas"). Arma UN HBITMAP
// compuesto con TODAS las paginas apiladas verticalmente (separacion
// PDFVIEW_PAGE_GAP_PX, margen PDFVIEW_SIDE_MARGIN_PX a los costados,
// cada pagina centrada horizontalmente contra el ancho de la PRIMERA
// pagina) via GDI puro (CreateCompatibleDC/CreateCompatibleBitmap/
// BitBlt, mismo patron que source\function\c5lib.prg:211-228 en
// FWH2603) y lo asigna a ::oBmp:hBitmap -- reusa TODO el mecanismo de
// scroll/Paint() de TBitmap sin cambios, ::nX/::nY simplemente scrollean
// dentro del compuesto como si fuera una pagina mas grande.
//
// Gates de seguridad, del mas barato al mas caro (ver DESIGN.md 70.1):
// pdf_document_get_page recorre el arbol /Pages COMPLETO en cada
// llamada sin cache de objetos -- armar el documento entero sin medir
// primero puede colgar la ventana varios minutos en documentos largos
// o con texto denso. Si CUALQUIER gate no pasa (o si GDI se queda sin
// memoria contigua al armar el compuesto -- proceso de 32 bits sin
// /LARGEADDRESSAWARE, ver win32/Build.bat), ::lContinuousMode queda en
// .F. y el visor sigue funcionando en modo de una pagina (que YA
// arranco antes de llamar aca, ver Open()) -- degradacion con gracia,
// no un modo roto.
//----------------------------------------------------------------------------//

METHOD BuildComposite() CLASS TPdfViewer

   local nPage
   local aRender
   local tStart, tPage1
   local nPageW, nPageH
   local nCompWidth, nCompHeight
   local hDCScreen, hDCMem, hBmpComposite, hDCPage
   local hBmpOldMem, hBmpOldPage
   local oBrushGray
   local nYOff, nXOff
   local hOldComposite

   ::lContinuousMode := .F.

   PdfViewLogDebug( "BuildComposite: arranca, nPageCount=" + hb_ntos( ::nPageCount ) + " nScale=" + Str( ::nScale, 10, 4 ) )

   if ::pDoc == NIL .or. ::nPageCount < 1
      PdfViewLogDebug( "BuildComposite: aborta, pDoc==NIL o nPageCount<1" )
      return .F.
   endif

   // Gate 1: cantidad de paginas -- O(1) (Pdf_PageCount ya lee /Count),
   // antes de renderizar nada.
   if ::nPageCount > PDFVIEW_CONTINUOUS_MAX_PAGES
      PdfViewLogDebug( "BuildComposite: GATE 1 (paginas) frena, " + hb_ntos( ::nPageCount ) + " > " + hb_ntos( PDFVIEW_CONTINUOUS_MAX_PAGES ) )
      return .F.
   endif
   PdfViewLogDebug( "BuildComposite: gate 1 OK, llamando Pdf_RenderToHBitmap pagina 1..." )

   tStart  := Seconds()
   aRender := Pdf_RenderToHBitmap( ::pDoc, 1, ::nScale, ::nUserRotate )
   if aRender == NIL
      PdfViewLogDebug( "BuildComposite: Pdf_RenderToHBitmap(pagina 1) devolvio NIL" )
      return .F.
   endif
   tPage1 := Seconds() - tStart
   if tPage1 <= 0
      tPage1 := 0.05           // Seconds() puede dar 0 en una pagina muy rapida -- no dividir/proyectar con 0
   endif

   nPageW := aRender[ 2 ]
   nPageH := aRender[ 3 ]
   PdfViewLogDebug( "BuildComposite: render pagina 1 OK, tiempo=" + Str( tPage1, 10, 3 ) + "s ancho=" + hb_ntos( nPageW ) + " alto=" + hb_ntos( nPageH ) )

   // Gate 2: proyeccion de tiempo -- el mas determinante (ver DESIGN.md
   // 70.1: render de texto denso puede costar segundos por pagina en
   // este motor).
   if tPage1 * ::nPageCount > PDFVIEW_CONTINUOUS_MAX_SECONDS
      PdfViewLogDebug( "BuildComposite: GATE 2 (tiempo) frena, proyeccion=" + Str( tPage1 * ::nPageCount, 10, 3 ) + "s > " + Str( PDFVIEW_CONTINUOUS_MAX_SECONDS, 10, 3 ) + "s" )
      DeleteObject( aRender[ 1 ] )
      return .F.
   endif

   // Gate 3: dimensiones del compuesto -- limite practico de
   // SetScrollRange/SetScrollPos (pierden precision pasado ~32767).
   nCompWidth  := nPageW + 2 * PDFVIEW_SIDE_MARGIN_PX
   nCompHeight := ::nPageCount * ( nPageH + PDFVIEW_PAGE_GAP_PX ) + PDFVIEW_PAGE_GAP_PX
   if nCompWidth > PDFVIEW_CONTINUOUS_MAX_DIM_PX .or. nCompHeight > PDFVIEW_CONTINUOUS_MAX_DIM_PX
      PdfViewLogDebug( "BuildComposite: GATE 3 (dimensiones) frena, " + hb_ntos( nCompWidth ) + "x" + hb_ntos( nCompHeight ) )
      DeleteObject( aRender[ 1 ] )
      return .F.
   endif

   // Gate 4: presupuesto de memoria, proyectando TODAS las paginas del
   // tamanio de la primera (no se conoce el resto todavia).
   if ::nPageCount * nPageW * nPageH * 3 > PDFVIEW_CONTINUOUS_MAX_BYTES
      PdfViewLogDebug( "BuildComposite: GATE 4 (memoria) frena, " + hb_ntos( Int( ::nPageCount * nPageW * nPageH * 3 / ( 1024 * 1024 ) ) ) + "MB" )
      DeleteObject( aRender[ 1 ] )
      return .F.
   endif

   PdfViewLogDebug( "BuildComposite: los 4 gates OK, armando compuesto " + hb_ntos( nCompWidth ) + "x" + hb_ntos( nCompHeight ) + "..." )

   // Paso los 4 gates -- arma el compuesto. Cursor de espera: aunque
   // pasaron los gates, sigue siendo un render de N paginas, puede
   // tardar un par de segundos y sin esto se ve como colgada.
   CursorWait()

   hDCScreen     := GetDC( 0 )
   hDCMem        := CreateCompatibleDC( hDCScreen )
   hBmpComposite := CreateCompatibleBitmap( hDCScreen, nCompWidth, nCompHeight )

   if hBmpComposite == NIL .or. hBmpComposite == 0
      // Fallo de asignacion GDI -- caso NORMAL a esperar (ver comentario
      // grande arriba), no una excepcion. Limpiar y caer al modo de una
      // pagina que ya esta andando.
      PdfViewLogDebug( "BuildComposite: CreateCompatibleBitmap fallo (memoria GDI)" )
      DeleteDC( hDCMem )
      ReleaseDC( 0, hDCScreen )
      DeleteObject( aRender[ 1 ] )
      CursorArrow()
      return .F.
   endif

   hBmpOldMem := SelectObject( hDCMem, hBmpComposite )

   oBrushGray := TBrush():New( , CLR_LIGHTGRAY )
   FillRect( hDCMem, { 0, 0, nCompHeight, nCompWidth }, oBrushGray:hBrush )
   oBrushGray:End()

   ::aPageOffsetY  := Array( ::nPageCount )
   ::aPageOffsetX  := Array( ::nPageCount )
   ::aPageWidthPx  := Array( ::nPageCount )
   ::aPageHeightPx := Array( ::nPageCount )

   hDCPage := CreateCompatibleDC( hDCScreen )

   for nPage := 1 to ::nPageCount
      if nPage > 1
         aRender := Pdf_RenderToHBitmap( ::pDoc, nPage, ::nScale, ::nUserRotate )
      endif

      nYOff := ( nPage - 1 ) * ( nPageH + PDFVIEW_PAGE_GAP_PX ) + PDFVIEW_PAGE_GAP_PX
      nXOff := PDFVIEW_SIDE_MARGIN_PX

      if aRender != NIL
         nXOff := PDFVIEW_SIDE_MARGIN_PX + Int( ( nPageW - aRender[ 2 ] ) / 2 )
         if nXOff < 0
            nXOff := 0
         endif

         // BitBlt( hDestDC, nDestX, nDestY, nWidth, nHeight, hSrcDC,
         // nSrcX, nSrcY, nRop ) -- X ANTES que Y (no es la convencion
         // {top,left,bottom,right} de FillRect/InvertRect de mas arriba).
         hBmpOldPage := SelectObject( hDCPage, aRender[ 1 ] )
         BitBlt( hDCMem, nXOff, nYOff, aRender[ 2 ], aRender[ 3 ], hDCPage, 0, 0, SRCCOPY )
         SelectObject( hDCPage, hBmpOldPage )
         DeleteObject( aRender[ 1 ] )

         ::aPageWidthPx[ nPage ]  := aRender[ 2 ]
         ::aPageHeightPx[ nPage ] := aRender[ 3 ]
      else
         // pagina puntual no se pudo renderizar (raro) -- deja un hueco
         // gris del tamanio asumido (::aPageWidthPx/HeightPx igual se
         // llenan, para que la conversion de coordenadas no se rompa).
         ::aPageWidthPx[ nPage ]  := nPageW
         ::aPageHeightPx[ nPage ] := nPageH
      endif

      ::aPageOffsetY[ nPage ] := nYOff
      ::aPageOffsetX[ nPage ] := nXOff

      aRender := NIL
   next

   DeleteDC( hDCPage )
   SelectObject( hDCMem, hBmpOldMem )
   DeleteDC( hDCMem )
   ReleaseDC( 0, hDCScreen )

   CursorArrow()

   hOldComposite     := ::hBitmap
   ::hBitmap         := hBmpComposite
   ::lContinuousMode := .T.

   if ::oBmp != NIL
      ::oBmp:hBitmap := ::hBitmap
      ::oBmp:ScrollAdjust()
      ::oBmp:Refresh()
   endif

   if hOldComposite != NIL .and. hOldComposite != 0 .and. hOldComposite != ::hBitmap
      DeleteObject( hOldComposite )
   endif

   PdfViewLogDebug( "BuildComposite: EXITO, lContinuousMode=.T." )

return .T.

//----------------------------------------------------------------------------//
// Busqueda binaria: en que pagina cae el pixel-fila 'nOffsetY' del
// compuesto (::aPageOffsetY[nPage] es el TOPE de esa pagina -- devuelve
// la ULTIMA pagina cuyo tope es <= nOffsetY). Fuera de modo continuo
// devuelve directo ::nCurPage (no hay compuesto que buscar).
//----------------------------------------------------------------------------//

METHOD PageAtOffsetY( nOffsetY ) CLASS TPdfViewer

   local nLo := 1, nHi := ::nPageCount, nMid

   if ::nPageCount < 1
      return 0
   endif
   if ! ::lContinuousMode
      return ::nCurPage
   endif

   while nLo < nHi
      nMid := Int( ( nLo + nHi + 1 ) / 2 )
      if ::aPageOffsetY[ nMid ] <= nOffsetY
         nLo := nMid
      else
         nHi := nMid - 1
      endif
   enddo

return nLo

//----------------------------------------------------------------------------//
// Tamanio de 'nPage' en puntos PDF -- en modo continuo, a partir del
// tamanio en PIXELS que se midio al armar el compuesto
// (aPageWidthPx/aPageHeightPx) dividido por ::nScale (mas preciso que
// asumir el tamanio de la pagina 1 para TODAS, ya usa el tamanio REAL
// de esa pagina puntual si vino distinto). Fuera de modo continuo (o si
// el indice no esta poblado todavia) cae a ::nPageWidthPt/HeightPt (la
// pagina actual, unico dato disponible en ese modo).
//----------------------------------------------------------------------------//

METHOD PageSizePt( nPage ) CLASS TPdfViewer

   local nScaleSafe := if( ::nScale > 0, ::nScale, 1.0 )

   if ::lContinuousMode .and. nPage >= 1 .and. nPage <= Len( ::aPageWidthPx )
      return { ::aPageWidthPx[ nPage ] / nScaleSafe, ::aPageHeightPx[ nPage ] / nScaleSafe }
   endif

return { ::nPageWidthPt, ::nPageHeightPt }

//----------------------------------------------------------------------------//
// Busqueda (Etapa 5 del roadmap de potencialidad MuPDF, ver DESIGN.md
// seccion 70) -- Find() arranca desde la pagina actual, FindNext() avanza
// (dentro de la pagina si quedan mas matches cacheados, si no a la
// siguiente pagina que tenga alguno) con wrap al llegar al final. El PDF
// completo se recorre pagina por pagina via Pdf_FindText (que ya usa el
// cache de 4 slots de pdf_hbfunc.c -- reabrir la busqueda sobre paginas ya
// visitadas no vuelve a correr el content stream).
//----------------------------------------------------------------------------//

METHOD Find( cNeedle, lCaseSensitive ) CLASS TPdfViewer

   DEFAULT lCaseSensitive := .F.

   // BUG REAL ENCONTRADO (Arturo: arrastre de seleccion entre paginas
   // "queda pegado" en pantalla al usar Buscar despues) -- ::ClearSelection()
   // solo se disparaba desde LButtonDown (un click NUEVO sobre la
   // pagina) -- clickear el campo de busqueda o el boton "Buscar" no
   // toca el control del bitmap para nada, asi que la seleccion vieja
   // se quedaba dibujada por Paint() indefinidamente. Una busqueda
   // nueva reemplaza logicamente cualquier seleccion vigente.
   ::ClearSelection()

   if ::pDoc == NIL .or. Empty( cNeedle )
      ::ClearFind()
      return .F.
   endif

   ::cFindNeedle         := cNeedle
   ::lFindCaseSensitive  := lCaseSensitive
   ::nFindPage           := ::nCurPage     // FindNext() arranca buscando EN esta pagina primero
   ::aFindMatches         := {}
   ::nFindMatchIdx        := 0

return ::FindNext()

//----------------------------------------------------------------------------//

METHOD FindNext() CLASS TPdfViewer

   local nPage, nTries

   if ::pDoc == NIL .or. Empty( ::cFindNeedle )
      return .F.
   endif

   // si la pagina cacheada todavia tiene mas matches sin mostrar, avanzar
   // ahi dentro antes de pasar de pagina.
   if ::nFindMatchIdx > 0 .and. ::nFindMatchIdx < Len( ::aFindMatches )
      ::nFindMatchIdx++
      return ::ShowCurrentFindMatch()
   endif

   // BUG REAL ENCONTRADO (Arturo: "funciona solo en una pagina, deberia
   // continuar en todas las paginas"): la version anterior siempre
   // arrancaba el escaneo EN ::nFindPage, incluso cuando esa pagina ya
   // estaba agotada (todos sus matches ya mostrados) -- como Pdf_FindText
   // vuelve a encontrar los MISMOS matches de esa pagina, el "avance" caia
   // siempre de nuevo ahi mismo antes de llegar a incrementar. Distincion
   // correcta: si ::nFindMatchIdx > 0 es porque YA se mostro al menos un
   // match de ::nFindPage en una llamada anterior -> esa pagina esta
   // agotada, arrancar en la SIGUIENTE. Si es 0 (recien llamado desde
   // Find(), primera busqueda), arrancar EN la pagina actual.
   if ::nFindMatchIdx > 0
      nPage := ::nFindPage + 1
      if nPage > ::nPageCount
         nPage := 1
      endif
   else
      nPage := if( ::nFindPage >= 1, ::nFindPage, ::nCurPage )
   endif

   // nPageCount iteraciones = como mucho una vuelta completa al documento
   // (visita cada pagina exactamente una vez; si se arranco en nFindPage+1,
   // la ULTIMA iteracion vuelve a caer en nFindPage -- a proposito, para
   // que un unico match en todo el documento haga "wrap" mostrandose de
   // nuevo en vez de reportar "no hay mas" sin necesidad).
   for nTries := 1 to ::nPageCount
      ::aFindMatches := Pdf_FindText( ::pDoc, nPage, ::cFindNeedle, ::lFindCaseSensitive )

      if Len( ::aFindMatches ) > 0
         ::nFindPage     := nPage
         ::nFindMatchIdx := 1
         return ::ShowCurrentFindMatch()
      endif

      nPage++
      if nPage > ::nPageCount
         nPage := 1
      endif
   next

   ::aFindMatches  := {}
   ::nFindMatchIdx := 0
   if ::oBmp != NIL
      ::oBmp:aHighlight := {}
      ::oBmp:Refresh()
   endif

return .F.

//----------------------------------------------------------------------------//

METHOD ShowCurrentFindMatch() CLASS TPdfViewer

   local aMatch

   if ::nFindMatchIdx < 1 .or. ::nFindMatchIdx > Len( ::aFindMatches )
      return .F.
   endif

   aMatch := ::aFindMatches[ ::nFindMatchIdx ]

   if ::nFindPage != ::nCurPage
      ::GoToPage( ::nFindPage )       // modo continuo: scroll puro. Modo una pagina: re-renderiza (y limpia ::oBmp:aHighlight -- se vuelve a setear abajo)
   endif

   if ::oBmp != NIL
      ::oBmp:aHighlight := { { ::nFindPage, aMatch[ 1 ], aMatch[ 2 ], aMatch[ 3 ], aMatch[ 4 ], aMatch[ 5 ], aMatch[ 6 ] } }
      ::oBmp:ScrollToPagePoint( ::nFindPage, aMatch[ 3 ], aMatch[ 4 ] )
      ::oBmp:Refresh()
   endif

return .T.

//----------------------------------------------------------------------------//

METHOD ClearFind() CLASS TPdfViewer

   ::cFindNeedle    := NIL
   ::aFindMatches   := {}
   ::nFindMatchIdx  := 0
   ::nFindPage      := 0

   if ::oBmp != NIL .and. Len( ::oBmp:aHighlight ) > 0
      ::oBmp:aHighlight := {}
      ::oBmp:Refresh()
   endif

return nil

//----------------------------------------------------------------------------//
// Impresion (Arturo: "un proceso para lanzar el documento a impresion").
//
// BUG REAL ENCONTRADO Y ARREGLADO (version anterior via TPrinter():New()
// + SayBitmap() -- Arturo probo y "no imprime nada", sin ningun error
// visible): SayBitmap() trata un ::hBitmap NUMERICO como un ID DE
// RECURSO compilado en el .exe (llama PalBmpLoad(xBitmap), que a su
// vez es SIEMPRE FindResource(..., MAKEINTRESOURCE(xBitmap), ...) --
// confirmado leyendo fwbmp.c de FWH2603) -- NUNCA como un HBITMAP real
// en memoria. El HBITMAP que devuelve Pdf_RenderToHBitmap no es
// ningun recurso del .exe, asi que FindResource fallaba en silencio,
// PalBmpLoad devolvia {0,0}, y SayBitmap no dibujaba NADA -- pero
// como ::hDC, StartPage()/EndPage() (adentro de TPrinter) SI eran
// validos, todo lo demas parecia "funcionar" (confirmado con
// PdfViewLogDebug: 10/10 paginas "mandadas", pero en realidad vacias).
//
// Fix, siguiendo el ejemplo real que paso Arturo (otra app propia que
// SI imprime, sin TPrinter): WinAPI crudo, expuesto directo por FWH2603
// sin pasar por la clase TPrinter -- GetPrintDC()/StartDoc()/StartPage()/
// EndPage()/EndDoc()/DeleteDC() (todos HB_FUNC globales, confirmado en
// printdc.c) mas DibFromBitmap(hBitmap,hPalette)/DibDraw() (winapi/
// dibbmp.c y dib.c) para el blit -- DibFromBitmap SI toma un HBITMAP
// real (no un ID de recurso). Se chequea el codigo de retorno de CADA
// paso (StartDoc/StartPage/EndPage) y se loguea -- la version anterior
// nunca miraba estos retornos, por eso una falla real hubiera quedado
// tan invisible como el bug de SayBitmap.
//
// Dialogo de paginas: en vez de un dialogo propio (la primera version
// tenia un bug de layout ademas), se usa el dialogo NATIVO de Windows
// con su radio "Todas/Paginas desde-hasta" -- Arturo encontro (y paso
// el codigo real) que GetPrintDC(hWnd,bSel,bPage) con bPage=.T. A MANO
// lo ofrece siempre de forma confiable; el problema real de la v1 no
// era el dialogo nativo en si, sino que TPrinter():New() arma ese
// mismo bPage leyendo PrnGetPagNums() (estado global de la ULTIMA
// llamada, no de la que esta por abrirse) en vez de pasar .T. directo.
// PrnGetPages() (--> {nFrom,nTo}) y PrnGetPagNums() (--> eligio
// "Paginas" en vez de "Todas") leen el resultado despues de cerrado.
//
// 300 DPI fijo por pagina (elegido por Arturo, mas nitido que 150 a
// costa de mas tiempo/memoria) -- el bitmap se estira con DibDraw al
// tamanio real en PIXELES DE IMPRESORA (GetDeviceCaps LOGPIXELSX/Y,
// que casi nunca es 300 -- una laser tipica es 600/1200), no en
// pulgadas: nPxImpresora = nPxPdf(a 300 DPI) * dpiImpresora / 300.
//----------------------------------------------------------------------------//

METHOD PrintDocument() CLASS TPdfViewer

   local hDC
   local aPages, lPageNums, nFrom, nTo, nPage
   local nDpiX, nDpiY
   local aRender, hBmp, nPxW, nPxH, nTargetW, nTargetH
   local nPrintScale := 300.0 / 72.0
   local nRet, hDib
   local nPaginasImpresas := 0

   if ::pDoc == NIL .or. ::nPageCount <= 0
      MsgStop( "No hay ningun documento abierto.", "Imprimir" )
      return .F.
   endif

   hDC := GetPrintDC( ::oWnd:hWnd, .F., .T. )    // bSel=.F. (no aplica "seleccion"), bPage=.T. A MANO -- ver comentario grande arriba
   PdfViewLogDebug( "PrintDocument: GetPrintDC hDC=" + hb_ntos( hDC ) )
   if hDC == 0
      return .F.                            // cancelado en el dialogo, o sin impresora instalada
   endif

   aPages    := PrnGetPages()                // {nFrom, nTo} -- solo confiable si lPageNums es .T.
   lPageNums := PrnGetPagNums()

   if lPageNums .and. aPages[ 1 ] > 0 .and. aPages[ 2 ] > 0
      nFrom := aPages[ 1 ]
      nTo   := aPages[ 2 ]
   else
      nFrom := 1                            // "Todas" (o el checkbox no se toco)
      nTo   := ::nPageCount
   endif
   if nFrom < 1 ; nFrom := 1 ; endif
   if nTo > ::nPageCount ; nTo := ::nPageCount ; endif
   if nFrom > nTo ; nTo := nFrom ; endif
   PdfViewLogDebug( "PrintDocument: rango " + hb_ntos( nFrom ) + ".." + hb_ntos( nTo ) + " (lPageNums=" + hb_ntos( iif( lPageNums, 1, 0 ) ) + ")" )

   nRet := StartDoc( hDC, "PDFEngine32" )
   PdfViewLogDebug( "PrintDocument: StartDoc devolvio " + hb_ntos( nRet ) )
   if nRet <= 0
      MsgStop( "No se pudo iniciar el trabajo de impresion (StartDoc devolvio " + hb_ntos( nRet ) + ").", "Imprimir" )
      DeleteDC( hDC )
      return .F.
   endif

   nDpiX := GetDeviceCaps( hDC, LOGPIXELSX )
   nDpiY := GetDeviceCaps( hDC, LOGPIXELSY )
   PdfViewLogDebug( "PrintDocument: impresora DPI=" + hb_ntos( nDpiX ) + "x" + hb_ntos( nDpiY ) )

   CursorWait()

   for nPage := nFrom to nTo
      aRender := Pdf_RenderToHBitmap( ::pDoc, nPage, nPrintScale, ::nUserRotate )
      if aRender == NIL
         PdfViewLogDebug( "PrintDocument: pagina " + hb_ntos( nPage ) + " Pdf_RenderToHBitmap devolvio NIL -- se salta" )
      else
         hBmp := aRender[ 1 ]
         nPxW := aRender[ 2 ]
         nPxH := aRender[ 3 ]

         nTargetW := Int( nPxW * nDpiX / 300 )
         nTargetH := Int( nPxH * nDpiY / 300 )

         nRet := StartPage( hDC )
         PdfViewLogDebug( "PrintDocument: pagina " + hb_ntos( nPage ) + " StartPage=" + hb_ntos( nRet ) + ;
            " pxPdf=" + hb_ntos( nPxW ) + "x" + hb_ntos( nPxH ) + " pxImpresora=" + hb_ntos( nTargetW ) + "x" + hb_ntos( nTargetH ) )

         if nRet > 0
            hDib := DibFromBitmap( hBmp, 0 )       // 0 = sin paleta (bitmap de 24 bits, sin indexado)
            if hDib == 0
               PdfViewLogDebug( "PrintDocument: pagina " + hb_ntos( nPage ) + " DibFromBitmap devolvio 0" )
            else
               DibDraw( hDC, hDib, 0, 0, 0, nTargetW, nTargetH, )
               GlobalFree( hDib )
            endif

            nRet := EndPage( hDC )
            PdfViewLogDebug( "PrintDocument: pagina " + hb_ntos( nPage ) + " EndPage=" + hb_ntos( nRet ) )
            if nRet > 0
               nPaginasImpresas++
            endif
         endif

         DeleteObject( hBmp )        // Pdf_RenderToHBitmap entrega un HBITMAP propio en cada llamada, ver ApplyRender()
      endif
   next

   EndDoc( hDC )
   DeleteDC( hDC )
   CursorArrow()

   PdfViewLogDebug( "PrintDocument: FIN, " + hb_ntos( nPaginasImpresas ) + " de " + hb_ntos( nTo - nFrom + 1 ) + " paginas OK" )
   MsgInfo( hb_ntos( nPaginasImpresas ) + " de " + hb_ntos( nTo - nFrom + 1 ) + " pagina(s) mandada(s) a la impresora.", "Imprimir" )

return .T.

//----------------------------------------------------------------------------//
// AcroForm: fila de Pdf_FormListFields (ver pdf_hbfunc.c) cuyo /Rect
// contiene (x,y) -- espacio de pagina, puntos PDF locales a 'nPage'.
// Se saltan los campos de solo lectura y los de tipo "otro" (radio/
// combo/lista/firma -- no editables en esta etapa, ver DESIGN.md). NIL
// si no hay match o si el documento no tiene AcroForm.
//----------------------------------------------------------------------------//

METHOD HitTestField( nPage, x, y ) CLASS TPdfViewer

   local aFields, aF, i

   if ::pDoc == NIL .or. nPage < 1
      return NIL
   endif

   aFields := Pdf_FormListFields( ::pDoc, nPage )

   for i := 1 to Len( aFields )
      aF := aFields[ i ]
      if aF[ 8 ]                                  // lReadOnly
         loop
      endif
      if aF[ 1 ] != 1 .and. aF[ 1 ] != 2           // ni texto ni checkbox
         loop
      endif
      if x >= aF[ 3 ] .and. x <= aF[ 5 ] .and. ;
         y >= aF[ 4 ] .and. y <= aF[ 6 ]
         return aF
      endif
   next

return NIL
