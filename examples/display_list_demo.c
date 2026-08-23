/* display_list_demo.c
 *
 * Verificacion minima de la arquitectura:
 * Content -> Display List -> Device.
 *
 * Uso: display_list_demo archivo.pdf
 */
#include <stdio.h>
#include <string.h>
#include "pdf.h"

int main(int argc, char **argv)
{
    const char *path;
    pdf_stream st;
    pdf_document doc;
    pdf_xref_table xref;
    pdf_page page;
    pdf_obj *page_obj;
    pdf_display_list list;
    int rc;

    path = (argc > 1) ? argv[1] : "rects.pdf";
    memset(&list, 0, sizeof(list));

    rc = pdf_stream_open(&st, path);
    if (rc != PDF_OK) return 1;
    rc = pdf_document_open(&doc, 128UL * 1024UL * 1024UL);
    if (rc != PDF_OK) { pdf_stream_close(&st); return 1; }
    rc = pdf_xref_load(&st, &doc.doc_arena, &xref);
    if (rc != PDF_OK) { pdf_document_close(&doc); pdf_stream_close(&st); return 1; }
    page_obj = pdf_document_get_page(&st, &xref, &doc.doc_arena, 0);
    if (page_obj == NULL) { pdf_document_close(&doc); pdf_stream_close(&st); return 1; }
    rc = pdf_page_open(&page, &doc, 1);
    if (rc != PDF_OK) { pdf_document_close(&doc); pdf_stream_close(&st); return 1; }

    rc = pdf_page_create_display_list(&st, &xref, &page, page_obj, &list);
    printf("create=%d count=%lu error=%d\\n", rc,
           pdf_display_list_count(&list), pdf_display_list_error(&list));

    pdf_display_list_destroy(&list);
    pdf_page_close(&page);
    pdf_document_close(&doc);
    pdf_stream_close(&st);
    return (rc == PDF_OK) ? 0 : 1;
}
