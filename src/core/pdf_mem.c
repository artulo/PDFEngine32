/* pdf_mem.c
 *
 * Ver pdf_mem.h para el porque de este diseño frente a fitz/memory.c.
 */

#include "pdf_mem.h"
#include <stdlib.h>
#include <stdio.h>

/* PDF_ARENA_DEBUG=1 (variable de entorno): traza cada bloque NUEVO
 * pedido (no cada pdf_arena_alloc -- la mayoria se absorben en un
 * bloque ya existente) -- diagnostico TEMPORAL para encontrar un
 * consumo de memoria sin techo aparente (ver DESIGN.md, seccion de
 * rendimiento -- mupdf_bug.cgiid=701945-slow.rendering.pdf consume
 * TODO el presupuesto sin importar cuanto se suba, senial de un loop
 * o de un tamanio de asignacion corrupto/gigante). Mismo criterio que
 * PDF_JPX_DEBUG/PDF_GLYPH_DEBUG ya usados en este proyecto. */
static long g_pdf_arena_debug_block_count = 0;

/* Diagnostico TEMPORAL: ultimo operador de content stream despachado
 * (pdf_render.c lo actualiza al entrar a pdf_render_op) -- para saber
 * QUE operador esta activo cuando se pide un bloque nuevo de 40 bytes
 * repetido (ver DESIGN.md, investigacion de rendimiento en curso). */
char g_pdf_debug_last_op[32] = "";

/* Alineacion simple a sizeof(void*). Suficiente para todos los tipos
 * escalares en BCC770 de 32 bits. */
#define PDF_ALIGN_UP(n) \
    ( ((n) + (sizeof(void*) - 1)) & ~(sizeof(void*) - 1) )

struct pdf_arena_block_s
{
    pdf_arena_block *next;
    size_t           capacity;  /* bytes utiles del bloque              */
    size_t           used;      /* cuanto de 'capacity' esta ocupado    */
    /* los bytes de datos empiezan justo despues de este header;
     * se acceden via pdf_block_data() */
};

static unsigned char *pdf_block_data(pdf_arena_block *b)
{
    return (unsigned char *)b + sizeof(pdf_arena_block);
}

void pdf_ledger_init(pdf_ledger *ledger, unsigned long budget_bytes)
{
    if (ledger == NULL)
        return;
    if (budget_bytes == 0)
        budget_bytes = PDF_DEFAULT_BUDGET_BYTES;
    ledger->budget_bytes = budget_bytes;
    ledger->used_bytes   = 0;
    ledger->peak_bytes   = 0;
    ledger->deny_count   = 0;
}

int pdf_ledger_reserve(pdf_ledger *ledger, unsigned long bytes)
{
    unsigned long would_use;

    if (ledger == NULL)
        return PDF_ERR_BADARG;

    /* chequeo de overflow antes de sumar */
    if (bytes > ledger->budget_bytes - ledger->used_bytes
        && ledger->used_bytes <= ledger->budget_bytes)
    {
        /* no entra: el llamador (arena/cache) debe intentar podar el
         * cache LRU o degradar la operacion antes de reintentar. */
        ledger->deny_count++;
        return PDF_ERR_NOMEM;
    }

    would_use = ledger->used_bytes + bytes;
    if (would_use < ledger->used_bytes) /* overflow de unsigned long */
    {
        ledger->deny_count++;
        return PDF_ERR_NOMEM;
    }

    ledger->used_bytes = would_use;
    if (ledger->used_bytes > ledger->peak_bytes)
        ledger->peak_bytes = ledger->used_bytes;

    return PDF_OK;
}

void pdf_ledger_release(pdf_ledger *ledger, unsigned long bytes)
{
    if (ledger == NULL)
        return;
    if (bytes > ledger->used_bytes)
        ledger->used_bytes = 0;
    else
        ledger->used_bytes -= bytes;
}

int pdf_arena_init(pdf_arena *arena, pdf_ledger *ledger,
                    size_t block_size, const char *name)
{
    if (arena == NULL || ledger == NULL || block_size == 0)
        return PDF_ERR_BADARG;

    arena->ledger         = ledger;
    arena->first           = NULL;
    arena->current          = NULL;
    arena->block_size      = block_size;
    arena->total_reserved  = 0;
    arena->name            = name;

    return PDF_OK;
}

static pdf_arena_block *pdf_new_block(pdf_arena *arena, size_t min_size)
{
    size_t alloc_size;
    unsigned long need;
    pdf_arena_block *b;

    /* Guarda contra overflow: si 'min_size' es absurdamente grande (por
     * ejemplo, si en algun lugar se casteo un -1 a size_t sin querer),
     * "sizeof(pdf_arena_block) + alloc_size" puede desbordar la
     * aritmetica de 32 bits y envolver a un numero CHICO -- eso haria
     * que malloc() reserve un bloque chico de verdad mientras el resto
     * del codigo (via b->capacity) sigue creyendo que hay espacio
     * gigante, escribiendo muy por fuera del buffer real. Mejor cortar
     * aca con un limite defensivo razonable para este motor. */
    if (min_size > (256UL * 1024UL * 1024UL))
        return NULL;

    alloc_size = arena->block_size;
    if (min_size > alloc_size)
        alloc_size = min_size; /* objeto mas grande que el bloque tipico */

    need = (unsigned long)(sizeof(pdf_arena_block) + alloc_size);

    if (getenv("PDF_ARENA_DEBUG") != NULL)
    {
        g_pdf_arena_debug_block_count++;
        if (g_pdf_arena_debug_block_count <= 40 || (g_pdf_arena_debug_block_count % 5000) == 0 ||
            min_size > 1024UL*1024UL)
            fprintf(stderr, "PDF_ARENA_DEBUG: bloque #%ld min_size=%lu alloc_size=%lu used=%lu last_op='%s'\n",
                    g_pdf_arena_debug_block_count, (unsigned long)min_size, (unsigned long)alloc_size,
                    arena->ledger ? arena->ledger->used_bytes : 0UL, g_pdf_debug_last_op);
    }

    if (pdf_ledger_reserve(arena->ledger, need) != PDF_OK)
        return NULL; /* presupuesto agotado: el llamador debe degradar */

    b = (pdf_arena_block *)malloc(sizeof(pdf_arena_block) + alloc_size);
    if (b == NULL)
    {
        pdf_ledger_release(arena->ledger, need);
        return NULL;
    }

    b->next     = NULL;
    b->capacity = alloc_size;
    b->used     = 0;

    arena->total_reserved += need;

    if (arena->current != NULL)
        arena->current->next = b;
    else
        arena->first = b;
    arena->current = b;

    return b;
}

void *pdf_arena_alloc(pdf_arena *arena, size_t size)
{
    size_t aligned;
    pdf_arena_block *b;

    if (arena == NULL || size == 0)
        return NULL;

    aligned = PDF_ALIGN_UP(size);

    b = arena->current;
    if (b == NULL || b->used + aligned > b->capacity)
    {
        b = pdf_new_block(arena, aligned);
        if (b == NULL)
            return NULL;
    }

    {
        unsigned char *p = pdf_block_data(b) + b->used;
        b->used += aligned;
        return (void *)p;
    }
}

void pdf_arena_reset(pdf_arena *arena)
{
    pdf_arena_block *b, *next;

    if (arena == NULL)
        return;

    b = arena->first;
    while (b != NULL)
    {
        unsigned long freed = (unsigned long)(sizeof(pdf_arena_block) + b->capacity);
        next = b->next;
        free(b);
        pdf_ledger_release(arena->ledger, freed);
        b = next;
    }

    arena->first          = NULL;
    arena->current         = NULL;
    arena->total_reserved = 0;
}

void pdf_arena_destroy(pdf_arena *arena)
{
    pdf_arena_reset(arena);
}

unsigned long pdf_arena_used(const pdf_arena *arena)
{
    unsigned long total;
    pdf_arena_block *b;

    if (arena == NULL)
        return 0;

    total = 0;
    b = arena->first;
    while (b != NULL)
    {
        total += (unsigned long)b->used;
        b = b->next;
    }
    return total;
}
