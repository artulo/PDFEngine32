/* arena_lifecycle_demo.c
 *
 * Ejemplo minimo (sin parsear PDFs reales) para verificar que el ciclo
 * de vida documento/pagina no acumula memoria entre paginas -- util
 * como primer smoke test al portar el motor a un compilador nuevo.
 */

#include <stdio.h>
#include "pdf.h"

static void process_page(pdf_document *doc, int page_number)
{
    pdf_page page;
    int rc;
    void *scratch;

    rc = pdf_page_open(&page, doc, page_number);
    if (rc != PDF_OK)
    {
        printf("pagina %d: no se pudo abrir (presupuesto agotado)\n", page_number);
        return;
    }

    scratch = pdf_arena_alloc(&page.decode_arena, 4096);
    if (scratch != NULL)
        printf("pagina %d: decodifico un stream de prueba (4096 bytes)\n", page_number);

    pdf_page_recycle_decode_arena(&page);

    printf("pagina %d: uso en page_arena=%lu bytes\n",
           page_number, pdf_arena_used(&page.page_arena));

    pdf_page_close(&page);
}

int main(void)
{
    pdf_document doc;
    int rc, i;
    const pdf_ledger *ledger;

    rc = pdf_document_open(&doc, 8UL * 1024UL * 1024UL);
    if (rc != PDF_OK)
    {
        printf("no se pudo abrir el documento (rc=%d)\n", rc);
        return 1;
    }

    for (i = 1; i <= 50; i++)
        process_page(&doc, i);

    ledger = pdf_document_ledger(&doc);
    printf("pico de uso: %lu bytes de %lu bytes de presupuesto (pedidos denegados: %lu)\n",
           ledger->peak_bytes, ledger->budget_bytes, ledger->deny_count);

    pdf_document_close(&doc);
    return 0;
}
