/* pdf_annot.c
 *
 * Ver pdf_annot.h.
 */

#include "pdf_annot.h"
#include <stdio.h>

pdf_obj *pdf_annot_generate_highlight_appearance(pdf_arena *arena,
    const double *quads, int n_quads,
    double r, double g, double b, double alpha,
    long new_obj_num, double *out_bbox)
{
    double bx0, by0, bx1, by1;
    int i;
    long max_content, written;
    char *content;
    pdf_obj *bbox_arr, *gs0_dict, *extgstate_dict, *resources, *ap_dict;

    if (arena == NULL || quads == NULL || n_quads <= 0 || n_quads > PDF_ANNOT_MAX_QUADS ||
        out_bbox == NULL)
        return NULL;

    /* Bounding box union de todos los quads -- ver comentario grande
     * junto a la declaracion (pdf_annot.h) sobre por que /BBox = union
     * de quads hace que la matriz de Appearance Streams (norma 12.5.5)
     * resulte identidad: el contenido de abajo dibuja en coordenadas
     * ABSOLUTAS de pagina, no relativas a un origen local. */
    bx0 = quads[0]; bx1 = quads[0];
    by0 = quads[1]; by1 = quads[1];
    for (i = 0; i < n_quads; i++)
    {
        int k;
        for (k = 0; k < 4; k++)
        {
            double x = quads[i * 8 + k * 2];
            double y = quads[i * 8 + k * 2 + 1];
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    out_bbox[0] = bx0; out_bbox[1] = by0; out_bbox[2] = bx1; out_bbox[3] = by1;

    /* Presupuesto generoso por quad (8 numeros con hasta ~10
     * caracteres via "%.2f" mas los operadores de path -- ver
     * comentario de PDF_ANNOT_MAX_QUADS en el header sobre por que
     * 'n_quads' ya viene acotado) + una cabecera fija chica. Se aloca
     * en la arena (no en el stack): a diferencia de
     * pdf_form_generate_text_appearance (un solo Tj corto, buffer fijo
     * de 2048 alcanza siempre), un resaltado de una seleccion larga
     * puede traer muchas lineas/quads. */
    max_content = 64L + (long)n_quads * 200L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content, "/GS0 gs\n%.3f %.3f %.3f rg\n", r, g, b);
    for (i = 0; i < n_quads; i++)
    {
        /* Orden de almacenamiento en 'quads' (igual que /QuadPoints):
         * TL,TR,BL,BR. El PATH de relleno recorre el PERIMETRO real
         * del rectangulo -- TL->TR->BR->BL->cerrar -- NO el orden de
         * QuadPoints (TL->TR->BL->BR conectados en ese orden dibujan
         * un "moño" autointersectado, no el rectangulo). */
        double tlx = quads[i * 8 + 0], tly = quads[i * 8 + 1];
        double trx = quads[i * 8 + 2], try_ = quads[i * 8 + 3];
        double blx = quads[i * 8 + 4], bly = quads[i * 8 + 5];
        double brx = quads[i * 8 + 6], bry = quads[i * 8 + 7];

        written += (long)sprintf(content + written,
            "%.2f %.2f m\n%.2f %.2f l\n%.2f %.2f l\n%.2f %.2f l\nh\nf\n",
            tlx, tly, trx, try_, brx, bry, blx, bly);
    }

    /* /ExtGState con Multiply + opacidad -- el "look" estandar de
     * cualquier marcador real (blend Multiply deja ver el texto/
     * graficos de abajo, a diferencia de un relleno opaco encima). */
    gs0_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, gs0_dict, "ca", pdf_obj_new_real(arena, alpha));
    pdf_dict_set(arena, gs0_dict, "BM", pdf_obj_new_name(arena, "Multiply"));
    extgstate_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, extgstate_dict, "GS0", gs0_dict);
    resources = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, resources, "ExtGState", extgstate_dict);

    bbox_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, bx0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, by0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, bx1));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, by1));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "Type", pdf_obj_new_name(arena, "XObject"));
    pdf_dict_set(arena, ap_dict, "Subtype", pdf_obj_new_name(arena, "Form"));
    pdf_dict_set(arena, ap_dict, "BBox", bbox_arr);
    pdf_dict_set(arena, ap_dict, "Resources", resources);

    return pdf_obj_new_synthetic_stream(arena, ap_dict, (const unsigned char *)content,
                                         written, new_obj_num, 0);
}

pdf_obj *pdf_annot_new_highlight(pdf_arena *arena,
    const double *rect, const double *quads, int n_quads,
    double r, double g, double b,
    pdf_obj *ap_stream_obj)
{
    pdf_obj *annot, *rect_arr, *quad_arr, *c_arr, *ap_dict;
    int i;

    if (arena == NULL || rect == NULL || quads == NULL || n_quads <= 0 ||
        n_quads > PDF_ANNOT_MAX_QUADS || ap_stream_obj == NULL)
        return NULL;

    rect_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[0]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[1]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[2]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[3]));

    quad_arr = pdf_obj_new_array(arena, n_quads * 8);
    for (i = 0; i < n_quads * 8; i++)
        pdf_array_push(arena, quad_arr, pdf_obj_new_real(arena, quads[i]));

    c_arr = pdf_obj_new_array(arena, 3);
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, r));
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, g));
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, b));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "N", ap_stream_obj);

    annot = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, annot, "Type", pdf_obj_new_name(arena, "Annot"));
    pdf_dict_set(arena, annot, "Subtype", pdf_obj_new_name(arena, "Highlight"));
    pdf_dict_set(arena, annot, "Rect", rect_arr);
    pdf_dict_set(arena, annot, "QuadPoints", quad_arr);
    pdf_dict_set(arena, annot, "C", c_arr);
    pdf_dict_set(arena, annot, "CA", pdf_obj_new_real(arena, 1.0));
    pdf_dict_set(arena, annot, "F", pdf_obj_new_int(arena, 4));
    pdf_dict_set(arena, annot, "AP", ap_dict);

    return annot;
}
