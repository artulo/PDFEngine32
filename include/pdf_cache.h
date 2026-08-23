/* pdf_cache.h
 *
 * Cache LRU de recursos reutilizables entre paginas (fuentes, glyphs,
 * imagenes decodificadas), equivalente a fitz/store.c pero con dos
 * diferencias de fondo:
 *
 *   1. El presupuesto (pdf_ledger) es obligatorio, no "FZ_STORE_UNLIMITED".
 *   2. La poda ocurre ANTES de insertar un item nuevo que no entra, no
 *      reactivamente cuando malloc() ya fallo (ver do_scavenging_malloc
 *      en fitz/memory.c: ahi primero se intenta crecer, y solo si el SO
 *      rechaza el pedido se libera algo -- eso es lo que fragmenta el
 *      heap en procesos largos).
 *
 * No usa malloc/free por nodo: los nodos del cache viven en una pdf_arena
 * "cache" propia, que se resetea entera cuando se vacia el cache -- igual
 * de barato reconstruirla que iterar y liberar nodo por nodo.
 */

#ifndef PDF_CACHE_H
#define PDF_CACHE_H

#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pdf_cache_free_fn)(void *value, void *user);

typedef struct pdf_cache_node_s pdf_cache_node;

typedef struct pdf_cache_s
{
    pdf_ledger        *ledger;
    pdf_arena           node_arena;   /* almacena los nodos del cache */
    pdf_cache_node    *head;         /* MRU */
    pdf_cache_node    *tail;         /* LRU */
    unsigned long       max_bytes;    /* techo propio del cache (subconjunto del ledger) */
    unsigned long       used_bytes;
    pdf_cache_free_fn   free_fn;
    void               *free_fn_user;
} pdf_cache;

/* max_bytes: techo del cache. Si es 0 se usa un techo conservador
 * (no existe modo "sin techo" -- ver DESIGN.md, seccion 2.2). */
int  pdf_cache_init(pdf_cache *cache, pdf_ledger *ledger,
                     unsigned long max_bytes,
                     pdf_cache_free_fn free_fn, void *free_fn_user);

/* Busca por puntero de clave (comparacion exacta de puntero: el llamador
 * es responsable de "internar" claves iguales al mismo puntero, p.ej. el
 * mismo fz_obj de fuente). Si lo encuentra, lo mueve a MRU y devuelve el
 * valor. Si no, devuelve NULL. */
void *pdf_cache_get(pdf_cache *cache, const void *key);

/* Inserta 'value' bajo 'key' con costo 'size_bytes'. Si no entra en el
 * presupuesto ni podando LRU, la insercion se descarta silenciosamente
 * (el llamador sigue usando 'value' el tiempo que la necesite y luego
 * el propio llamador es responsable de liberarla -- el cache no tomo
 * posesion). Devuelve PDF_OK si quedo cacheado, PDF_ERR_NOMEM si no. */
int pdf_cache_put(pdf_cache *cache, const void *key, void *value,
                   unsigned long size_bytes);

/* Vacia el cache entero de una sola vez (llama free_fn por cada valor,
 * libera los nodos reseteando la arena interna -- no nodo por nodo). */
void pdf_cache_clear(pdf_cache *cache);

void pdf_cache_destroy(pdf_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* PDF_CACHE_H */
