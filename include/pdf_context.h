/* pdf_context.h
 *
 * Contexto central de PDFEngine32.
 *
 * Esta capa concentra el estado que antes estaba repartido entre
 * pdf_document, pdf_mem y pdf_cache. No reemplaza las arenas: las
 * administra. La finalidad es acercar el flujo interno a la arquitectura
 * Context -> Document -> Page -> Interpreter -> Device, manteniendo ANSI C89.
 */

#ifndef PDF_CONTEXT_H
#define PDF_CONTEXT_H

#include "pdf_error.h"
#include "pdf_mem.h"
#include "pdf_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_CONTEXT_CACHE_FRACTION 2UL

typedef void (*pdf_context_message_fn)(void *user, int level,
                                        int code, const char *message);

typedef struct pdf_context_s
{
    pdf_ledger ledger;
    pdf_cache  resource_cache;

    unsigned long budget_bytes;
    unsigned long error_count;
    unsigned long warning_count;
    int           last_error;
    int           strict;

    pdf_context_message_fn message_fn;
    void                    *message_user;
} pdf_context;

int pdf_context_init(pdf_context *ctx, unsigned long budget_bytes,
                     pdf_cache_free_fn cache_free_fn,
                     void *cache_free_user);

void pdf_context_close(pdf_context *ctx);

int pdf_context_set_error(pdf_context *ctx, int code,
                          const char *message);

int pdf_context_warn(pdf_context *ctx, int code,
                     const char *message);

void pdf_context_clear_error(pdf_context *ctx);

int pdf_context_last_error(const pdf_context *ctx);

const pdf_ledger *pdf_context_ledger(const pdf_context *ctx);

pdf_ledger *pdf_context_ledger_mut(pdf_context *ctx);

pdf_cache *pdf_context_cache(pdf_context *ctx);

void pdf_context_set_message_proc(pdf_context *ctx,
                                  pdf_context_message_fn fn,
                                  void *user);

void pdf_context_set_strict(pdf_context *ctx, int strict_mode);

#ifdef __cplusplus
}
#endif

#endif /* PDF_CONTEXT_H */
