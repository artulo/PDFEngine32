/* test_acroform.prg -- prueba de consola de los bindings AcroForm
 * nuevos (Pdf_FormListFields/Pdf_FormSetFieldValue, ver pdf_hbfunc.c/
 * pdf_form.c).
 *
 * NOTA: PDF_PAGECOUNT anda bien en este harness, pero
 * PDF_FORMLISTFIELDS crashea -- mismo bug preexistente ya documentado
 * en test_crypt.prg ("el harness de consola no resuelve paginas igual
 * que la app GUI real"): las tres funciones que crashean aca
 * (Pdf_RenderToHBitmap, Pdf_ExtractText, y ahora Pdf_FormListFields)
 * llaman todas a pdf_document_get_page(); la unica que anda
 * (Pdf_PageCount) no la llama. Evidencia indirecta fuerte de que NO es
 * un bug nuevo de pdf_form.c: la MISMA logica de parseo de campos
 * (pdf_form_list_fields) ya se confirmo funcionando correctamente
 * contra este mismo archivo real via la app GUI real
 * (pdf_render_draw_annotations, que la llama con el mismo page_obj) --
 * ver DESIGN.md y [[project_aes_encryption_implemented]] para el
 * patron de verificacion via pdf_demo.exe en vez de este harness. */

REQUEST HB_GT_STD_DEFAULT

PROCEDURE Main( cFile )

   LOCAL pDoc, aFields, aF, i, lOk, cNew

   IF Empty( cFile )
      cFile := "..\tests\From_Garantia_primer_requerimiento_0826.pdf"
   ENDIF

   ? "Abriendo:", cFile
   pDoc := Pdf_Open( cFile, 0 )
   IF pDoc == NIL
      ? "*** PDF_OPEN devolvio NIL ***"
      RETURN
   ENDIF
   ? "PDF_PAGECOUNT:", Pdf_PageCount( pDoc )

   aFields := Pdf_FormListFields( pDoc, 1 )
   ? "PDF_FORMLISTFIELDS(pagina 1):", Len( aFields ), "campo(s)"

   FOR i := 1 TO Len( aFields )
      aF := aFields[ i ]
      ? "  [" + hb_ntos(i) + "]", ;
        "tipo=" + hb_ntos( aF[1] ), ;
        "valor=[" + aF[2] + "]", ;
        "rect=(" + hb_ntos(aF[3]) + "," + hb_ntos(aF[4]) + ")-(" + hb_ntos(aF[5]) + "," + hb_ntos(aF[6]) + ")", ;
        "nombre=" + aF[7], ;
        "soloLectura=" + hb_ntos( IIF(aF[8],1,0) ), ;
        "objNum=" + hb_ntos(aF[9]), "objGen=" + hb_ntos(aF[10]), ;
        "onState=" + aF[11]
      IF i >= 15
         ? "  ... (truncado, mas de 15 campos)"
         EXIT
      ENDIF
   NEXT

   IF Len( aFields ) == 0
      ? "*** ADVERTENCIA: 0 campos -- o el PDF no tiene AcroForm, o algo fallo en el parseo ***"
   ELSE
      ? "*** OK: se listaron campos reales ***"

      /* Probar SetFieldValue sobre el primer campo de TEXTO que aparezca */
      FOR i := 1 TO Len( aFields )
         aF := aFields[ i ]
         IF aF[1] == 1 .AND. !aF[8]
            ? "Probando Pdf_FormSetFieldValue sobre campo de texto:", aF[7], "(objNum=" + hb_ntos(aF[9]) + ")"
            lOk := Pdf_FormSetFieldValue( pDoc, aF[9], aF[10], 1, "PRUEBA AUTOMATICA 123" )
            ? "  resultado:", lOk

            /* releer la lista -- deberia reflejar el nuevo valor, ya
               que se muto el objeto EN MEMORIA (no hace falta reabrir) */
            aFields := Pdf_FormListFields( pDoc, 1 )
            ? "  valor releido:", "[" + aFields[i][2] + "]"
            IF aFields[i][2] == "PRUEBA AUTOMATICA 123"
               ? "  *** OK: el valor mutado se refleja en una relectura ***"
            ELSE
               ? "  *** FALLO: el valor releido no coincide ***"
            ENDIF
            EXIT
         ENDIF
      NEXT

      /* Probar toggle de checkbox sobre el primer checkbox que aparezca */
      FOR i := 1 TO Len( aFields )
         aF := aFields[ i ]
         IF aF[1] == 2 .AND. !aF[8] .AND. !Empty( aF[11] )
            cNew := IIF( aF[2] == aF[11], "Off", aF[11] )
            ? "Probando Pdf_FormSetFieldValue sobre checkbox:", aF[7], "(objNum=" + hb_ntos(aF[9]) + ") nuevo valor=" + cNew
            lOk := Pdf_FormSetFieldValue( pDoc, aF[9], aF[10], 2, cNew )
            ? "  resultado:", lOk
            aFields := Pdf_FormListFields( pDoc, 1 )
            ? "  valor releido:", "[" + aFields[i][2] + "]"
            IF aFields[i][2] == cNew
               ? "  *** OK: el checkbox mutado se refleja en una relectura ***"
            ELSE
               ? "  *** FALLO: el valor releido no coincide ***"
            ENDIF
            EXIT
         ENDIF
      NEXT
   ENDIF

   Pdf_Close( pDoc )
   ? "*** FIN ***"

RETURN
