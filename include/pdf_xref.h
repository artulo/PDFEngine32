/* pdf_xref.h
 *
 * Indice y resolucion de objetos indirectos PDF. Separa la localizacion
 * de objetos (xref clasico/xref stream/object streams) del parser de
 * sintaxis. La API mantiene compatibilidad con PDFEngine32_fix_22.
 */
#ifndef PDF_XREF_H
#define PDF_XREF_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_crypt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_xref_entry_s
{
    long offset;
    long gen;
    int  in_use;
    int  compressed;
    long objstm_num;
    long objstm_index;
} pdf_xref_entry;

typedef struct pdf_xref_table_s
{
    pdf_xref_entry *entries;
    long count;
    long capacity;
    pdf_obj *trailer;
    pdf_crypt crypt;

    /* Cache de resolucion de objetos, por numero (ver DESIGN.md,
     * seccion de rendimiento) -- 'resolved[num]' es NULL hasta la
     * primera vez que se resuelve, despues siempre devuelve el MISMO
     * puntero (arreglo paralelo a 'entries', tamanio 'count', armado
     * en pdf_xref_load() una vez que 'count' es definitivo). Sin esto,
     * pdf_xref_load_object() volvia a parsear el objeto desde cero en
     * CADA llamada, sin importar cuantas veces se pidiera el mismo
     * numero -- ademas de lento (un recorrido de /Pages completo para
     * renderizar N paginas costaba ~O(N^2) en reparseos), esto hacia
     * que dos resoluciones del mismo objeto en momentos distintos
     * fueran SIEMPRE copias independientes -- rompiendo, por ejemplo,
     * que una mutacion de AcroForm (pdf_dict_set sobre un widget
     * resuelto una vez) sobreviviera a un re-render posterior (que
     * resuelve el mismo widget de nuevo, sin verlo mutado). 'cache_arena'
     * es la arena de VIDA LARGA (siempre doc_arena en la practica,
     * ver pdf_xref_load) usada para TODA resolucion de objeto de aca
     * en mas -- el parametro 'arena' de pdf_xref_load_object() se
     * ignora para la asignacion en si (solo importa para decidir DONDE
     * cachear, no dentro de que arena de corta vida vive el objeto),
     * asi que un llamador puede seguir pasando una arena de pagina de
     * vida corta sin que el puntero cacheado quede colgando cuando esa
     * arena se libere. */
    pdf_obj **resolved;
    pdf_arena *cache_arena;
} pdf_xref_table;

void pdf_xref_reset(pdf_xref_table *xref);
int pdf_xref_load(pdf_stream *st, pdf_arena *arena, pdf_xref_table *xref);
long pdf_xref_offset(const pdf_xref_table *xref, long num);
const pdf_xref_entry *pdf_xref_entry_at(const pdf_xref_table *xref, long num);
pdf_obj *pdf_xref_load_object(pdf_stream *st, const pdf_xref_table *xref,
                              long num, pdf_arena *arena);

/* Nombre historico, conservado para compatibilidad con codigo existente. */
pdf_obj *pdf_parser_load_object(pdf_stream *st, const pdf_xref_table *xref,
                                long num, pdf_arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* PDF_XREF_H */
