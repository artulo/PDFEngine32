/* pdf_page.h
 *
 * Ciclo de vida de una pagina dentro de un documento abierto. Cada
 * pagina tiene dos arenas propias:
 *
 *   - page_arena:   vive mientras la pagina esta activa (objetos del
 *                    diccionario de pagina, arbol de contenido parseado).
 *                    Se libera entera en pdf_page_close.
 *   - decode_arena: vive durante la decodificacion de UN stream (una
 *                    imagen, un content stream). Se recicla con
 *                    pdf_page_recycle_decode_arena entre streams
 *                    sucesivos de la misma pagina.
 */

#ifndef PDF_PAGE_H
#define PDF_PAGE_H

#include "pdf_mem.h"
#include "pdf_document.h"
#include "pdf_parser.h"
#include "pdf_filter.h"
#include "pdf_device.h"
#include "pdf_display_list.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_BLOCK_SIZE_PAGE   (64UL * 1024UL)
#define PDF_BLOCK_SIZE_DECODE (16UL * 1024UL)

/* Tamanio de tile para decodificacion/render de imagenes grandes
 * (ver DESIGN.md 2.3). 256x256 a 4 bytes/px (RGBA) = 256 KB por tile,
 * cabe comodo en un solo bloque de la arena 'decode'. */
#define PDF_TILE_W 256
#define PDF_TILE_H 256

typedef struct pdf_page_s
{
    pdf_document *doc;
    pdf_arena     page_arena;
    pdf_arena     decode_arena;
    int           page_number;
} pdf_page;

/* Abre una pagina dentro del presupuesto del documento. */
int  pdf_page_open(pdf_page *page, pdf_document *doc, int page_number);

/* Libera la arena de la pagina de un solo golpe (O(numero de bloques),
 * no O(numero de objetos) -- sin importar si hubo errores de parseo o
 * decodificacion a mitad de camino). */
void pdf_page_close(pdf_page *page);

/* Recicla la arena de decodificacion entre streams sucesivos de la
 * misma pagina, para que no acumulen memoria que ya no hace falta. */
void pdf_page_recycle_decode_arena(pdf_page *page);

/* Resuelve y decodifica el content stream de una pagina, soportando
 * TANTO /Contents como referencia unica ("4 0 R") COMO array de
 * referencias ("[11 0 R 12 0 R ...]") -- este segundo caso es un patron
 * muy comun en PDFs reales (varios streams por pagina) que el estandar
 * exige tratar como si estuvieran concatenados con un salto de linea
 * entre ellos. Actualmente solo decodifica FlateDecode (el filtro mas
 * comun para content streams); otros filtros devuelven PDF_ERR_UNSUPPORTED.
 *
 * 'arena' es donde se alocan tanto los objetos intermedios como el
 * buffer final concatenado -- tipicamente page->decode_arena.
 *
 * Devuelve PDF_OK con 'out' apuntando al contenido decodificado listo
 * para pdf_content_run, o un codigo de error si no se encontro /Contents
 * o si algun stream no se pudo decodificar (en cuyo caso 'out' queda en
 * NULL/0, sin abortar el resto del pipeline). */
int pdf_page_get_content(pdf_stream *st, const pdf_xref_table *xref,
                          pdf_obj *page_obj, pdf_arena *arena, pdf_buf *out);

/* Resuelve la N-esima pagina (0-based) recorriendo el arbol /Pages de
 * forma RECURSIVA COMPLETA -- muchos PDFs reales (sobre todo los de
 * muchas paginas, generados por Acrobat/AutoCAD/InDesign) anidan /Pages
 * dentro de /Pages para balancear el arbol en vez de listar todas las
 * hojas directo en el nodo raiz. Mirar solo un nivel de /Kids (como
 * hacia una version anterior de este motor) hace que un nodo /Pages
 * intermedio se confunda con una pagina real -- tiene /MediaBox heredado
 * pero no /Contents propio, asi que el sintoma tipico es "se ve el
 * tamanio de pagina bien pero no aparece contenido".
 *
 * 'arena' es tipicamente doc_arena (los objetos del arbol de paginas son
 * de vida larga, se consultan en cada acceso a paginas del documento). */
pdf_obj *pdf_document_get_page(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, int page_index);

/* Devuelve un atributo heredable de la pagina siguiendo el Page Tree hacia
 * sus ancestros. Si la pagina tiene la clave, gana la definicion local;
 * si no, se busca el nodo /Pages padre mas cercano.
 *
 * Atributos previstos por PDF: Resources, MediaBox, CropBox, Rotate y
 * UserUnit. La funcion es generica y tambien sirve para diagnostico. */
pdf_obj *pdf_page_get_inherited(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, pdf_obj *page_obj,
                                const char *key);

/* Atajo para /Resources heredado. Devuelve siempre el diccionario resuelto
 * cuando existe; NULL significa que la pagina realmente no tiene recursos. */
pdf_obj *pdf_page_get_resources(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, pdf_obj *page_obj);

/* Ejecuta el contenido de la pagina directamente sobre un device. La
 * pagina se resuelve y su /Contents se decodifica internamente usando
 * decode_arena; el device no toma ownership de los objetos temporales. */
int pdf_page_run_device(pdf_stream *st, const pdf_xref_table *xref,
                        pdf_page *page, pdf_obj *page_obj,
                        pdf_device *device);

/* Construye una display list persistente de la pagina. Los operandos y
 * las imagenes inline quedan clonados dentro de la arena de la display
 * list, por lo que la decode_arena de la pagina puede reciclarse despues. */
int pdf_page_create_display_list(pdf_stream *st, const pdf_xref_table *xref,
                                 pdf_page *page, pdf_obj *page_obj,
                                 pdf_display_list *list);

#ifdef __cplusplus
}
#endif

#endif /* PDF_PAGE_H */
