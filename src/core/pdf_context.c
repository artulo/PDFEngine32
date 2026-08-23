/* pdf_context.c
 *
 * Ver pdf_context.h.
 */

#include "pdf_context.h"
#include <stddef.h>

static void pdf_context_emit(pdf_context *ctx, int level, int code,
                             const char *message)
{
    if (ctx == NULL)
        return;

    if (ctx->message_fn != NULL)
        ctx->message_fn(ctx->message_user, level, code, message);
}

int pdf_context_init(pdf_context *ctx, unsigned long budget_bytes,
                     pdf_cache_free_fn cache_free_fn,
                     void *cache_free_user)
{
    int rc;
    unsigned long cache_budget;

    if (ctx == NULL)
        return PDF_ERR_BADARG;

    ctx->budget_bytes   = 0;
    ctx->error_count    = 0;
    ctx->warning_count  = 0;
    ctx->last_error     = PDF_OK;
    ctx->strict         = 0;
    ctx->message_fn     = NULL;
    ctx->message_user   = NULL;

    pdf_ledger_init(&ctx->ledger, budget_bytes);
    ctx->budget_bytes = ctx->ledger.budget_bytes;

    cache_budget = ctx->budget_bytes / PDF_CONTEXT_CACHE_FRACTION;
    if (cache_budget == 0)
        cache_budget = ctx->budget_bytes;

    rc = pdf_cache_init(&ctx->resource_cache, &ctx->ledger,
                        cache_budget, cache_free_fn, cache_free_user);
    if (rc != PDF_OK)
    {
        ctx->last_error = rc;
        return rc;
    }

    return PDF_OK;
}

void pdf_context_close(pdf_context *ctx)
{
    if (ctx == NULL)
        return;

    pdf_cache_destroy(&ctx->resource_cache);
    ctx->last_error = PDF_OK;
}

int pdf_context_set_error(pdf_context *ctx, int code,
                          const char *message)
{
    if (ctx == NULL)
        return code;

    if (code == PDF_OK)
        return PDF_OK;

    ctx->last_error = code;
    ctx->error_count++;
    pdf_context_emit(ctx, 2, code, message);

    return code;
}

int pdf_context_warn(pdf_context *ctx, int code,
                     const char *message)
{
    if (ctx == NULL)
        return code;

    ctx->warning_count++;
    pdf_context_emit(ctx, 1, code, message);

    if (ctx->strict && code != PDF_OK)
        return pdf_context_set_error(ctx, code, message);

    return PDF_OK;
}

void pdf_context_clear_error(pdf_context *ctx)
{
    if (ctx == NULL)
        return;
    ctx->last_error = PDF_OK;
}

int pdf_context_last_error(const pdf_context *ctx)
{
    if (ctx == NULL)
        return PDF_ERR_BADARG;
    return ctx->last_error;
}

const pdf_ledger *pdf_context_ledger(const pdf_context *ctx)
{
    if (ctx == NULL)
        return NULL;
    return &ctx->ledger;
}

pdf_ledger *pdf_context_ledger_mut(pdf_context *ctx)
{
    if (ctx == NULL)
        return NULL;
    return &ctx->ledger;
}

pdf_cache *pdf_context_cache(pdf_context *ctx)
{
    if (ctx == NULL)
        return NULL;
    return &ctx->resource_cache;
}

void pdf_context_set_message_proc(pdf_context *ctx,
                                  pdf_context_message_fn fn,
                                  void *user)
{
    if (ctx == NULL)
        return;

    ctx->message_fn = fn;
    ctx->message_user = user;
}

void pdf_context_set_strict(pdf_context *ctx, int strict_mode)
{
    if (ctx == NULL)
        return;

    ctx->strict = strict_mode ? 1 : 0;
}
