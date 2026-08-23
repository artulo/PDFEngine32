/* pdf_document.h
 *
 * Ciclo de vida del documento. El estado global de memoria/cache/error
 * vive en pdf_context; doc_arena conserva los objetos de larga vida del
 * documento. Esto prepara la arquitectura Context -> Document -> Page.
 */

#ifndef PDF_DOCUMENT_H
#define PDF_DOCUMENT_H

#include "pdf_context.h"
#include "pdf_xref.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_BLOCK_SIZE_DOC (256UL * 1024UL)

typedef struct pdf_document_s
{
    pdf_context context;
    pdf_arena   doc_arena;
    void       *reserved;
} pdf_document;

int  pdf_document_open(pdf_document *doc, unsigned long budget_bytes);
void pdf_document_close(pdf_document *doc);

const pdf_ledger *pdf_document_ledger(const pdf_document *doc);
pdf_ledger *pdf_document_ledger_mut(pdf_document *doc);
pdf_context *pdf_document_context(pdf_document *doc);
const pdf_context *pdf_document_context_const(const pdf_document *doc);

/* Resuelve Catalog (/Root) y el nodo raiz del arbol de paginas (/Pages).
 * Los objetos devueltos viven en 'arena'. La resolucion de referencias se
 * hace mediante el subsistema XRef y no duplica la logica del parser. */
pdf_obj *pdf_document_get_catalog(pdf_stream *st, const pdf_xref_table *xref,
                                   pdf_arena *arena);
pdf_obj *pdf_document_get_pages_root(pdf_stream *st, const pdf_xref_table *xref,
                                      pdf_arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* PDF_DOCUMENT_H */
