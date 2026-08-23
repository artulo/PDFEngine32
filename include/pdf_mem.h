/* pdf_mem.h
 *
 * Arenas de memoria con presupuesto obligatorio.
 *
 * Reemplaza al patron "malloc/free por objeto" de MuPDF (fitz/memory.c,
 * fitz/store.c) por bloques de vida acotada que se liberan enteros.
 *
 * Compatible con BCC770 (C ANSI: sin declarar variables a mitad de bloque,
 * sin comentarios //, sin extensiones GNU).
 */

#ifndef PDF_MEM_H
#define PDF_MEM_H

#include <stddef.h>
#include "pdf_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Los codigos de error (PDF_OK, PDF_ERR_NOMEM, etc.) viven en pdf_error.h */

/* ---- ledger: contador global de presupuesto ------------------------ */

typedef struct pdf_ledger_s
{
    unsigned long budget_bytes;   /* presupuesto total del proceso/doc */
    unsigned long used_bytes;     /* reservado actualmente             */
    unsigned long peak_bytes;     /* maximo historico, para diagnostico*/
    unsigned long deny_count;     /* veces que se nego un pedido       */
} pdf_ledger;

/* Inicializa un ledger. budget_bytes NUNCA puede ser "ilimitado":
 * si el llamador pasa 0, se aplica PDF_DEFAULT_BUDGET_BYTES. */
#define PDF_DEFAULT_BUDGET_BYTES (64UL * 1024UL * 1024UL) /* 64 MB */

void pdf_ledger_init(pdf_ledger *ledger, unsigned long budget_bytes);

/* Intenta reservar 'bytes' contra el ledger. Devuelve PDF_OK o
 * PDF_ERR_NOMEM. No toca al sistema operativo: solo lleva la cuenta. */
int  pdf_ledger_reserve(pdf_ledger *ledger, unsigned long bytes);

/* Libera 'bytes' del ledger (nunca falla; satura en 0). */
void pdf_ledger_release(pdf_ledger *ledger, unsigned long bytes);

/* ---- arena: bloque de vida acotada ---------------------------------- */

typedef struct pdf_arena_block_s pdf_arena_block;

typedef struct pdf_arena_s
{
    pdf_ledger      *ledger;      /* ledger contra el que descuenta      */
    pdf_arena_block *first;
    pdf_arena_block *current;
    size_t           block_size;  /* tamanio de cada bloque interno      */
    unsigned long    total_reserved; /* bytes tomados del ledger por esta arena */
    const char      *name;        /* solo diagnostico ("doc","page","decode") */
} pdf_arena;

/* Crea una arena que descuenta del ledger dado. block_size es el tamanio
 * de cada bloque interno (recomendado: 64KB para 'page', 16KB para
 * 'decode', 256KB para 'doc'). Devuelve PDF_OK o error. */
int pdf_arena_init(pdf_arena *arena, pdf_ledger *ledger,
                    size_t block_size, const char *name);

/* Reserva 'size' bytes dentro de la arena. Si el bloque actual no alcanza,
 * pide un bloque nuevo al ledger (puede fallar con PDF_ERR_NOMEM si no hay
 * presupuesto). Alineado a sizeof(void*). */
void *pdf_arena_alloc(pdf_arena *arena, size_t size);

/* Libera TODA la arena de una vez (O(numero de bloques), no O(numero de
 * objetos)). Devuelve el presupuesto tomado al ledger. Deja la arena lista
 * para reusarse (no hace falta volver a llamar pdf_arena_init). */
void pdf_arena_reset(pdf_arena *arena);

/* Destruye la arena definitivamente (libera bloques + reset). */
void pdf_arena_destroy(pdf_arena *arena);

/* Bytes actualmente en uso logico dentro de la arena (para diagnostico /
 * decidir si conviene degradar una decodificacion a menor resolucion). */
unsigned long pdf_arena_used(const pdf_arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* PDF_MEM_H */
