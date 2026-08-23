/* pdf_document.c
 *
 * Ver pdf_document.h.
 */

#include "pdf_document.h"
#include <stddef.h>

static void pdf_resource_free_fn(void *value, void *user)
{
    (void)value;
    (void)user;
}

int pdf_document_open(pdf_document *doc, unsigned long budget_bytes)
{
    int rc;

    if (doc == NULL)
        return PDF_ERR_BADARG;

    rc = pdf_context_init(&doc->context, budget_bytes,
                          pdf_resource_free_fn, NULL);
    if (rc != PDF_OK)
        return rc;

    rc = pdf_arena_init(&doc->doc_arena,
                        pdf_context_ledger_mut(&doc->context),
                        (size_t)PDF_BLOCK_SIZE_DOC, "doc");
    if (rc != PDF_OK)
    {
        pdf_context_close(&doc->context);
        return rc;
    }

    doc->reserved = NULL;
    return PDF_OK;
}

void pdf_document_close(pdf_document *doc)
{
    if (doc == NULL)
        return;

    pdf_arena_destroy(&doc->doc_arena);
    pdf_context_close(&doc->context);
}

const pdf_ledger *pdf_document_ledger(const pdf_document *doc)
{
    if (doc == NULL)
        return NULL;
    return pdf_context_ledger(&doc->context);
}

pdf_ledger *pdf_document_ledger_mut(pdf_document *doc)
{
    if (doc == NULL)
        return NULL;
    return pdf_context_ledger_mut(&doc->context);
}

pdf_context *pdf_document_context(pdf_document *doc)
{
    if (doc == NULL)
        return NULL;
    return &doc->context;
}

const pdf_context *pdf_document_context_const(const pdf_document *doc)
{
    if (doc == NULL)
        return NULL;
    return &doc->context;
}


static pdf_obj *pdf_document_resolve_ref(pdf_stream *st,
                                          const pdf_xref_table *xref,
                                          pdf_arena *arena,
                                          pdf_obj *obj)
{
    if (obj == NULL)
        return NULL;
    if (obj->type != PDF_REF)
        return obj;
    return pdf_xref_load_object(st, xref, obj->u.ref.num, arena);
}

pdf_obj *pdf_document_get_catalog(pdf_stream *st, const pdf_xref_table *xref,
                                   pdf_arena *arena)
{
    pdf_obj *root;

    if (st == NULL || xref == NULL || arena == NULL || xref->trailer == NULL)
        return NULL;

    root = pdf_dict_get(xref->trailer, "Root");
    root = pdf_document_resolve_ref(st, xref, arena, root);
    if (root == NULL || root->type != PDF_DICT)
        return NULL;

    return root;
}

pdf_obj *pdf_document_get_pages_root(pdf_stream *st, const pdf_xref_table *xref,
                                      pdf_arena *arena)
{
    pdf_obj *catalog;
    pdf_obj *pages;

    catalog = pdf_document_get_catalog(st, xref, arena);
    if (catalog == NULL)
        return NULL;

    pages = pdf_dict_get(catalog, "Pages");
    pages = pdf_document_resolve_ref(st, xref, arena, pages);
    if (pages == NULL || pages->type != PDF_DICT)
        return NULL;

    return pages;
}
