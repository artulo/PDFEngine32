/* render_pdf_page.c
 *
 * Ejemplo end-to-end de PDFEngine32: abre un PDF real, resuelve
 * Root->Pages->Kids[0], decodifica el content stream (FlateDecode con
 * el inflate propio de pdf_filter.c), lo interpreta con pdf_content_run
 * y pinta el resultado en un pdf_bitmap usando pdf_render_op -- todo
 * dentro de las arenas del documento/pagina, con presupuesto acotado.
 *
 * Uso: render_pdf_page archivo.pdf salida.ppm [escala]
 *   escala: factor puntos-PDF -> pixeles (default 1.0 = 72 DPI nativo;
 *   usar p.ej. 4.0 para 288 DPI, calidad real de fotos de imprenta).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdf.h"

int main(int argc, char **argv)
{
    const char *in_path, *out_path;
    double scale;
    pdf_stream st;
    pdf_document doc;
    pdf_page page;
    pdf_xref_table xref;
    pdf_obj *page_obj, *mediabox, *resources;
    double page_w, page_h;
    pdf_bitmap bmp;
    pdf_render_device dev;
    const pdf_ledger *ledger;

    in_path  = (argc > 1) ? argv[1] : "rects.pdf";
    out_path = (argc > 2) ? argv[2] : "out.ppm";
    scale    = (argc > 3) ? atof(argv[3]) : 1.0; /* p.ej. 4.0 = 288 DPI, calidad de foto de imprenta */

    if (pdf_stream_open(&st, in_path) != PDF_OK)
    {
        printf("no se pudo abrir %s\n", in_path);
        return 1;
    }

    if (pdf_document_open(&doc, 128UL * 1024UL * 1024UL) != PDF_OK)
    {
        printf("no se pudo abrir el documento\n");
        return 1;
    }

    if (pdf_xref_load(&st, &doc.doc_arena, &xref) != PDF_OK)
    {
        printf("no se pudo leer xref/trailer\n");
        return 1;
    }

    /* BUG REAL ENCONTRADO (transparencia/shadings, fase 1): este
     * ejemplo llamaba pdf_document_get_page() ANTES de pdf_page_open()
     * y con doc.doc_arena en vez de page.page_arena -- el binding real
     * (harbour/pdf_hbfunc.c) siempre abre la pagina PRIMERO y resuelve
     * /Pages con la arena de la pagina. No se aislo la causa exacta de
     * la falla resultante (probablemente la pagina se resolvia con una
     * arena que se reseteaba/invalidaba mas tarde), pero alinear el
     * orden con el binding que SI esta confirmado funcionando arreglo
     * el problema -- confirmado contra tests/rects.pdf. */
    if (pdf_page_open(&page, &doc, 1) != PDF_OK)
    {
        printf("no se pudo abrir la pagina (motor)\n");
        return 1;
    }

    page_obj = pdf_document_get_page(&st, &xref, &page.page_arena, 0);
    if (page_obj == NULL)
    {
        printf("no se pudo resolver la pagina 1\n");
        pdf_page_close(&page);
        return 1;
    }

    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        double x0 = (mediabox->u.arr.items[0]->type == PDF_INT) ? (double)mediabox->u.arr.items[0]->u.integer : mediabox->u.arr.items[0]->u.real;
        double y0 = (mediabox->u.arr.items[1]->type == PDF_INT) ? (double)mediabox->u.arr.items[1]->u.integer : mediabox->u.arr.items[1]->u.real;
        double x1 = (mediabox->u.arr.items[2]->type == PDF_INT) ? (double)mediabox->u.arr.items[2]->u.integer : mediabox->u.arr.items[2]->u.real;
        double y1 = (mediabox->u.arr.items[3]->type == PDF_INT) ? (double)mediabox->u.arr.items[3]->u.integer : mediabox->u.arr.items[3]->u.real;
        page_w = x1 - x0;
        page_h = y1 - y0;
    }
    else
    {
        page_w = 612; page_h = 792; /* default carta, si no hay MediaBox */
    }

    printf("pagina: %.0fx%.0f pt\n", page_w, page_h);

    resources = pdf_page_get_resources(&st, &xref, &page.page_arena, page_obj);

    if (pdf_bitmap_create(&page.page_arena, (int)(page_w * scale), (int)(page_h * scale), &bmp) != PDF_OK)
    {
        printf("no se pudo crear el bitmap\n");
        return 1;
    }

    pdf_render_device_init(&dev, &bmp, page_h, scale, resources, &st, &xref, &page.page_arena);

    {
        pdf_buf content;
        int rc = pdf_page_get_content(&st, &xref, page_obj, &page.decode_arena, &content);

        if (rc == PDF_OK)
        {
            pdf_content_ops ops;
            ops.op           = pdf_render_op;
            ops.inline_image = NULL;
            ops.user         = &dev;
            pdf_content_run(content.data, content.len, &page.decode_arena, &ops);
        }
        else if (rc == PDF_ERR_NOTFOUND)
        {
            printf("la pagina no tiene /Contents (o esta vacia)\n");
        }
        else
        {
            printf("no se pudo decodificar el content stream (rc=%d)\n", rc);
        }
    }

    if (pdf_bitmap_write_ppm(&bmp, out_path) == PDF_OK)
        printf("bitmap escrito en %s\n", out_path);
    else
        printf("no se pudo escribir %s\n", out_path);

    printf("page_arena=%lu decode_arena=%lu bytes\n",
           pdf_arena_used(&page.page_arena), pdf_arena_used(&page.decode_arena));

    ledger = pdf_document_ledger(&doc);
    printf("pico total del documento: %lu de %lu bytes presupuestados\n",
           ledger->peak_bytes, ledger->budget_bytes);

    pdf_page_close(&page);
    pdf_document_close(&doc);
    pdf_stream_close(&st);

    return 0;
}
