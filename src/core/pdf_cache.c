/* pdf_cache.c
 *
 * Ver pdf_cache.h para el diseno. Nodos de la lista viven en una arena
 * propia (no malloc/free por nodo).
 */

#include "pdf_cache.h"
#include <stddef.h>

#define PDF_CACHE_DEFAULT_MAX_BYTES (16UL * 1024UL * 1024UL) /* 16 MB */
#define PDF_CACHE_NODE_BLOCK        (8UL * 1024UL)            /* 8 KB por bloque de nodos */

struct pdf_cache_node_s
{
    const void      *key;
    void            *value;
    unsigned long    size_bytes;
    pdf_cache_node  *prev; /* hacia MRU */
    pdf_cache_node  *next; /* hacia LRU */
};

int pdf_cache_init(pdf_cache *cache, pdf_ledger *ledger,
                    unsigned long max_bytes,
                    pdf_cache_free_fn free_fn, void *free_fn_user)
{
    int rc;

    if (cache == NULL || ledger == NULL)
        return PDF_ERR_BADARG;

    if (max_bytes == 0)
        max_bytes = PDF_CACHE_DEFAULT_MAX_BYTES;

    rc = pdf_arena_init(&cache->node_arena, ledger,
                         (size_t)PDF_CACHE_NODE_BLOCK, "cache-nodes");
    if (rc != PDF_OK)
        return rc;

    cache->ledger        = ledger;
    cache->head           = NULL;
    cache->tail           = NULL;
    cache->max_bytes      = max_bytes;
    cache->used_bytes     = 0;
    cache->free_fn        = free_fn;
    cache->free_fn_user   = free_fn_user;

    return PDF_OK;
}

static void pdf_cache_unlink(pdf_cache *cache, pdf_cache_node *n)
{
    if (n->prev != NULL) n->prev->next = n->next; else cache->head = n->next;
    if (n->next != NULL) n->next->prev = n->prev; else cache->tail = n->prev;
    n->prev = NULL;
    n->next = NULL;
}

static void pdf_cache_push_front(pdf_cache *cache, pdf_cache_node *n)
{
    n->prev = NULL;
    n->next = cache->head;
    if (cache->head != NULL)
        cache->head->prev = n;
    cache->head = n;
    if (cache->tail == NULL)
        cache->tail = n;
}

static void pdf_cache_evict_node(pdf_cache *cache, pdf_cache_node *n)
{
    pdf_cache_unlink(cache, n);
    cache->used_bytes -= n->size_bytes;
    if (cache->free_fn != NULL)
        cache->free_fn(n->value, cache->free_fn_user);
    /* el nodo en si NO se libera individualmente: vive en node_arena y
     * se recupera recien cuando se llama pdf_cache_clear(). Esto es a
     * proposito -- ver DESIGN.md 2.1: preferimos "desperdiciar" unos
     * bytes de nodo ya evictado antes de la proxima poda completa, a
     * cambio de no fragmentar el heap con free() individuales. */
}

/* Poda hasta liberar al menos 'need_bytes' dentro del techo del cache.
 * Devuelve 1 si logro suficiente espacio, 0 si no (cache vacio). */
static int pdf_cache_make_room(pdf_cache *cache, unsigned long need_bytes)
{
    while (cache->used_bytes + need_bytes > cache->max_bytes && cache->tail != NULL)
    {
        pdf_cache_evict_node(cache, cache->tail);
    }
    return (cache->used_bytes + need_bytes <= cache->max_bytes);
}

void *pdf_cache_get(pdf_cache *cache, const void *key)
{
    pdf_cache_node *n;

    if (cache == NULL || key == NULL)
        return NULL;

    for (n = cache->head; n != NULL; n = n->next)
    {
        if (n->key == key)
        {
            if (n != cache->head)
            {
                pdf_cache_unlink(cache, n);
                pdf_cache_push_front(cache, n);
            }
            return n->value;
        }
    }
    return NULL;
}

int pdf_cache_put(pdf_cache *cache, const void *key, void *value,
                   unsigned long size_bytes)
{
    pdf_cache_node *n;

    if (cache == NULL || key == NULL || value == NULL)
        return PDF_ERR_BADARG;

    if (size_bytes > cache->max_bytes)
        return PDF_ERR_NOMEM; /* nunca va a entrar, ni podando todo */

    /* podar ANTES de reservar el nodo -- a diferencia de fitz/memory.c,
     * que reserva primero y solo poda si el malloc del SO fallo. */
    if (!pdf_cache_make_room(cache, size_bytes))
        return PDF_ERR_NOMEM;

    n = (pdf_cache_node *)pdf_arena_alloc(&cache->node_arena, sizeof(pdf_cache_node));
    if (n == NULL)
        return PDF_ERR_NOMEM;

    n->key        = key;
    n->value      = value;
    n->size_bytes = size_bytes;
    n->prev       = NULL;
    n->next       = NULL;

    pdf_cache_push_front(cache, n);
    cache->used_bytes += size_bytes;

    return PDF_OK;
}

void pdf_cache_clear(pdf_cache *cache)
{
    pdf_cache_node *n;

    if (cache == NULL)
        return;

    if (cache->free_fn != NULL)
    {
        for (n = cache->head; n != NULL; n = n->next)
            cache->free_fn(n->value, cache->free_fn_user);
    }

    pdf_arena_reset(&cache->node_arena);
    cache->head       = NULL;
    cache->tail       = NULL;
    cache->used_bytes = 0;
}

void pdf_cache_destroy(pdf_cache *cache)
{
    if (cache == NULL)
        return;
    pdf_cache_clear(cache);
    pdf_arena_destroy(&cache->node_arena);
}
