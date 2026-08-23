/* pdf_text_extract.c -- ver pdf_text_extract.h */

#include <string.h>
#include <math.h>
#include "pdf_text_extract.h"
#include "pdf_render.h"
#include "pdf_bitmap.h"
#include "pdf_xref.h"

/* contexto pasado al gancho de extraccion via 'user' -- ver
 * pdf_render_device_set_text_extract_hook. */
typedef struct
{
    pdf_text_page *out;
} collect_ctx;

static void collect_hook(void *user, int unicode, int raw_code,
                          double x0_px, double y0_px,
                          double advance_px, double height_px,
                          double rotation_deg, int render_mode)
{
    collect_ctx *ctx = (collect_ctx *)user;
    pdf_text_page *out = ctx->out;
    pdf_text_glyph *g;

    if (out->n_glyphs >= out->glyph_cap)
        return; /* trunca silenciosamente, ver comentario en el header */

    g = &out->glyphs[out->n_glyphs];
    g->unicode      = unicode;
    g->raw_code     = raw_code;
    g->x0_px        = x0_px;
    g->y0_px        = y0_px;
    g->advance_px   = advance_px;
    g->height_px    = height_px;
    g->rotation_deg = rotation_deg;
    g->render_mode  = render_mode;
    g->is_space     = (unicode == ' ');
    out->n_glyphs++;
}

int pdf_text_extract_page(pdf_stream *st, const pdf_xref_table *xref,
                           pdf_page *page, pdf_obj *page_obj,
                           pdf_text_page *out)
{
    pdf_obj *mediabox, *cropbox, *resources, *rotate_obj;
    double media_x0, media_y0, media_x1, media_y1;
    double crop_x0, crop_y0, crop_x1, crop_y1;
    double page_w, page_h;
    int rotate, bmp_w, bmp_h;
    pdf_bitmap bmp;
    pdf_render_device dev;
    pdf_content_ops ops;
    pdf_buf content;
    collect_ctx ctx;
    int rc;

    if (st == NULL || xref == NULL || page == NULL || page_obj == NULL || out == NULL)
        return PDF_ERR_BADARG;
    if (out->glyphs == NULL || out->glyph_cap <= 0)
        return PDF_ERR_BADARG;

    out->n_glyphs = 0;

    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_REF)
        mediabox = pdf_xref_load_object(st, xref, mediabox->u.ref.num, &page->page_arena);
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        media_x0 = pdf_obj_num(mediabox->u.arr.items[0], 0.0);
        media_y0 = pdf_obj_num(mediabox->u.arr.items[1], 0.0);
        media_x1 = pdf_obj_num(mediabox->u.arr.items[2], 612.0);
        media_y1 = pdf_obj_num(mediabox->u.arr.items[3], 792.0);
        if (media_x1 < media_x0) { double t = media_x0; media_x0 = media_x1; media_x1 = t; }
        if (media_y1 < media_y0) { double t = media_y0; media_y0 = media_y1; media_y1 = t; }
    }
    else
    {
        media_x0 = 0.0; media_y0 = 0.0; media_x1 = 612.0; media_y1 = 792.0;
    }

    /* BUG REAL ENCONTRADO (Arturo, PDF real con /CropBox bien distinto
     * de /MediaBox) -- ver el mismo fix en pdf_hbfunc.c
     * HB_FUNC(PDF_RENDERTOHBITMAP) para el detalle completo; se repite
     * aca para que las posiciones de glyph que este modulo reporta
     * (seleccion/resaltado) coincidan con lo que realmente se renderiza. */
    crop_x0 = media_x0; crop_y0 = media_y0; crop_x1 = media_x1; crop_y1 = media_y1;
    cropbox = pdf_page_get_inherited(st, xref, &page->page_arena, page_obj, "CropBox");
    if (cropbox != NULL && cropbox->type == PDF_ARRAY && cropbox->u.arr.count == 4)
    {
        double cx0 = pdf_obj_num(cropbox->u.arr.items[0], media_x0);
        double cy0 = pdf_obj_num(cropbox->u.arr.items[1], media_y0);
        double cx1 = pdf_obj_num(cropbox->u.arr.items[2], media_x1);
        double cy1 = pdf_obj_num(cropbox->u.arr.items[3], media_y1);
        if (cx1 < cx0) { double t = cx0; cx0 = cx1; cx1 = t; }
        if (cy1 < cy0) { double t = cy0; cy0 = cy1; cy1 = t; }
        if (cx0 > media_x0) crop_x0 = cx0;
        if (cy0 > media_y0) crop_y0 = cy0;
        if (cx1 < media_x1) crop_x1 = cx1;
        if (cy1 < media_y1) crop_y1 = cy1;
    }
    if (crop_x1 <= crop_x0) { crop_x0 = media_x0; crop_x1 = media_x1; }
    if (crop_y1 <= crop_y0) { crop_y0 = media_y0; crop_y1 = media_y1; }

    page_w = crop_x1 - crop_x0;
    page_h = crop_y1 - crop_y0;
    if (page_w <= 0.0) page_w = 612.0;
    if (page_h <= 0.0) page_h = 792.0;

    /* BUG REAL ENCONTRADO (Arturo: "las letras estan volteadas", pagina
     * con /Rotate 90) -- ver pdf_render_device_set_rotation. Igual que
     * pdf_hbfunc.c: leer /Rotate (heredable) y aplicarlo, para que las
     * posiciones de glyph que este modulo reporta (usadas para
     * seleccion/resaltado en la UI) coincidan con lo que realmente se
     * ve en pantalla. */
    rotate = 0;
    rotate_obj = pdf_page_get_inherited(st, xref, &page->page_arena, page_obj, "Rotate");
    if (rotate_obj != NULL)
        rotate = pdf_normalize_rotation((int)pdf_obj_num(rotate_obj, 0.0));

    bmp_w = (int)page_w;
    bmp_h = (int)page_h;
    if (rotate == 90 || rotate == 270)
    {
        int tmp = bmp_w;
        bmp_w = bmp_h;
        bmp_h = tmp;
        out->page_width_pt  = page_h;
        out->page_height_pt = page_w;
    }
    else
    {
        out->page_width_pt  = page_w;
        out->page_height_pt = page_h;
    }

    resources = pdf_page_get_resources(st, xref, &page->page_arena, page_obj);

    /* bitmap throwaway: dev->bitmap == NULL no esta soportado en todo
     * el render todavia (ver etapa 6 diferida en el plan de esta
     * fase), asi que igual que pdf_hbfunc.c se pasa un bitmap real
     * aunque no nos interese ningun pixel de la salida. */
    if (pdf_bitmap_create(&page->page_arena, bmp_w, bmp_h, &bmp) != PDF_OK)
        return PDF_ERR_NOMEM;

    pdf_render_device_init(&dev, &bmp, page_h, 1.0, resources, st, xref, &page->page_arena);
    dev.page_width_pt = page_w;
    dev.crop_x0 = crop_x0;
    dev.crop_y0 = crop_y0;
    pdf_render_device_set_rotation(&dev, rotate);
    ctx.out = out;
    pdf_render_device_set_text_extract_hook(&dev, collect_hook, &ctx);

    rc = pdf_page_get_content(st, xref, page_obj, &page->decode_arena, &content);
    if (rc != PDF_OK)
        return PDF_OK; /* pagina sin /Contents: out->n_glyphs queda en 0, no es error */

    ops.op           = pdf_render_op;
    ops.inline_image  = NULL;
    ops.user          = &dev;
    pdf_content_run(content.data, content.len, &page->decode_arena, &ops);

    return PDF_OK;
}

/* ---- busqueda ------------------------------------------------------- */

#define PDF_TEXT_SEARCH_MAX_NEEDLE 256

static int utf8_decode(const char *s, int *cps, int max_cps)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;

    while (*p != '\0' && n < max_cps)
    {
        unsigned int cp;
        int extra;

        if ((*p & 0x80) == 0x00) { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { p++; continue; } /* byte de continuacion huerfano: se ignora */

        p++;
        while (extra > 0 && (*p & 0xC0) == 0x80)
        {
            cp = (cp << 6) | (*p & 0x3F);
            p++;
            extra--;
        }
        if (extra > 0) continue; /* secuencia UTF-8 cortada/invalida: se ignora */

        cps[n++] = (int)cp;
    }
    return n;
}

static int cp_fold(int cp, int case_sensitive)
{
    if (case_sensitive) return cp;
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
}

int pdf_text_search(const pdf_text_page *tp, const char *needle_utf8,
                     int case_sensitive, pdf_text_match *out_matches,
                     int max_matches, int *out_count)
{
    int needle_cp[PDF_TEXT_SEARCH_MAX_NEEDLE];
    int needle_len;
    int found = 0;
    int i;

    if (out_count != NULL)
        *out_count = 0;

    if (tp == NULL || needle_utf8 == NULL || needle_utf8[0] == '\0')
        return PDF_ERR_BADARG;

    needle_len = utf8_decode(needle_utf8, needle_cp, PDF_TEXT_SEARCH_MAX_NEEDLE);
    if (needle_len <= 0)
        return PDF_ERR_BADARG;

    for (i = 0; i + needle_len <= tp->n_glyphs; i++)
    {
        int k, match;
        double x0, y0, x1, y1;

        match = 1;
        for (k = 0; k < needle_len; k++)
        {
            if (cp_fold(tp->glyphs[i + k].unicode, case_sensitive) !=
                cp_fold(needle_cp[k], case_sensitive))
            {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;

        x0 = x1 = tp->glyphs[i].x0_px;
        y0 = y1 = tp->glyphs[i].y0_px;
        for (k = 0; k < needle_len; k++)
        {
            const pdf_text_glyph *g = &tp->glyphs[i + k];
            double gx1 = g->x0_px + g->advance_px;
            double gy_top = g->y0_px - g->height_px * PDF_TEXT_ASCENT_FRAC;
            double gy_bot = g->y0_px + g->height_px * PDF_TEXT_DESCENT_FRAC;

            if (g->x0_px < x0) x0 = g->x0_px;
            if (gx1 > x1) x1 = gx1;
            if (gy_top < y0) y0 = gy_top;
            if (gy_bot > y1) y1 = gy_bot;
        }

        if (found < max_matches)
        {
            out_matches[found].start_glyph_idx = i;
            out_matches[found].end_glyph_idx   = i + needle_len - 1;
            out_matches[found].x0 = x0;
            out_matches[found].y0 = y0;
            out_matches[found].x1 = x1;
            out_matches[found].y1 = y1;
        }
        found++;
    }

    if (out_count != NULL)
        *out_count = found;

    return PDF_OK;
}

int pdf_text_nearest_glyph(const pdf_text_page *tp, double x, double y)
{
    int i, best = -1;
    double best_score = 0.0;

    if (tp == NULL || tp->n_glyphs <= 0)
        return -1;

    for (i = 0; i < tp->n_glyphs; i++)
    {
        const pdf_text_glyph *g = &tp->glyphs[i];
        double gx_mid = g->x0_px + g->advance_px * 0.5;
        double dy = g->y0_px - y;
        double dx = gx_mid - x;
        /* factor 1e6 en Y: cualquier diferencia de linea, por chica que
         * sea, tiene que pesar mas que CUALQUIER diferencia de X posible
         * en una pagina real (paginas de miles de puntos de ancho
         * seguirian sin alcanzar a esta escala) -- ver comentario del
         * header. */
        double score = fabs(dy) * 1.0e6 + fabs(dx);

        if (best < 0 || score < best_score)
        {
            best = i;
            best_score = score;
        }
    }

    return best;
}
