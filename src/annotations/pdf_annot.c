/* pdf_annot.c
 *
 * Ver pdf_annot.h.
 */

#include "pdf_annot.h"
#include "pdf_afm.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

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

    max_content = 64L + (long)n_quads * 200L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content, "/GS0 gs\n%.3f %.3f %.3f rg\n", r, g, b);
    for (i = 0; i < n_quads; i++)
    {
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

/* --- Formas libres: /Line, /Square, /Circle, /Ink --------------------- */

/* Arma /BBox + /Resources (vacio -- ninguna de estas apariencias
 * referencia un recurso con nombre, a diferencia de Highlight que
 * necesita /ExtGState/GS0 para el blend Multiply) + envuelve
 * 'content'/'written' en un stream sintetico. Helper interno comun a
 * los 4 generadores de abajo -- mismo patron que el tramo final de
 * pdf_annot_generate_highlight_appearance, sin el /Resources. */
static pdf_obj *build_shape_appearance_stream(pdf_arena *arena,
    double bx0, double by0, double bx1, double by1,
    const char *content, long written, long new_obj_num)
{
    pdf_obj *bbox_arr, *ap_dict;

    bbox_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, bx0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, by0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, bx1));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, by1));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "Type", pdf_obj_new_name(arena, "XObject"));
    pdf_dict_set(arena, ap_dict, "Subtype", pdf_obj_new_name(arena, "Form"));
    pdf_dict_set(arena, ap_dict, "BBox", bbox_arr);

    return pdf_obj_new_synthetic_stream(arena, ap_dict, (const unsigned char *)content,
                                         written, new_obj_num, 0);
}

/* Dict comun a las 4 anotaciones nuevas: /Type /Annot /Subtype=subtype
 * /Rect=rect /C=[r g b] /CA=1.0 /F=4 /AP<</N=ap_stream_obj>>. El
 * llamador agrega despues cualquier clave extra especifica del subtipo
 * (/L para Line, /InkList para Ink). */
static pdf_obj *build_shape_annot_dict(pdf_arena *arena, const char *subtype,
    const double *rect, double r, double g, double b, pdf_obj *ap_stream_obj)
{
    pdf_obj *annot, *rect_arr, *c_arr, *ap_dict;

    rect_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[0]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[1]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[2]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[3]));

    c_arr = pdf_obj_new_array(arena, 3);
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, r));
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, g));
    pdf_array_push(arena, c_arr, pdf_obj_new_real(arena, b));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "N", ap_stream_obj);

    annot = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, annot, "Type", pdf_obj_new_name(arena, "Annot"));
    pdf_dict_set(arena, annot, "Subtype", pdf_obj_new_name(arena, subtype));
    pdf_dict_set(arena, annot, "Rect", rect_arr);
    pdf_dict_set(arena, annot, "C", c_arr);
    pdf_dict_set(arena, annot, "CA", pdf_obj_new_real(arena, 1.0));
    pdf_dict_set(arena, annot, "F", pdf_obj_new_int(arena, 4));
    pdf_dict_set(arena, annot, "AP", ap_dict);

    return annot;
}

/* Ancho/alto de la punta de flecha de Line -- fijos, ver pdf_annot.h. */
#define PDF_ANNOT_ARROW_LEN   10.0
#define PDF_ANNOT_ARROW_WIDTH  4.0

pdf_obj *pdf_annot_generate_line_appearance(pdf_arena *arena,
    double x1, double y1, double x2, double y2,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox)
{
    double dx, dy, len, ux, uy, px, py;
    double backx, backy, wing1x, wing1y, wing2x, wing2y;
    double bx0, by0, bx1, by1, margin;
    long max_content, written;
    char *content;

    if (arena == NULL || out_bbox == NULL)
        return NULL;

    dx = x2 - x1;
    dy = y2 - y1;
    len = sqrt(dx * dx + dy * dy);
    if (len < 0.001)
        return NULL; /* linea degenerada -- sin direccion para la flecha */

    ux = dx / len;
    uy = dy / len;
    px = -uy; /* perpendicular unitario */
    py = ux;

    backx = x2 - PDF_ANNOT_ARROW_LEN * ux;
    backy = y2 - PDF_ANNOT_ARROW_LEN * uy;
    wing1x = backx + PDF_ANNOT_ARROW_WIDTH * px;
    wing1y = backy + PDF_ANNOT_ARROW_WIDTH * py;
    wing2x = backx - PDF_ANNOT_ARROW_WIDTH * px;
    wing2y = backy - PDF_ANNOT_ARROW_WIDTH * py;

    bx0 = x1; bx1 = x1; by0 = y1; by1 = y1;
    if (x2 < bx0) bx0 = x2; if (x2 > bx1) bx1 = x2;
    if (y2 < by0) by0 = y2; if (y2 > by1) by1 = y2;
    if (wing1x < bx0) bx0 = wing1x; if (wing1x > bx1) bx1 = wing1x;
    if (wing1y < by0) by0 = wing1y; if (wing1y > by1) by1 = wing1y;
    if (wing2x < bx0) bx0 = wing2x; if (wing2x > bx1) bx1 = wing2x;
    if (wing2y < by0) by0 = wing2y; if (wing2y > by1) by1 = wing2y;
    margin = (line_width / 2.0) + 1.0;
    bx0 -= margin; by0 -= margin; bx1 += margin; by1 += margin;
    out_bbox[0] = bx0; out_bbox[1] = by0; out_bbox[2] = bx1; out_bbox[3] = by1;

    max_content = 512L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content,
        "%.3f w\n%.3f %.3f %.3f RG\n%.2f %.2f m\n%.2f %.2f l\nS\n"
        "%.3f %.3f %.3f rg\n%.2f %.2f m\n%.2f %.2f l\n%.2f %.2f l\nh\nf\n",
        line_width, r, g, b, x1, y1, x2, y2,
        r, g, b, x2, y2, wing1x, wing1y, wing2x, wing2y);

    return build_shape_appearance_stream(arena, bx0, by0, bx1, by1, content, written, new_obj_num);
}

pdf_obj *pdf_annot_new_line(pdf_arena *arena, const double *rect,
    double x1, double y1, double x2, double y2,
    double r, double g, double b, pdf_obj *ap_stream_obj)
{
    pdf_obj *annot, *l_arr, *le_arr;

    if (arena == NULL || rect == NULL || ap_stream_obj == NULL)
        return NULL;

    annot = build_shape_annot_dict(arena, "Line", rect, r, g, b, ap_stream_obj);
    if (annot == NULL)
        return NULL;

    l_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, l_arr, pdf_obj_new_real(arena, x1));
    pdf_array_push(arena, l_arr, pdf_obj_new_real(arena, y1));
    pdf_array_push(arena, l_arr, pdf_obj_new_real(arena, x2));
    pdf_array_push(arena, l_arr, pdf_obj_new_real(arena, y2));
    pdf_dict_set(arena, annot, "L", l_arr);

    /* Metadata de interop -- NO determina el dibujo real (eso lo hace
     * el /AP/N generado en pdf_annot_generate_line_appearance, que
     * dibuja la punta de flecha el mismo). */
    le_arr = pdf_obj_new_array(arena, 2);
    pdf_array_push(arena, le_arr, pdf_obj_new_name(arena, "None"));
    pdf_array_push(arena, le_arr, pdf_obj_new_name(arena, "OpenArrow"));
    pdf_dict_set(arena, annot, "LE", le_arr);

    return annot;
}

pdf_obj *pdf_annot_generate_square_appearance(pdf_arena *arena,
    double x0, double y0, double x1, double y1,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox)
{
    double margin;
    long max_content, written;
    char *content;

    if (arena == NULL || out_bbox == NULL || x1 <= x0 || y1 <= y0)
        return NULL;

    margin = line_width / 2.0;
    out_bbox[0] = x0 - margin; out_bbox[1] = y0 - margin;
    out_bbox[2] = x1 + margin; out_bbox[3] = y1 + margin;

    max_content = 256L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content,
        "%.3f w\n%.3f %.3f %.3f RG\n%.2f %.2f %.2f %.2f re\nS\n",
        line_width, r, g, b, x0, y0, x1 - x0, y1 - y0);

    return build_shape_appearance_stream(arena, out_bbox[0], out_bbox[1], out_bbox[2], out_bbox[3],
                                          content, written, new_obj_num);
}

pdf_obj *pdf_annot_new_square(pdf_arena *arena, const double *rect,
    double r, double g, double b, pdf_obj *ap_stream_obj)
{
    if (arena == NULL || rect == NULL || ap_stream_obj == NULL)
        return NULL;
    return build_shape_annot_dict(arena, "Square", rect, r, g, b, ap_stream_obj);
}

pdf_obj *pdf_annot_generate_circle_appearance(pdf_arena *arena,
    double x0, double y0, double x1, double y1,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox)
{
    double cx, cy, rx, ry, margin;
    const double k = 0.5522847498307936;
    long max_content, written;
    char *content;

    if (arena == NULL || out_bbox == NULL || x1 <= x0 || y1 <= y0)
        return NULL;

    cx = (x0 + x1) / 2.0; cy = (y0 + y1) / 2.0;
    rx = (x1 - x0) / 2.0; ry = (y1 - y0) / 2.0;

    margin = line_width / 2.0;
    out_bbox[0] = x0 - margin; out_bbox[1] = y0 - margin;
    out_bbox[2] = x1 + margin; out_bbox[3] = y1 + margin;

    max_content = 512L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content,
        "%.3f w\n%.3f %.3f %.3f RG\n"
        "%.2f %.2f m\n"
        "%.2f %.2f %.2f %.2f %.2f %.2f c\n"
        "%.2f %.2f %.2f %.2f %.2f %.2f c\n"
        "%.2f %.2f %.2f %.2f %.2f %.2f c\n"
        "%.2f %.2f %.2f %.2f %.2f %.2f c\n"
        "h\nS\n",
        line_width, r, g, b,
        cx + rx, cy,
        cx + rx, cy + ry * k,   cx + rx * k, cy + ry,   cx, cy + ry,
        cx - rx * k, cy + ry,   cx - rx, cy + ry * k,   cx - rx, cy,
        cx - rx, cy - ry * k,   cx - rx * k, cy - ry,   cx, cy - ry,
        cx + rx * k, cy - ry,   cx + rx, cy - ry * k,   cx + rx, cy);

    return build_shape_appearance_stream(arena, out_bbox[0], out_bbox[1], out_bbox[2], out_bbox[3],
                                          content, written, new_obj_num);
}

pdf_obj *pdf_annot_new_circle(pdf_arena *arena, const double *rect,
    double r, double g, double b, pdf_obj *ap_stream_obj)
{
    if (arena == NULL || rect == NULL || ap_stream_obj == NULL)
        return NULL;
    return build_shape_annot_dict(arena, "Circle", rect, r, g, b, ap_stream_obj);
}

pdf_obj *pdf_annot_generate_ink_appearance(pdf_arena *arena,
    const double *points, int n_points,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox)
{
    double bx0, by0, bx1, by1, margin;
    int i;
    long max_content, written;
    char *content;

    if (arena == NULL || points == NULL || out_bbox == NULL ||
        n_points < 2 || n_points > PDF_ANNOT_MAX_INK_POINTS)
        return NULL;

    bx0 = points[0]; bx1 = points[0];
    by0 = points[1]; by1 = points[1];
    for (i = 1; i < n_points; i++)
    {
        double x = points[i * 2];
        double y = points[i * 2 + 1];
        if (x < bx0) bx0 = x;
        if (x > bx1) bx1 = x;
        if (y < by0) by0 = y;
        if (y > by1) by1 = y;
    }
    margin = (line_width / 2.0) + 1.0;
    bx0 -= margin; by0 -= margin; bx1 += margin; by1 += margin;
    out_bbox[0] = bx0; out_bbox[1] = by0; out_bbox[2] = bx1; out_bbox[3] = by1;

    /* Presupuesto por punto: "%.2f %.2f l\n" con hasta ~10 caracteres
     * por numero -- ver comentario analogo en
     * pdf_annot_generate_highlight_appearance sobre por que 'n_points'
     * ya viene acotado (PDF_ANNOT_MAX_INK_POINTS). */
    max_content = 64L + (long)n_points * 32L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    written = (long)sprintf(content, "%.3f w\n%.3f %.3f %.3f RG\n%.2f %.2f m\n",
                             line_width, r, g, b, points[0], points[1]);
    for (i = 1; i < n_points; i++)
    {
        written += (long)sprintf(content + written, "%.2f %.2f l\n",
                                  points[i * 2], points[i * 2 + 1]);
    }
    written += (long)sprintf(content + written, "S\n");

    return build_shape_appearance_stream(arena, bx0, by0, bx1, by1, content, written, new_obj_num);
}

pdf_obj *pdf_annot_new_ink(pdf_arena *arena, const double *rect,
    const double *points, int n_points,
    double r, double g, double b, pdf_obj *ap_stream_obj)
{
    pdf_obj *annot, *stroke_arr, *inklist_arr;
    int i;

    if (arena == NULL || rect == NULL || points == NULL || ap_stream_obj == NULL ||
        n_points < 2 || n_points > PDF_ANNOT_MAX_INK_POINTS)
        return NULL;

    annot = build_shape_annot_dict(arena, "Ink", rect, r, g, b, ap_stream_obj);
    if (annot == NULL)
        return NULL;

    stroke_arr = pdf_obj_new_array(arena, n_points * 2);
    for (i = 0; i < n_points * 2; i++)
        pdf_array_push(arena, stroke_arr, pdf_obj_new_real(arena, points[i]));

    /* Un solo sub-array = un solo trazo por anotacion (alcance v1, ver
     * pdf_annot.h). */
    inklist_arr = pdf_obj_new_array(arena, 1);
    pdf_array_push(arena, inklist_arr, stroke_arr);
    pdf_dict_set(arena, annot, "InkList", inklist_arr);

    return annot;
}

/* --- Globo de tip: /FreeText --------------------------------------- */

#define PDF_ANNOT_TIP_FONT_SIZE   9.0
#define PDF_ANNOT_TIP_PAD         6.0
#define PDF_ANNOT_TIP_MAX_CONTENT_W 200.0
#define PDF_ANNOT_TIP_LINE_HEIGHT (PDF_ANNOT_TIP_FONT_SIZE * 1.3)
#define PDF_ANNOT_TIP_TAIL_H      14.0
#define PDF_ANNOT_TIP_TAIL_W      10.0
/* Bytes por linea ya envuelta -- interno, no expuesto en el header
 * (PDF_ANNOT_TIP_MAX_TEXT_LEN de 300 bytes totales nunca se acerca a
 * necesitar una linea mas larga que esto salvo un token unico sin
 * espacios -- ver el clamp de 'word_len' en wrap_tip_text). */
#define PDF_ANNOT_TIP_LINE_MAX_CHARS 128

static double word_width(const char *word, int len, double font_size)
{
    double w = 0.0;
    int i;
    for (i = 0; i < len; i++)
    {
        int code = (unsigned char)word[i];
        int em = pdf_afm_width("Helvetica", code);
        if (em < 0) em = 500;
        w += (double)em / 1000.0 * font_size;
    }
    return w;
}

/* Envuelve 'text' (wrap "greedy" por palabra) en hasta
 * PDF_ANNOT_TIP_MAX_LINES lineas de hasta PDF_ANNOT_TIP_LINE_MAX_CHARS
 * bytes cada una. Ademas de envolver automaticamente por ancho, respeta
 * saltos de linea EXPLICITOS que el usuario haya tipeado (`\r`, `\n`, o
 * `\r\n` -- el mensaje viene de un TEdit multilinea real, ver
 * TPdfTipEdit/StartTipEntry en pdf_viewer.prg) como cortes de linea
 * FORZADOS, incluyendo lineas en blanco intencionales ("\n\n"). 'lines'/
 * 'line_widths' ya vienen alocados por el llamador (arrays de tamanio
 * PDF_ANNOT_TIP_MAX_LINES). Devuelve la cantidad de lineas generadas
 * (siempre >= 1). */
static int wrap_tip_text(const char *text, double font_size, double max_w,
                          char lines[][PDF_ANNOT_TIP_LINE_MAX_CHARS], double *line_widths)
{
    int n_lines = 0;
    int pos = 0;
    int len = (int)strlen(text);
    double space_w = word_width(" ", 1, font_size);

    while (n_lines < PDF_ANNOT_TIP_MAX_LINES)
    {
        int line_len = 0;
        double line_w = 0.0;
        int first_word = 1;

        while (pos < len && text[pos] == ' ') pos++; /* espacios al inicio de la linea */

        if (pos >= len)
            break;

        /* Linea en blanco intencional -- consumir UN salto (o el par
         * \r\n / \n\r) y cerrar esta linea vacia sin buscar palabras. */
        if (text[pos] == '\r' || text[pos] == '\n')
        {
            char c = text[pos];
            pos++;
            if ((c == '\r' && pos < len && text[pos] == '\n') ||
                (c == '\n' && pos < len && text[pos] == '\r'))
                pos++;
            lines[n_lines][0] = 0;
            line_widths[n_lines] = 0.0;
            n_lines++;
            continue;
        }

        while (pos < len)
        {
            int word_start, word_len;
            double w;

            if (text[pos] == '\r' || text[pos] == '\n')
            {
                char c = text[pos];
                pos++;
                if ((c == '\r' && pos < len && text[pos] == '\n') ||
                    (c == '\n' && pos < len && text[pos] == '\r'))
                    pos++;
                break; /* corte forzado -- cierra esta linea */
            }
            if (text[pos] == ' ')
            {
                pos++;
                continue;
            }

            word_start = pos;
            while (pos < len && text[pos] != ' ' && text[pos] != '\r' && text[pos] != '\n')
                pos++;
            word_len = pos - word_start;
            /* Token unico sin espacios mas largo que una linea entera
             * (p.ej. una URL larga) -- cortarlo, no dejarlo crecer sin
             * limite (garantiza que 'pos' siempre avanza, ver arriba). */
            if (word_len >= PDF_ANNOT_TIP_LINE_MAX_CHARS)
                word_len = PDF_ANNOT_TIP_LINE_MAX_CHARS - 1;

            w = word_width(text + word_start, word_len, font_size);

            if (!first_word && line_w + space_w + w > max_w)
            {
                pos = word_start; /* esta palabra pasa a la proxima linea */
                break;
            }
            if (line_len + (first_word ? 0 : 1) + word_len >= PDF_ANNOT_TIP_LINE_MAX_CHARS)
            {
                pos = word_start;
                break;
            }

            if (!first_word)
            {
                lines[n_lines][line_len++] = ' ';
                line_w += space_w;
            }
            memcpy(lines[n_lines] + line_len, text + word_start, (size_t)word_len);
            line_len += word_len;
            line_w += w;
            first_word = 0;
        }

        lines[n_lines][line_len] = 0;
        line_widths[n_lines] = line_w;
        n_lines++;
    }

    /* Texto que no entro en PDF_ANNOT_TIP_MAX_LINES -- truncamiento
     * simple con "...", evita que el llamador tenga que lidiar con un
     * mensaje "perdido" silenciosamente sin ningun indicio visual. */
    if (pos < len && n_lines > 0)
    {
        int last = n_lines - 1;
        int cur_len = (int)strlen(lines[last]);
        if (cur_len + 3 < PDF_ANNOT_TIP_LINE_MAX_CHARS)
        {
            strcpy(lines[last] + cur_len, "...");
            line_widths[last] += word_width("...", 3, font_size);
        }
    }

    if (n_lines == 0)
    {
        lines[0][0] = 0;
        line_widths[0] = 0.0;
        n_lines = 1;
    }

    return n_lines;
}

/* Escapa '(' ')' '\' para un literal string de content stream -- mismo
 * criterio que pdf_form_generate_text_appearance (src/forms/pdf_form.c).
 * 'out' debe tener espacio para hasta in_len*2 bytes. Devuelve la
 * cantidad de bytes escritos en 'out' (sin el terminador). */
static int escape_pdf_text(const char *in, int in_len, char *out)
{
    int oi = 0, i;
    for (i = 0; i < in_len; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '(' || c == ')' || c == '\\')
            out[oi++] = '\\';
        out[oi++] = (char)c;
    }
    out[oi] = 0;
    return oi;
}

pdf_obj *pdf_annot_generate_tip_appearance(pdf_arena *arena,
    double anchor_x, double anchor_y, const char *text,
    long new_obj_num, double *out_bbox)
{
    char lines[PDF_ANNOT_TIP_MAX_LINES][PDF_ANNOT_TIP_LINE_MAX_CHARS];
    double line_widths[PDF_ANNOT_TIP_MAX_LINES];
    int n_lines, i;
    double box_content_w, box_w, box_h;
    double box_x0, box_y0, box_x1, box_y1;
    double tail_base_x;
    long max_content, written;
    char *content;
    char esc[PDF_ANNOT_TIP_LINE_MAX_CHARS * 2];
    pdf_obj *bbox_arr, *ap_dict, *resources, *font_dict, *font_entry;
    long text_len;

    if (arena == NULL || out_bbox == NULL || text == NULL)
        return NULL;
    text_len = (long)strlen(text);
    if (text_len <= 0 || text_len > PDF_ANNOT_TIP_MAX_TEXT_LEN)
        return NULL;

    n_lines = wrap_tip_text(text, PDF_ANNOT_TIP_FONT_SIZE, PDF_ANNOT_TIP_MAX_CONTENT_W, lines, line_widths);

    box_content_w = 0.0;
    for (i = 0; i < n_lines; i++)
        if (line_widths[i] > box_content_w) box_content_w = line_widths[i];
    if (box_content_w < 20.0) box_content_w = 20.0;

    box_w = box_content_w + 2.0 * PDF_ANNOT_TIP_PAD;
    box_h = (double)n_lines * PDF_ANNOT_TIP_LINE_HEIGHT + 2.0 * PDF_ANNOT_TIP_PAD;

    /* El globo crece hacia arriba y a la derecha desde el punto de
     * click -- ver DESIGN.md (sin clampeo contra bordes de pagina en
     * esta version). */
    box_x0 = anchor_x;
    box_y0 = anchor_y + PDF_ANNOT_TIP_TAIL_H;
    box_x1 = box_x0 + box_w;
    box_y1 = box_y0 + box_h;
    tail_base_x = box_x0 + 4.0;

    out_bbox[0] = anchor_x;
    out_bbox[1] = anchor_y;
    out_bbox[2] = box_x1;
    out_bbox[3] = box_y1;

    max_content = 512L + (long)n_lines * 400L;
    content = (char *)pdf_arena_alloc(arena, (size_t)max_content);
    if (content == NULL)
        return NULL;

    /* Globo (relleno + borde) y colita (mismo relleno+borde) -- 'b' =
     * cerrar+rellenar+trazar en un solo operador (T.32000-1 8.5.2.1). */
    written = (long)sprintf(content,
        "1.000 0.980 0.720 rg\n0.350 0.300 0.050 RG\n1.0 w\n"
        "%.2f %.2f %.2f %.2f re\nb\n"
        "%.2f %.2f m\n%.2f %.2f l\n%.2f %.2f l\nh\nb\n",
        box_x0, box_y0, box_w, box_h,
        anchor_x, anchor_y, tail_base_x, box_y0, tail_base_x + PDF_ANNOT_TIP_TAIL_W, box_y0);

    written += (long)sprintf(content + written, "BT\n/Helv %.2f Tf\n0 g\n", PDF_ANNOT_TIP_FONT_SIZE);
    for (i = 0; i < n_lines; i++)
    {
        double line_y = box_y1 - PDF_ANNOT_TIP_PAD - (double)(i + 1) * PDF_ANNOT_TIP_LINE_HEIGHT
                         + (PDF_ANNOT_TIP_LINE_HEIGHT - PDF_ANNOT_TIP_FONT_SIZE) / 2.0;
        escape_pdf_text(lines[i], (int)strlen(lines[i]), esc);
        written += (long)sprintf(content + written, "1 0 0 1 %.2f %.2f Tm\n(%s) Tj\n",
                                  box_x0 + PDF_ANNOT_TIP_PAD, line_y, esc);
    }
    written += (long)sprintf(content + written, "ET\n");

    bbox_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, out_bbox[0]));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, out_bbox[1]));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, out_bbox[2]));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, out_bbox[3]));

    /* /Resources propio, siempre -- a diferencia de
     * pdf_form_generate_text_appearance (que primero intenta /DR de
     * /Root/AcroForm), este stream es autocontenido: no hay ningun
     * /DA de campo existente del que depender. */
    font_entry = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, font_entry, "Type", pdf_obj_new_name(arena, "Font"));
    pdf_dict_set(arena, font_entry, "Subtype", pdf_obj_new_name(arena, "Type1"));
    pdf_dict_set(arena, font_entry, "BaseFont", pdf_obj_new_name(arena, "Helvetica"));
    font_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, font_dict, "Helv", font_entry);
    resources = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, resources, "Font", font_dict);

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "Type", pdf_obj_new_name(arena, "XObject"));
    pdf_dict_set(arena, ap_dict, "Subtype", pdf_obj_new_name(arena, "Form"));
    pdf_dict_set(arena, ap_dict, "BBox", bbox_arr);
    pdf_dict_set(arena, ap_dict, "Resources", resources);

    return pdf_obj_new_synthetic_stream(arena, ap_dict, (const unsigned char *)content,
                                         written, new_obj_num, 0);
}

pdf_obj *pdf_annot_new_freetext(pdf_arena *arena, const double *rect,
    const char *text, pdf_obj *ap_stream_obj)
{
    pdf_obj *annot, *rect_arr, *ic_arr, *ap_dict;

    if (arena == NULL || rect == NULL || text == NULL || ap_stream_obj == NULL)
        return NULL;

    rect_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[0]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[1]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[2]));
    pdf_array_push(arena, rect_arr, pdf_obj_new_real(arena, rect[3]));

    ic_arr = pdf_obj_new_array(arena, 3);
    pdf_array_push(arena, ic_arr, pdf_obj_new_real(arena, 1.0));
    pdf_array_push(arena, ic_arr, pdf_obj_new_real(arena, 0.98));
    pdf_array_push(arena, ic_arr, pdf_obj_new_real(arena, 0.72));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "N", ap_stream_obj);

    annot = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, annot, "Type", pdf_obj_new_name(arena, "Annot"));
    pdf_dict_set(arena, annot, "Subtype", pdf_obj_new_name(arena, "FreeText"));
    pdf_dict_set(arena, annot, "Rect", rect_arr);
    pdf_dict_set(arena, annot, "Contents", pdf_obj_new_string(arena, text, (long)strlen(text)));
    pdf_dict_set(arena, annot, "DA", pdf_obj_new_string(arena, "/Helv 9 Tf 0 g", (long)strlen("/Helv 9 Tf 0 g")));
    pdf_dict_set(arena, annot, "IC", ic_arr);
    pdf_dict_set(arena, annot, "CA", pdf_obj_new_real(arena, 1.0));
    pdf_dict_set(arena, annot, "F", pdf_obj_new_int(arena, 4));
    pdf_dict_set(arena, annot, "AP", ap_dict);

    return annot;
}
