/* pdf_parser.h
 *
 * Parseo de objetos PDF (recursivo sobre pdf_lexer/pdf_stream). La tabla
 * xref y la resolucion de objetos viven en pdf_xref.c; se soporta TANTO la tabla clasica ("xref" ... "trailer")
 * COMO xref streams (PDF 1.5+, /Type /XRef) con object streams
 * (/Type /ObjStm) para objetos comprimidos. Los objetos indirectos se
 * cargan bajo demanda mediante pdf_xref_load_object() (con alias
 * historico pdf_parser_load_object()).
 */

#ifndef PDF_PARSER_H
#define PDF_PARSER_H

#include "pdf_object.h"
#include "pdf_lexer.h"
#include "pdf_stream.h"
#include "pdf_crypt.h"
#include "pdf_xref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parsea UN objeto (numero, string, name, array, dict o stream) a partir
 * del token ya leido en 'tok' (o si tok->type==PDF_TOK_EOF, lee el
 * primer token el mismo). 'arena' es donde vive el objeto resultante.
 * No resuelve referencias indirectas (deja PDF_REF tal cual). */
pdf_obj *pdf_parse_object(pdf_stream *st, pdf_token *tok, pdf_arena *arena);

/* Parsea "N G obj ... endobj" completo, posicionando el stream en
 * 'offset' antes de arrancar. Si el objeto es un stream (dict seguido
 * de "stream" ... "endstream"), NO copia los bytes del stream a
 * memoria: solo guarda raw_offset/raw_length en el pdf_obj resultante
 * (PDF_STREAM) para que se lean bajo demanda con pdf_stream_read +
 * el filtro correspondiente, escribiendo en decode_arena.
 *
 * 'xref_for_length' (puede ser NULL) se usa SOLO para resolver /Length
 * cuando viene como referencia indirecta ("/Length 5 0 R", un patron muy
 * comun en PDFs generados en streaming, donde el productor no conoce el
 * tamanio del stream hasta despues de escribirlo). Si es NULL y /Length
 * resulta ser una referencia, raw_length queda en -1 (sin resolver) --
 * comportamiento anterior, preservado para cuando todavia no hay xref
 * disponible (p.ej. mientras se esta cargando el xref mismo). */
pdf_obj *pdf_parse_indirect_object(pdf_stream *st, long offset,
                                    long *out_num, long *out_gen,
                                    pdf_arena *arena,
                                    const pdf_xref_table *xref_for_length);

#ifdef __cplusplus
}
#endif

#endif /* PDF_PARSER_H */
