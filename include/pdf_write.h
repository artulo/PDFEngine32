/* pdf_write.h
 *
 * Escritor de PDF minimo: SOLO "incremental update" (la tecnica
 * estandar de PDF para esto -- agregar objetos nuevos + una seccion de
 * xref nueva al final del archivo, dejando todo lo existente intacto).
 * Pensado para el caso concreto de AcroForm (ver pdf_form.h/DESIGN.md):
 * un puñado de objetos "tocados" (widgets con /V o /AP mutados en
 * memoria, mas los streams de apariencia nuevos generados por
 * pdf_form_generate_text_appearance) se serializan y se apendean al
 * archivo original.
 *
 * NO es un serializador de documentos completo -- no reescribe streams
 * sin modificar (no hace falta: siguen resolviendo contra el archivo
 * original via /Prev, que cualquier lector PDF real sabe seguir,
 * incluido este motor). NO soporta documentos encriptados en esta
 * etapa (ver DESIGN.md, limitacion de alcance explicita).
 */

#ifndef PDF_WRITE_H
#define PDF_WRITE_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_xref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Escribe una actualizacion incremental a 'out_path' con los
 * 'n_touched' objetos de 'touched_objs'/'touched_nums'/'touched_gens'
 * (arreglos paralelos -- el pdf_obj YA MUTADO/generado, su numero y
 * generacion de objeto). 'st'/'xref' son el documento ABIERTO
 * original (se usa 'st->fp' para copiar el archivo byte a byte, y
 * 'xref->trailer' para /Root e /ID).
 *
 * Escritura seguraa: todo se arma primero en un archivo temporal
 * ('out_path' + ".pdftmp"); el archivo ORIGINAL se renombra a
 * 'out_path' + ".bak" (no se borra) ANTES de mover el temporal a su
 * lugar -- si algo falla a mitad de camino, el ORIGINAL nunca se pierde
 * (queda como .bak, recuperable a mano). No se intenta borrar el .bak
 * al terminar -- decision deliberada, mismo espiritu que "nunca perder
 * el archivo real de Arturo" del resto de este modulo.
 *
 * Devuelve PDF_OK, o un codigo de error (PDF_ERR_BADARG/PDF_ERR_IO) si
 * no se pudo escribir -- en ese caso 'out_path' puede haber quedado
 * como el .bak si el fallo fue DESPUES de renombrar el original (raro,
 * solo si el ultimo rename() del temporal falla) -- se intenta
 * recuperar automaticamente (renombrar el .bak de vuelta) en ese caso. */
int pdf_write_incremental_update(pdf_stream *st, const pdf_xref_table *xref,
                                  pdf_arena *arena,
                                  pdf_obj **touched_objs,
                                  const long *touched_nums, const long *touched_gens,
                                  int n_touched, const char *out_path);

#ifdef __cplusplus
}
#endif

#endif /* PDF_WRITE_H */
