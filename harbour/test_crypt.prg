/* test_crypt.prg -- prueba de consola del soporte AES nuevo
 * (pdf_aes.c/pdf_sha2.c/pdf_crypt.c). Abre el PDF real V4/R4/AESV2
 * que se sabe que fallaba antes del fix (tests/From_Garantia_primer_
 * requerimiento_0826.pdf) y confirma que ahora abre con paginas reales
 * en vez de "0 paginas".
 *
 * NOTA: PDF_PAGECOUNT anda bien en este harness standalone, pero
 * PDF_RENDERTOHBITMAP/PDF_EXTRACTTEXT crashean aca -- es el mismo bug
 * preexistente ya documentado de "el harness de consola no resuelve
 * paginas igual que la app GUI real" (confirmado independiente de este
 * cambio: pasa igual con un PDF sin cifrar). La verificacion real de
 * punta a punta de este fix se hizo contra pdf_demo.exe (capturas de
 * pantalla), no contra este harness -- ver DESIGN.md seccion 71 y
 * [[project_aes_encryption_implemented]]. Este archivo queda como
 * prueba de regresion rapida de PDF_PAGECOUNT nada mas. */

REQUEST HB_GT_STD_DEFAULT

PROCEDURE Main( cFile )

   LOCAL pDoc, nPages, nGlyphs, hBmp

   IF Empty( cFile )
      cFile := "..\tests\From_Garantia_primer_requerimiento_0826.pdf"
   ENDIF

   ? "Abriendo:", cFile
   pDoc := Pdf_Open( cFile, 0 )
   IF pDoc == NIL
      ? "*** PDF_OPEN devolvio NIL ***"
      RETURN
   ENDIF

   nPages := Pdf_PageCount( pDoc )
   ? "PDF_PAGECOUNT:", nPages

   IF nPages <= 0
      ? "*** FALLO: 0 paginas -- el descifrado AES no esta funcionando ***"
   ELSE
      ? "*** OK: el documento abrio con paginas reales ***"

      ? "Llamando Pdf_RenderToHBitmap..."
      hBmp := Pdf_RenderToHBitmap( pDoc, 1, 1.0 )
      ? "PDF_RENDERTOHBITMAP devolvio:", hBmp

      ? "Llamando Pdf_ExtractText..."
      nGlyphs := Pdf_ExtractText( pDoc, 1 )
      ? "PDF_EXTRACTTEXT(pagina 1):", nGlyphs, "glyphs"
      IF nGlyphs > 0
         ? "*** OK: se extrajo texto real de la pagina 1 (el contenido descifro bien) ***"
      ELSE
         ? "*** ADVERTENCIA: 0 glyphs en pagina 1 (puede ser una pagina sin texto, o el descifrado dio basura) ***"
      ENDIF
   ENDIF

   Pdf_Close( pDoc )
   ? "*** FIN ***"

RETURN
