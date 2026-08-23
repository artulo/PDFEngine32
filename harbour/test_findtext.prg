/* test_findtext.prg -- prueba de consola (sin FiveWin/GUI) del
 * marshalling C<->Harbour de la Etapa 3 (bindings de texto, ver
 * pdf_hbfunc.c y el plan de la Fase 2 del roadmap de potencialidad
 * MuPDF). Abre un PDF sintetico y ejercita PDF_EXTRACTTEXT/
 * PDF_FINDTEXT/PDF_GLYPHSINRECT/PDF_GETGLYPHTEXT directo, sin pasar
 * por TPdfViewer -- aisla bugs de marshalling C<->Harbour de bugs de
 * UI (Etapa 4/5, todavia no implementadas). */

REQUEST HB_GT_STD_DEFAULT

PROCEDURE Main( cFile )

   LOCAL pDoc, nGlyphs, aMatches, aRanges, cText, i

   IF Empty( cFile )
      cFile := "search_test.pdf"
   ENDIF

   ? "Abriendo:", cFile
   pDoc := Pdf_Open( cFile, 0 )
   IF pDoc == NIL
      ? "*** PDF_OPEN devolvio NIL ***"
      RETURN
   ENDIF
   ? "PDF_PAGECOUNT:", Pdf_PageCount( pDoc )

   nGlyphs := Pdf_ExtractText( pDoc, 1 )
   ? "PDF_EXTRACTTEXT(pagina 1):", nGlyphs, "glyphs"
   IF nGlyphs != 25
      ? "*** ESPERADO 25 glyphs ('The quick brown fox jumps') ***"
   ENDIF

   aMatches := Pdf_FindText( pDoc, 1, "quick", .T. )
   ? "PDF_FINDTEXT('quick'):", Len( aMatches ), "match(es)"
   FOR i := 1 TO Len( aMatches )
      ? "  ", "start=" + hb_ntos( aMatches[ i ][ 1 ] ), ;
              "end=" + hb_ntos( aMatches[ i ][ 2 ] ), ;
              "x0=" + hb_ntos( aMatches[ i ][ 3 ] ), ;
              "y0=" + hb_ntos( aMatches[ i ][ 4 ] ), ;
              "x1=" + hb_ntos( aMatches[ i ][ 5 ] ), ;
              "y1=" + hb_ntos( aMatches[ i ][ 6 ] )
   NEXT
   IF Len( aMatches ) != 1 .OR. aMatches[ 1 ][ 1 ] != 5 .OR. aMatches[ 1 ][ 2 ] != 9
      ? "*** ESPERADO 1 match, glyphs 5..9 (1-based) ***"
   ENDIF

   aMatches := Pdf_FindText( pDoc, 1, "QUICK", .F. )
   ? "PDF_FINDTEXT('QUICK', case-insensitive):", Len( aMatches ), "match(es)"
   IF Len( aMatches ) != 1
      ? "*** ESPERADO 1 match case-insensitive ***"
   ENDIF

   aMatches := Pdf_FindText( pDoc, 1, "elephant", .T. )
   ? "PDF_FINDTEXT('elephant'):", Len( aMatches ), "match(es) (esperado 0)"

   /* rectangulo que cubre toda la linea (y0_px=92, altura ~12) */
   aRanges := Pdf_GlyphsInRect( pDoc, 1, 0, 80, 300, 100 )
   ? "PDF_GLYPHSINRECT(0,80,300,100):", Len( aRanges ), "rango(s)"
   FOR i := 1 TO Len( aRanges )
      ? "  ", "start=" + hb_ntos( aRanges[ i ][ 1 ] ), "end=" + hb_ntos( aRanges[ i ][ 2 ] )
   NEXT
   IF Len( aRanges ) != 1 .OR. aRanges[ 1 ][ 1 ] != 1 .OR. aRanges[ 1 ][ 2 ] != 25
      ? "*** ESPERADO 1 rango cubriendo los 25 glyphs (1..25) ***"
   ENDIF

   cText := Pdf_GetGlyphText( pDoc, 1, aRanges )
   ? "PDF_GETGLYPHTEXT:", "[" + cText + "]"
   IF cText != "The quick brown fox jumps"
      ? "*** TEXTO NO COINCIDE ***"
   ENDIF

   /* rectangulo vacio (fuera de cualquier glyph) */
   aRanges := Pdf_GlyphsInRect( pDoc, 1, 0, 500, 50, 550 )
   ? "PDF_GLYPHSINRECT (fuera de rango):", Len( aRanges ), "rango(s) (esperado 0)"

   Pdf_Close( pDoc )
   ? "*** FIN ***"

RETURN
