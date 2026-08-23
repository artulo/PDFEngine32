/* test_between.prg -- prueba de consola (sin FiveWin/GUI) del fix
 * "seleccion por secuencia de texto" (Pdf_GlyphsBetweenPoints, ver
 * pdf_hbfunc.c y DESIGN.md seccion 70): Arturo reporto que copiar una
 * seleccion multi-linea traia texto "al azar" en vez de seguir letras/
 * palabras -- la causa era que Pdf_GlyphsInRect selecciona por
 * interseccion GEOMETRICA de rectangulo (recorta cada linea a la misma
 * columna de X). Este test verifica contra multiline_test.pdf (3
 * lineas conocidas) que arrastrar de la mitad de la linea 1 a la mitad
 * de la linea 3 trae el RESTO de la linea 1 + TODA la linea 2 + el
 * PRINCIPIO de la linea 3, en el orden de lectura correcto. */

REQUEST HB_GT_STD_DEFAULT

PROCEDURE Main( cFile )

   LOCAL pDoc, aRanges, cText, i

   IF Empty( cFile )
      cFile := "multiline_test.pdf"
   ENDIF

   ? "Abriendo:", cFile
   pDoc := Pdf_Open( cFile, 0 )
   IF pDoc == NIL
      ? "*** PDF_OPEN devolvio NIL ***"
      RETURN
   ENDIF

   Pdf_ExtractText( pDoc, 1 )

   /* mismos puntos de consulta que test_nearest_glyph.c (glyph 0-based
    * indice 6, x=93.36 y=92 -- 2da 'B' de BBBB en la linea 1; glyph
    * 0-based indice 34, x=99.34 y=132 -- 2da 'H' de HHHH en la linea 3),
    * apenas corridos +1 en X como alli. En Harbour (1-based) eso mapea a
    * los indices 7 y 35. */
   aRanges := Pdf_GlyphsBetweenPoints( pDoc, 1, 94.36, 92, 100.34, 132 )
   ? "PDF_GLYPHSBETWEENPOINTS:", Len( aRanges ), "rango(s) (uno por linea)"
   FOR i := 1 TO Len( aRanges )
      ? "  ", "start=" + hb_ntos( aRanges[ i ][ 1 ] ), "end=" + hb_ntos( aRanges[ i ][ 2 ] )
   NEXT
   IF Len( aRanges ) != 3
      ? "*** ESPERADO 3 rangos (uno por linea tocada) ***"
   ENDIF

   cText := Pdf_GetGlyphText( pDoc, 1, aRanges )
   ? "PDF_GETGLYPHTEXT:", "[" + cText + "]"
   IF cText != "BBB CCCC" + Chr( 10 ) + "DDDD EEEE FFFF" + Chr( 10 ) + "GGGG HH"
      ? "*** TEXTO NO SIGUE LA SECUENCIA DE LECTURA ESPERADA ***"
   ENDIF

   Pdf_Close( pDoc )
   ? "*** FIN ***"

RETURN
