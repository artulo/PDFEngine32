/* test_composite_gates.prg -- diagnostico de consola (sin GUI) para
 * entender por que BuildComposite() (pdf_viewer.prg, vista continua)
 * no esta entrando en modo continuo contra un documento real. Imprime
 * Pdf_PageCount(), el tiempo de Pdf_RenderToHBitmap() de la pagina 1,
 * y la proyeccion contra los mismos umbrales que usa BuildComposite()
 * (PDFVIEW_CONTINUOUS_MAX_*, ver pdf_viewer.prg) para ver cual gate
 * (si alguno) esta frenando. */

REQUEST HB_GT_STD_DEFAULT

#define PDFVIEW_CONTINUOUS_MAX_PAGES    150
#define PDFVIEW_CONTINUOUS_MAX_SECONDS  6.0
#define PDFVIEW_CONTINUOUS_MAX_BYTES    (180 * 1024 * 1024)
#define PDFVIEW_CONTINUOUS_MAX_DIM_PX   30000
#define PDFVIEW_PAGE_GAP_PX             12
#define PDFVIEW_SIDE_MARGIN_PX          16

PROCEDURE Main( cFile )

   LOCAL pDoc, nPageCount, aRender, tStart, tPage1, nPageW, nPageH
   LOCAL nCompWidth, nCompHeight, nProjBytes

   IF Empty( cFile )
      cFile := "..\tests\Conveyor_Handbook.pdf"
   ENDIF

   ? "Abriendo:", cFile
   pDoc := Pdf_Open( cFile, 192 )
   IF pDoc == NIL
      ? "*** PDF_OPEN devolvio NIL ***"
      RETURN
   ENDIF

   nPageCount := Pdf_PageCount( pDoc )
   ? "Pdf_PageCount:", nPageCount

   IF nPageCount > PDFVIEW_CONTINUOUS_MAX_PAGES
      ? "*** GATE 1 (cantidad de paginas) FRENA:", nPageCount, ">", PDFVIEW_CONTINUOUS_MAX_PAGES
      Pdf_Close( pDoc )
      RETURN
   ENDIF
   ? "Gate 1 (cantidad de paginas) OK"

   tStart := Seconds()
   aRender := Pdf_RenderToHBitmap( pDoc, 1, 1.0 )
   tPage1 := Seconds() - tStart
   IF aRender == NIL
      ? "*** Pdf_RenderToHBitmap(pagina 1) devolvio NIL ***"
      Pdf_Close( pDoc )
      RETURN
   ENDIF
   IF tPage1 <= 0
      tPage1 := 0.05
   ENDIF
   nPageW := aRender[ 2 ]
   nPageH := aRender[ 3 ]
   ? "Render pagina 1: tiempo=" + Str( tPage1, 10, 3 ) + "s", "ancho=" + hb_ntos( nPageW ), "alto=" + hb_ntos( nPageH )

   ? "Proyeccion tiempo total:", Str( tPage1 * nPageCount, 10, 3 ), "vs limite", PDFVIEW_CONTINUOUS_MAX_SECONDS
   IF tPage1 * nPageCount > PDFVIEW_CONTINUOUS_MAX_SECONDS
      ? "*** GATE 2 (proyeccion de tiempo) FRENA ***"
   ELSE
      ? "Gate 2 (proyeccion de tiempo) OK"
   ENDIF

   nCompWidth  := nPageW + 2 * PDFVIEW_SIDE_MARGIN_PX
   nCompHeight := nPageCount * ( nPageH + PDFVIEW_PAGE_GAP_PX ) + PDFVIEW_PAGE_GAP_PX
   ? "Compuesto proyectado:", hb_ntos( nCompWidth ) + "x" + hb_ntos( nCompHeight ), "px, vs limite", PDFVIEW_CONTINUOUS_MAX_DIM_PX
   IF nCompWidth > PDFVIEW_CONTINUOUS_MAX_DIM_PX .OR. nCompHeight > PDFVIEW_CONTINUOUS_MAX_DIM_PX
      ? "*** GATE 3 (dimensiones) FRENA ***"
   ELSE
      ? "Gate 3 (dimensiones) OK"
   ENDIF

   nProjBytes := nPageCount * nPageW * nPageH * 3
   ? "Bytes proyectados:", hb_ntos( Int( nProjBytes / ( 1024 * 1024 ) ) ) + "MB", "vs limite", hb_ntos( Int( PDFVIEW_CONTINUOUS_MAX_BYTES / ( 1024 * 1024 ) ) ) + "MB"
   IF nProjBytes > PDFVIEW_CONTINUOUS_MAX_BYTES
      ? "*** GATE 4 (presupuesto de memoria) FRENA ***"
   ELSE
      ? "Gate 4 (presupuesto de memoria) OK"
   ENDIF

   Pdf_Close( pDoc )
   ? "*** FIN ***"

RETURN
