/* pdf_render.c
 *
 * Ver pdf_render.h.
 */

#include "pdf_render.h"
#include "pdf_shading.h"
#include "pdf_form.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void pdf_render_device_init(pdf_render_device *dev, pdf_bitmap *bitmap,
                             double page_height_pt, double scale,
                             pdf_obj *resources,
                             pdf_stream *st, const pdf_xref_table *xref,
                             pdf_arena *arena)
{
    pdf_matrix id = PDF_MATRIX_IDENTITY_INIT;

    dev->bitmap         = bitmap;
    dev->page_height_pt = page_height_pt;
    dev->page_width_pt  = 0.0;
    dev->scale          = (scale > 0.0) ? scale : 1.0;
    dev->rotate         = 0;
    dev->crop_x0        = 0.0;
    dev->crop_y0        = 0.0;
    dev->gstate_top      = 0;

    dev->gstate_stack[0].ctm          = id;
    dev->gstate_stack[0].fill_color.r = 0.0;
    dev->gstate_stack[0].fill_color.g = 0.0;
    dev->gstate_stack[0].fill_color.b = 0.0;
    dev->gstate_stack[0].stroke_color = dev->gstate_stack[0].fill_color;
    dev->gstate_stack[0].line_width   = 1.0;
    dev->gstate_stack[0].fill_alpha   = 1.0;
    dev->gstate_stack[0].stroke_alpha = 1.0;
    dev->gstate_stack[0].blend_mode   = PDF_BLEND_NORMAL;
    dev->gstate_stack[0].clip_x0      = 0;
    dev->gstate_stack[0].clip_y0      = 0;
    dev->gstate_stack[0].clip_x1      = (bitmap != NULL) ? bitmap->width  : 0;
    dev->gstate_stack[0].clip_y1      = (bitmap != NULL) ? bitmap->height : 0;
    dev->gstate_stack[0].clip_mask        = NULL;
    dev->gstate_stack[0].clip_mask_stride = 0;
    dev->gstate_stack[0].smask_mask       = NULL;
    dev->gstate_stack[0].smask_stride     = 0;
    dev->gstate_stack[0].fill_pattern     = NULL;
    dev->gstate_stack[0].fill_pattern_matrix = id;
    dev->gstate_stack[0].fill_cs_is_pattern  = 0;
    dev->base_ctm      = id;
    dev->gstate_stack[0].font         = NULL;
    dev->gstate_stack[0].font_size    = 12.0;
    dev->gstate_stack[0].char_space   = 0.0;
    dev->gstate_stack[0].word_space   = 0.0;
    dev->gstate_stack[0].h_scale      = 1.0;
    dev->gstate_stack[0].leading      = 0.0;
    dev->gstate_stack[0].rise         = 0.0;
    dev->gstate_stack[0].render_mode  = 0;

    pdf_path_reset(&dev->cur_path);

    dev->text_matrix = id;
    dev->line_matrix = id;

    dev->resources = resources;
    dev->st        = st;
    dev->xref      = xref;
    dev->arena     = arena;
    dev->n_fonts   = 0;
    dev->form_depth = 0;
    dev->pending_clip = 0;
    dev->pending_clip_evenodd = 0;

    dev->glyph_draw      = NULL;
    dev->glyph_draw_user = NULL;

    dev->text_extract      = NULL;
    dev->text_extract_user = NULL;

    dev->font_cache_hook      = NULL;
    dev->font_cache_hook_user = NULL;
}

void pdf_render_device_set_font_cache_hook(pdf_render_device *dev,
                                            pdf_font_cache_fn fn, void *user)
{
    if (dev == NULL) return;
    dev->font_cache_hook      = fn;
    dev->font_cache_hook_user = user;
}

void pdf_render_device_set_text_extract_hook(pdf_render_device *dev,
                                              pdf_text_extract_fn fn, void *user)
{
    if (dev == NULL) return;
    dev->text_extract      = fn;
    dev->text_extract_user = user;
}

int pdf_normalize_rotation(int rotate_deg)
{
    int r = rotate_deg % 360;
    if (r < 0) r += 360;
    /* redondea al multiplo de 90 mas cercano en vez de exigir un valor
     * exacto -- /Rotate "deberia" ser siempre multiplo de 90 segun el
     * estandar, pero un PDF real con un valor invalido (p.ej. 45) no
     * tiene por que trabar el render: mismo criterio tolerante que el
     * resto del motor. */
    r = ((r + 45) / 90) * 90;
    if (r >= 360) r = 0;
    return r;
}

void pdf_render_device_set_rotation(pdf_render_device *dev, int rotate_deg)
{
    if (dev == NULL) return;
    dev->rotate = pdf_normalize_rotation(rotate_deg);
}

void pdf_render_device_set_glyph_hook(pdf_render_device *dev,
                                       pdf_glyph_draw_fn fn, void *user)
{
    if (dev == NULL) return;
    dev->glyph_draw      = fn;
    dev->glyph_draw_user = user;
}

static pdf_matrix mat_concat(pdf_matrix m, pdf_matrix ctm)
{
    pdf_matrix r;
    r.a = m.a * ctm.a + m.b * ctm.c;
    r.b = m.a * ctm.b + m.b * ctm.d;
    r.c = m.c * ctm.a + m.d * ctm.c;
    r.d = m.c * ctm.b + m.d * ctm.d;
    r.e = m.e * ctm.a + m.f * ctm.c + ctm.e;
    r.f = m.e * ctm.b + m.f * ctm.d + ctm.f;
    return r;
}

static pdf_point mat_transform(pdf_matrix m, double x, double y)
{
    pdf_point p;
    p.x = m.a * x + m.c * y + m.e;
    p.y = m.b * x + m.d * y + m.f;
    return p;
}

/* Inversa de una matriz afin 2x3 (ver DESIGN.md seccion 68, operador
 * 'sh'): usada para llevar coordenadas de pixel de vuelta al espacio
 * propio del shading (device_to_shading = inversa de shading_to_
 * device). Devuelve 0 (y deja '*out' sin tocar) si la matriz es
 * singular (determinante ~0, CTM degenerada) -- el llamador debe
 * tratarlo igual que cualquier otro fallo de carga (no pintar nada). */
static int mat_invert(pdf_matrix m, pdf_matrix *out)
{
    double det = m.a * m.d - m.b * m.c;
    if (det > -1e-12 && det < 1e-12)
        return 0;
    out->a =  m.d / det;
    out->b = -m.b / det;
    out->c = -m.c / det;
    out->d =  m.a / det;
    out->e = -(m.e * out->a + m.f * out->c);
    out->f = -(m.e * out->b + m.f * out->d);
    return 1;
}

static pdf_gstate *cur_gstate(pdf_render_device *dev)
{
    return &dev->gstate_stack[dev->gstate_top];
}

/* BUG REAL ENCONTRADO (Arturo: "las letras estan volteadas", pagina de
 * titulo con /Rotate 90 -- ver pdf_render_device_set_rotation) --
 * /Rotate nunca se aplicaba. Este es el UNICO punto de conversion
 * PDF-espacio-de-usuario -> pixel de todo el renderer (se llama desde
 * decenas de sitios: paths, texto, imagenes, shadings), asi que
 * ajustarlo aca alcanza para que TODO el pipeline rote junto, sin tocar
 * cada llamador.
 *
 * Sin rotar (de siempre): invierte Y nomas, bitmap = page_w x page_h
 * (escalados). Con rotate=R: primero se calcula el punto SIN rotar
 * (u,v) en un bitmap imaginario page_w x page_h, despues se rota ESE
 * punto R grados horario hacia un bitmap de salida con ancho/alto
 * intercambiados para 90/270 (el llamador es responsable de crear el
 * bitmap real ya con esas dimensiones finales, ver comentario en
 * pdf_render_device_set_rotation). */
static pdf_point to_pixel(pdf_render_device *dev, pdf_point p)
{
    pdf_point r;
    double u, v;
    double page_w_px, page_h_px;

    u = (p.x - dev->crop_x0) * dev->scale;
    v = (dev->page_height_pt - (p.y - dev->crop_y0)) * dev->scale;

    if (dev->rotate == 0)
    {
        r.x = u;
        r.y = v;
        return r;
    }

    page_w_px = dev->page_width_pt * dev->scale;
    page_h_px = dev->page_height_pt * dev->scale;

    if (dev->rotate == 90)
    {
        r.x = page_h_px - v;
        r.y = u;
    }
    else if (dev->rotate == 180)
    {
        r.x = page_w_px - u;
        r.y = page_h_px - v;
    }
    else /* 270 */
    {
        r.x = v;
        r.y = page_w_px - u;
    }

    return r;
}

/* Version matriz de to_pixel() de arriba -- mismos 4 casos de
 * dev->rotate, expresados como matriz afin en vez de funcion por punto,
 * para los sitios que necesitan COMPONER la transformacion
 * PDF-espacio-de-usuario -> pixel con otra matriz (patrones de shading,
 * shadings via 'sh', Form XObjects con /Matrix -- tres sitios en este
 * archivo que armaban esta matriz a mano SIN rotacion antes de este fix,
 * ver DESIGN.md: mismo bug que to_pixel(), pasado por alto en la
 * primera pasada porque son 3 usos con matriz en vez de punto-a-punto).
 * Convencion: x' = a*x + c*y + e ; y' = b*x + d*y + f (PDF_MATRIX_IDENTITY_INIT).
 * 'e'/'f' de cada caso llevan tambien el offset de /CropBox (crop_x0/
 * crop_y0, ver to_pixel() y el comentario de pdf_render_device_s) --
 * derivado sustituyendo u=(x-crop_x0)*s, v=ph-(y-crop_y0)*s en las
 * mismas 4 formulas de to_pixel(). */
/* Cuerpo de rotation_to_pixel_matrix() de abajo, parametrizado por
 * escalares en vez de un pdf_render_device* -- factorizado en la fase
 * de resaltado de texto para que pdf_render_topdown_to_native() (mas
 * abajo) pueda construir la MISMA matriz con scale=1.0 (el espacio que
 * usa pdf_text_extract_page(), no el de render a bitmap) y devolver su
 * inversa, sin duplicar la formula a mano en un segundo lugar donde
 * pudiera desincronizarse del renderer real. */
static pdf_matrix rotation_matrix_params(double scale, double page_w_pt, double page_h_pt,
                                          double crop_x0, double crop_y0, int rotate)
{
    pdf_matrix m;
    double s = scale;
    double pw = page_w_pt * s;
    double ph = page_h_pt * s;
    double cx = crop_x0 * s;
    double cy = crop_y0 * s;

    if (rotate == 90)
    {
        m.a = 0.0;  m.b = s;    m.c = s;    m.d = 0.0;  m.e = -cy; m.f = -cx;
    }
    else if (rotate == 180)
    {
        m.a = -s;   m.b = 0.0;  m.c = 0.0;  m.d = s;    m.e = pw + cx;  m.f = -cy;
    }
    else if (rotate == 270)
    {
        m.a = 0.0;  m.b = -s;   m.c = -s;   m.d = 0.0;  m.e = ph + cy;  m.f = pw + cx;
    }
    else /* 0 */
    {
        m.a = s;    m.b = 0.0;  m.c = 0.0;  m.d = -s;   m.e = -cx; m.f = ph + cy;
    }

    return m;
}

static pdf_matrix rotation_to_pixel_matrix(pdf_render_device *dev)
{
    return rotation_matrix_params(dev->scale, dev->page_width_pt, dev->page_height_pt,
                                   dev->crop_x0, dev->crop_y0, dev->rotate);
}

/* Inversa exacta de rotation_to_pixel_matrix()/to_pixel() con scale=1.0
 * -- el mismo espacio "topdown" que usa pdf_text_extract_page() para
 * ubicar glyphs (y por lo tanto el mismo que aSelRanges en
 * pdf_viewer.prg): puntos PDF, Y invertida, con /CropBox y /Rotate
 * NATIVO de la pagina ya aplicados. Convierte de vuelta al espacio de
 * usuario PDF NATIVO (bottom-up, sin rotar) que exige la norma para
 * /Rect y /QuadPoints -- ver HB_FUNC(PDF_ANNOT_ADDHIGHLIGHT) en
 * pdf_hbfunc.c, unico consumidor hoy. 'rotate_deg' es el /Rotate
 * NATIVO heredado de la pagina (normalizado a 0/90/180/270) -- NUNCA
 * ::nUserRotate (rotacion de VISTA del boton "Rotar", que aSelRanges
 * nunca incluye, ver comentario grande en el call site). Devuelve 0
 * (sin tocar '*out_x'/'*out_y') si la matriz resultara degenerada --
 * no deberia pasar con los 4 casos soportados, pero mat_invert() ya
 * expone ese chequeo y no hay razon para no propagarlo. */
int pdf_render_topdown_to_native(int rotate_deg,
                                  double crop_x0, double crop_y0,
                                  double page_w, double page_h,
                                  double topdown_x, double topdown_y,
                                  double *out_x, double *out_y)
{
    pdf_matrix fwd = rotation_matrix_params(1.0, page_w, page_h, crop_x0, crop_y0, rotate_deg);
    pdf_matrix inv;
    pdf_point p;

    if (!mat_invert(fwd, &inv))
        return 0;

    p = mat_transform(inv, topdown_x, topdown_y);
    *out_x = p.x;
    *out_y = p.y;
    return 1;
}

/* Se llama SIEMPRE al terminar de pintar (o descartar, para 'n') el
 * path actual -- si hubo un W/W* desde el ultimo pintado, intersecta
 * el clip vigente con el bounding box del path (aproximacion
 * rectangular del clip real, ver pdf_bitmap.h) y sincroniza el clip
 * activo del bitmap, ANTES de resetear dev->cur_path. El estandar PDF
 * exige que W/W* recien surtan efecto despues del operador de pintado
 * que les sigue -- por eso el clip pendiente se aplica aca y no en el
 * momento de W/W* mismo. */
static void finish_path(pdf_render_device *dev, pdf_gstate *gs)
{
    if (dev->pending_clip && dev->cur_path.n_points > 0)
    {
        double minx, miny, maxx, maxy;
        int i;

        minx = maxx = dev->cur_path.points[0].x;
        miny = maxy = dev->cur_path.points[0].y;
        for (i = 1; i < dev->cur_path.n_points; i++)
        {
            double x = dev->cur_path.points[i].x, y = dev->cur_path.points[i].y;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }

        {
            int nx0 = (int)minx, ny0 = (int)miny;
            int nx1 = (int)(maxx + 0.999), ny1 = (int)(maxy + 0.999);
            if (nx0 > gs->clip_x0) gs->clip_x0 = nx0;
            if (ny0 > gs->clip_y0) gs->clip_y0 = ny0;
            if (nx1 < gs->clip_x1) gs->clip_x1 = nx1;
            if (ny1 < gs->clip_y1) gs->clip_y1 = ny1;
            if (gs->clip_x1 < gs->clip_x0) gs->clip_x1 = gs->clip_x0;
            if (gs->clip_y1 < gs->clip_y0) gs->clip_y1 = gs->clip_y0;
        }

        pdf_bitmap_set_clip(dev->bitmap, gs->clip_x0, gs->clip_y0, gs->clip_x1, gs->clip_y1);

        /* clip no rectangular (ver DESIGN.md seccion 68): si el path de
         * clip no es un rectangulo alineado a ejes, se rasteriza a una
         * mascara de pagina completa y se AND-combina con la heredada
         * (interseccion real de clips anidados). BUG REAL ENCONTRADO Y
         * ARREGLADO: antes se pedia el buffer de pagina completa (los
         * bmp_width*bmp_height bytes de out_mask) INCONDICIONALMENTE, y
         * recien DESPUES pdf_raster_rasterize_clip_mask() detectaba
         * internamente el caso comun 're W n' (rectangulo simple) para
         * no llenarlo -- para entonces el buffer ya se habia pedido y
         * quedaba pegado para siempre en la arena, sin usarse. Un mapa
         * aeronautico denso (tests/mupdf_bug.cgiid=701945-
         * slow.rendering.pdf) hace 're W n' miles de veces para
         * delimitar zonas coloreadas -- cada uno tiraba una mascara de
         * pagina entera (~620 KB para este archivo) a la basura,
         * agotando el presupuesto de memoria antes de terminar de
         * dibujar el mapa (se veia incompleto, sin error visible: mismo
         * criterio tolerante de siempre). Ahora se chequea el caso
         * rectangulo ANTES de pedir el buffer -- el fast-path de
         * pdf_bitmap_set_clip (bbox, ya aplicado arriba) sigue
         * cubriendo ese caso sin gastar nada mas. */
        if (!pdf_raster_path_is_rect(&dev->cur_path))
        {
            unsigned char *new_mask = (unsigned char *)pdf_arena_alloc(dev->arena,
                (size_t)dev->bitmap->width * (size_t)dev->bitmap->height);
            if (new_mask != NULL &&
                pdf_raster_rasterize_clip_mask(&dev->cur_path,
                    dev->pending_clip_evenodd ? PDF_FILL_EVENODD : PDF_FILL_NONZERO,
                    dev->bitmap->width, dev->bitmap->height, new_mask))
            {
                if (gs->clip_mask != NULL)
                {
                    long i, n = (long)dev->bitmap->width * (long)dev->bitmap->height;
                    for (i = 0; i < n; i++)
                        new_mask[i] = (unsigned char)(((int)new_mask[i] * (int)gs->clip_mask[i]) / 255);
                }
                gs->clip_mask = new_mask;
                gs->clip_mask_stride = dev->bitmap->width;
            }
        }
    }
    pdf_bitmap_set_clip_mask(dev->bitmap, gs->clip_mask, gs->clip_mask_stride);
    dev->pending_clip = 0;
    pdf_path_reset(&dev->cur_path);
}

/* Escala aproximada del CTM (para convertir 'w', en espacio de usuario,
 * a un ancho de trazo en pixeles) -- raiz cuadrada del valor absoluto
 * del determinante, que da un factor de escala uniforme razonable
 * incluso si el CTM tiene escalas distintas en X/Y o rotacion. */
static double ctm_scale(pdf_matrix m)
{
    double det = m.a * m.d - m.b * m.c;
    if (det < 0) det = -det;
    return sqrt(det);
}

/* BUG REAL ENCONTRADO Y ARREGLADO (transparencia/shadings, fase 1 --
 * ver DESIGN.md seccion 51 para el estado ANTERIOR de este fix, y
 * seccion 68 para el arreglo real): el compositor Porter-Duff de
 * pdf_bitmap.c (alpha, 16 blend modes, soft mask, knockout) ya estaba
 * completo desde hace tiempo pero nunca se invocaba desde aca -- en
 * vez de blending real, se usaba un gate binario (pintar 100% opaco
 * o no pintar nada) como paliativo. Ahora se sincroniza el estado de
 * pintado real del bitmap (opacity/blend_mode/soft_mask) antes de
 * cada operacion de pintado, y se deja que pdf_bitmap_set_pixel_
 * coverage haga la composicion real -- ver pdf_bitmap.c:403-465. */
static void sync_paint_state(pdf_render_device *dev, pdf_gstate *gs, int is_stroke)
{
    double alpha = is_stroke ? gs->stroke_alpha : gs->fill_alpha;
    pdf_bitmap_set_opacity(dev->bitmap, alpha);
    pdf_bitmap_set_blend_mode(dev->bitmap, gs->blend_mode);
    pdf_bitmap_set_soft_mask(dev->bitmap, gs->smask_mask, gs->smask_stride);
}

/* --- relleno con patron de shading (PatternType 2, ver DESIGN.md
 * seccion 68 -- 'scn' con colorspace /Pattern) -------------------------- */

typedef struct
{
    pdf_shading shading;
    pdf_matrix  device_to_pattern;
} pattern_fill_ctx;

static int pattern_pixel_fn(void *user, int x, int y, pdf_color *out)
{
    pattern_fill_ctx *ctx = (pattern_fill_ctx *)user;
    double dx = (double)x + 0.5, dy = (double)y + 0.5;
    double px = dx * ctx->device_to_pattern.a + dy * ctx->device_to_pattern.c + ctx->device_to_pattern.e;
    double py = dx * ctx->device_to_pattern.b + dy * ctx->device_to_pattern.d + ctx->device_to_pattern.f;
    return pdf_shading_eval(&ctx->shading, px, py, out);
}

/* Rellena dev->cur_path con el color plano vigente, o con el patron de
 * shading activo (gs->fill_pattern) si hay uno -- unifica el camino
 * comun a todos los operadores de relleno (f, F, f-evenodd, B,
 * B-evenodd, b, b-evenodd). El espacio del patron es el CTM "de base"
 * del stream de contenido actual (dev->base_ctm), NO el CTM vigente en
 * el momento del relleno -- ver comentario junto a 'base_ctm' en
 * pdf_render.h (evita el bug clasico de que el patron "se mueva" con
 * el path que lo usa). */
static void fill_current_path(pdf_render_device *dev, pdf_gstate *gs, pdf_fill_rule rule)
{
    sync_paint_state(dev, gs, 0);

    if (gs->fill_pattern != NULL)
    {
        pdf_obj *sh_obj = pdf_dict_get(gs->fill_pattern, "Shading");
        pattern_fill_ctx ctx;

        if (sh_obj != NULL && sh_obj->type == PDF_REF)
            sh_obj = pdf_parser_load_object(dev->st, dev->xref, sh_obj->u.ref.num, dev->arena);

        if (sh_obj != NULL &&
            pdf_shading_load(dev->st, dev->xref, sh_obj, dev->arena, &ctx.shading) == PDF_OK &&
            ctx.shading.kind != PDF_SHADING_UNSUPPORTED)
        {
            pdf_matrix to_pixel_mat, pattern_to_pixel;

            to_pixel_mat = rotation_to_pixel_matrix( dev );

            pattern_to_pixel = mat_concat(mat_concat(gs->fill_pattern_matrix, dev->base_ctm), to_pixel_mat);
            if (mat_invert(pattern_to_pixel, &ctx.device_to_pattern))
            {
                pdf_raster_fill_path_shaded(dev->bitmap, &dev->cur_path, rule, pattern_pixel_fn, &ctx);
                return;
            }
        }
        /* patron sin /Shading valido o CTM degenerada: cae al color de
         * relleno plano vigente (mismo comportamiento que este motor
         * ya tenia ANTES de soportar patrones -- el nombre de patron
         * se descartaba y quedaba el color plano, ver comentario en
         * 'scn' mas arriba), en vez de dejar el area sin pintar. */
    }

    pdf_raster_fill_path(dev->bitmap, &dev->cur_path, gs->fill_color, rule);
}

/* --- soft mask de ExtGState (/SMask, tipo Luminosity) --------------------
 * Ver DESIGN.md seccion 68. El grupo /G se renderiza aparte (fondo NEGRO,
 * segun el estandar -- luminosidad 0 = totalmente enmascarado fuera de lo
 * que el grupo realmente pinte) y se convierte a una mascara de 8 bits via
 * luminancia estandar (0.3R+0.59G+0.11B). Tipo /Alpha (mascara por canal
 * alfa del grupo en vez de luminosidad) queda fuera de alcance -- se
 * ignora esa clave de /SMask, mismo criterio tolerante que el resto del
 * motor (degradar a "sin soft mask" en vez de un valor inventado). */
static long g_pdf_smask_debug_count = 0;

static void load_soft_mask_group(pdf_render_device *dev, pdf_gstate *gs, pdf_obj *smask_dict)
{
    const char *stype;
    pdf_obj *g_obj;
    pdf_bitmap mask_bmp;
    unsigned char *lum;
    long i, n;

    if (getenv("PDF_ARENA_DEBUG") != NULL)
    {
        g_pdf_smask_debug_count++;
        if (g_pdf_smask_debug_count <= 20 || (g_pdf_smask_debug_count % 1000) == 0)
            fprintf(stderr, "PDF_SMASK_DEBUG: llamada #%ld a load_soft_mask_group, form_depth=%d\n",
                    g_pdf_smask_debug_count, dev->form_depth);
    }

    gs->smask_mask = NULL;
    gs->smask_stride = 0;

    stype = pdf_dict_get_name(smask_dict, "S");
    if (stype == NULL || strcmp(stype, "Luminosity") != 0)
        return;

    g_obj = pdf_dict_get(smask_dict, "G");
    if (g_obj != NULL && g_obj->type == PDF_REF)
        g_obj = pdf_parser_load_object(dev->st, dev->xref, g_obj->u.ref.num, dev->arena);
    if (g_obj == NULL || g_obj->type != PDF_STREAM ||
        g_obj->u.stm.raw_length <= 0 || g_obj->u.stm.raw_length > 500L * 1024L * 1024L)
        return;

    if (pdf_bitmap_create(dev->arena, dev->bitmap->width, dev->bitmap->height, &mask_bmp) != PDF_OK)
        return;
    /* fondo negro por default (backdrop de luminosidad, ver /BC --
     * no soportado: siempre negro, el caso comun) -- pdf_bitmap_create
     * arranca en blanco, se pisa entero antes de dibujar el grupo. */
    memset(mask_bmp.pixels, 0, (size_t)mask_bmp.width * (size_t)mask_bmp.height * 3);

    {
        pdf_buf raw_content;
        unsigned char *raw;
        int got_ok = 0;

        raw = (unsigned char *)pdf_arena_alloc(dev->arena, (size_t)g_obj->u.stm.raw_length);
        if (raw != NULL)
        {
            long got;
            const char *filter;

            pdf_stream_seek(dev->st, g_obj->u.stm.raw_offset);
            got = pdf_stream_read(dev->st, raw, g_obj->u.stm.raw_length);
            if (dev->xref != NULL && dev->xref->crypt.active)
                got = pdf_crypt_decrypt(&dev->xref->crypt, g_obj->u.stm.obj_num, g_obj->u.stm.obj_gen, raw, got);
            filter = pdf_dict_get_name(g_obj, "Filter");

            if (filter == NULL) { raw_content.data = raw; raw_content.len = got; got_ok = 1; }
            else if (strcmp(filter, "FlateDecode") == 0)
                got_ok = (pdf_filter_flate(dev->arena, raw, got, 0, &raw_content) == PDF_OK);
            else if (strcmp(filter, "ASCII85Decode") == 0)
                got_ok = (pdf_filter_ascii85(dev->arena, raw, got, &raw_content) == PDF_OK);
        }

        if (got_ok && dev->form_depth < 4)
        {
            pdf_obj *g_res = pdf_dict_get(g_obj, "Resources");
            pdf_obj *saved_resources = dev->resources;
            pdf_obj *matrix_obj = pdf_dict_get(g_obj, "Matrix");
            pdf_bitmap *saved_bitmap = dev->bitmap;
            pdf_matrix saved_base_ctm = dev->base_ctm;

            if (g_res != NULL && g_res->type == PDF_REF)
                g_res = pdf_parser_load_object(dev->st, dev->xref, g_res->u.ref.num, dev->arena);
            if (g_res != NULL)
                dev->resources = g_res;

            if (dev->gstate_top + 1 < PDF_RENDER_MAX_GSTATE_DEPTH)
            {
                dev->gstate_stack[dev->gstate_top + 1] = *gs;
                dev->gstate_top++;
                gs = cur_gstate(dev);

                if (matrix_obj != NULL && matrix_obj->type == PDF_ARRAY && matrix_obj->u.arr.count == 6)
                {
                    pdf_matrix fm;
                    fm.a = pdf_obj_num(matrix_obj->u.arr.items[0], 1.0);
                    fm.b = pdf_obj_num(matrix_obj->u.arr.items[1], 0.0);
                    fm.c = pdf_obj_num(matrix_obj->u.arr.items[2], 0.0);
                    fm.d = pdf_obj_num(matrix_obj->u.arr.items[3], 1.0);
                    fm.e = pdf_obj_num(matrix_obj->u.arr.items[4], 0.0);
                    fm.f = pdf_obj_num(matrix_obj->u.arr.items[5], 0.0);
                    gs->ctm = mat_concat(fm, gs->ctm);
                }
                /* sin soft mask propio ni patron heredado dentro del
                 * grupo de mascara -- evita recursion rara (una mascara
                 * que a su vez dependiera de otra mascara). */
                gs->smask_mask = NULL; gs->smask_stride = 0;
                gs->fill_pattern = NULL; gs->fill_cs_is_pattern = 0;
                gs->fill_alpha = 1.0; gs->stroke_alpha = 1.0; gs->blend_mode = PDF_BLEND_NORMAL;

                dev->bitmap = &mask_bmp;
                dev->base_ctm = gs->ctm;
                dev->form_depth++;
                {
                    pdf_content_ops nested_ops;
                    nested_ops.op          = pdf_render_op;
                    nested_ops.inline_image = NULL;
                    nested_ops.user        = dev;
                    pdf_content_run(raw_content.data, raw_content.len, dev->arena, &nested_ops);
                }
                dev->form_depth--;
                dev->base_ctm = saved_base_ctm;
                dev->bitmap = saved_bitmap;

                dev->gstate_top--;
                gs = cur_gstate(dev);
            }

            dev->resources = saved_resources;
        }
    }

    lum = (unsigned char *)pdf_arena_alloc(dev->arena, (size_t)mask_bmp.width * (size_t)mask_bmp.height);
    if (lum == NULL)
        return;

    n = (long)mask_bmp.width * (long)mask_bmp.height;
    for (i = 0; i < n; i++)
    {
        unsigned char r = mask_bmp.pixels[i * 3];
        unsigned char g = mask_bmp.pixels[i * 3 + 1];
        unsigned char b = mask_bmp.pixels[i * 3 + 2];
        int y = (int)(0.30 * r + 0.59 * g + 0.11 * b + 0.5);
        if (y > 255) y = 255;
        lum[i] = (unsigned char)y;
    }

    gs->smask_mask = lum;
    gs->smask_stride = mask_bmp.width;
}

/* --- resolucion de fuentes por nombre desde /Resources ------------------ */

static pdf_obj *find_font_dict(pdf_render_device *dev, const char *name)
{
    pdf_obj *font_res, *ref;

    if (dev->resources == NULL)
        return NULL;

    font_res = pdf_dict_get(dev->resources, "Font");
    if (font_res == NULL)
        return NULL;

    /* BUG REAL ENCONTRADO (render de fuentes real, confirmado contra
     * Utilization_and_efficiency_of_ground_gra.pdf): /Font dentro de
     * /Resources puede venir ELLA MISMA como referencia indirecta
     * ("5 0 R"), no solo cada entrada individual dentro de ella -- sin
     * resolverla, el 'pdf_dict_get(font_res, name)' de abajo trata un
     * PDF_REF como si fuera un PDF_DICT (interpreta el union con el
     * miembro equivocado) y nunca encuentra ninguna clave, dejando
     * TODO el texto de la pagina sin fuente resuelta (gs->font==NULL
     * todo el documento -- ancho generico 500 en vez del real, y
     * sustitucion de sistema sin is_bold/is_serif reales). Invisible
     * con la caja placeholder de antes (no dependia de la fuente para
     * verse "razonable"), pero con contornos reales el ancho/estilo
     * equivocado se nota -- ver PDF_GLYPH_DEBUG mas abajo, agregado
     * durante la investigacion de este bug (mismo criterio que
     * PDF_JPX_DEBUG en pdf_jpx.c). */
    if (font_res->type == PDF_REF)
        font_res = pdf_parser_load_object(dev->st, dev->xref, font_res->u.ref.num, dev->arena);
    if (font_res == NULL || font_res->type != PDF_DICT)
        return NULL;

    ref = pdf_dict_get(font_res, name);
    if (ref == NULL)
        return NULL;

    if (ref->type == PDF_REF)
        return pdf_parser_load_object(dev->st, dev->xref, ref->u.ref.num, dev->arena);

    return ref; /* dict de fuente inline (raro, pero valido) */
}

/* PDF_GLYPH_DEBUG=1 (variable de entorno): imprime a stderr, por cada
 * 'Tf', si la fuente se pudo resolver -- diagnostico agregado durante
 * la investigacion del bug de /Font indirecto de arriba, mismo
 * criterio que PDF_JPX_DEBUG en pdf_jpx.c. */
static pdf_font *resolve_font(pdf_render_device *dev, const char *name)
{
    int i;
    pdf_obj *fdict;
    pdf_font *font;
    int debug = (getenv("PDF_GLYPH_DEBUG") != NULL);

    for (i = 0; i < dev->n_fonts; i++)
        if (strcmp(dev->font_cache[i].name, name) == 0)
            return dev->font_cache[i].font;

    if (dev->n_fonts >= PDF_RENDER_FONT_CACHE_SIZE)
        return NULL; /* cache lleno: esta pagina usa demasiadas fuentes distintas */

    fdict = find_font_dict(dev, name);
    if (fdict == NULL)
    {
        if (debug) fprintf(stderr, "PDF_GLYPH_DEBUG: Tf /%s -- find_font_dict no encontro el recurso\n", name);
        return NULL;
    }

    /* Cache de documento (ver pdf_font_cache_fn, pdf_render.h) -- si
     * esta activo, 'fdict' (puntero ESTABLE gracias al cache de
     * pdf_xref_load_object) es la clave; la fuente parseada persiste
     * mas alla de este render, asi que un zoom/pagina siguiente que
     * use la MISMA fuente no vuelve a parsear el programa TrueType/CFF
     * embebido. Sin el gancho (NULL), comportamiento identico a
     * siempre: pdf_font_load() directo, sin persistir. */
    if (dev->font_cache_hook != NULL)
    {
        font = dev->font_cache_hook(dev->font_cache_hook_user, fdict, dev->st, dev->xref, dev->arena);
        if (debug)
            fprintf(stderr, "PDF_GLYPH_DEBUG: Tf /%s -- font_cache_hook %s base_font=%s\n",
                    name, (font != NULL) ? "hit/load OK" : "fallo", (font != NULL) ? font->base_font : "?");
    }
    else
    {
        font = (pdf_font *)pdf_arena_alloc(dev->arena, sizeof(pdf_font));
        if (font != NULL)
        {
            int rc = pdf_font_load(fdict, font, dev->st, dev->xref, dev->arena);
            if (debug)
                fprintf(stderr, "PDF_GLYPH_DEBUG: Tf /%s -- pdf_font_load rc=%d base_font=%s\n",
                        name, rc, (rc == PDF_OK) ? font->base_font : "?");
            if (rc != PDF_OK)
                font = NULL;
        }
    }

    if (font == NULL)
        return NULL;

    i = dev->n_fonts;
    strncpy(dev->font_cache[i].name, name, PDF_FONT_NAME_MAX - 1);
    dev->font_cache[i].name[PDF_FONT_NAME_MAX - 1] = 0;
    dev->font_cache[i].font = font;
    dev->n_fonts++;

    return font;
}

/* --- render de glyphs reales (contornos TrueType) ------------------------
 *
 * Reemplaza la caja placeholder por el contorno real del glyph -- ver
 * DESIGN.md, ronda "render de fuentes real". Fuente embebida
 * (/FontFile2, via pdf_font->embedded_ttf) tiene prioridad; si no hay
 * o no se pudo parsear, se sustituye con un .ttf real del sistema
 * (pdf_ttf_find_system_font/_symbol_font, ver pdf_ttf.h -- sin GDI).
 * Si NINGUNA de las dos resuelve un glyph dibujable, se cae a la caja
 * placeholder de siempre (tolerante, nunca deja texto en blanco). */

/* case-insensitive, mismo criterio que name_contains_ci en pdf_font.c
 * (sin helper compartido entre modulos, ver pdf_ttf.c). */
static int name_has_ci(const char *hay, const char *needle)
{
    size_t hn, nn, i;

    if (hay == NULL) return 0;
    hn = strlen(hay);
    nn = strlen(needle);
    if (nn == 0 || nn > hn) return 0;

    for (i = 0; i + nn <= hn; i++)
    {
        size_t j;
        int ok = 1;
        for (j = 0; j < nn; j++)
        {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* Resuelve que pdf_ttf_font + glyph index dibujar para 'code' (fuente
 * embebida primero, sustituto de sistema despues). Devuelve 0 si
 * ninguno de los dos caminos resuelve un glyph -- el llamador cae a la
 * caja placeholder. */
static int resolve_glyph(const pdf_font *font, int code, int resolved_unicode,
                          const pdf_ttf_font **out_font, int *out_gid)
{
    const char *base_name = (font != NULL) ? font->base_font : "";
    int is_symbol = name_has_ci(base_name, "wingding") || name_has_ci(base_name, "webding")
                  || name_has_ci(base_name, "symbol");

    if (font != NULL && font->has_embedded_ttf)
    {
        int gid = font->is_cid
            ? ((font->cid_to_gid != NULL) ? (int)font->cid_to_gid[code & 0xFFFF] : (code & 0xFFFF))
            : pdf_ttf_gid_for_unicode(&font->embedded_ttf, resolved_unicode);

        if (gid > 0)
        {
            *out_font = &font->embedded_ttf;
            *out_gid  = gid;
            return 1;
        }
        /* sin match en el cmap embebido: seguir probando sustitucion
         * de sistema abajo en vez de rendirse -- mejor un glyph
         * aproximado (fuente distinta) que ninguno. */
    }

    if (is_symbol)
    {
        const pdf_ttf_font *sysf = pdf_ttf_find_system_symbol_font(base_name);
        int gid = (sysf != NULL) ? pdf_ttf_gid_for_symbol_code(sysf, code) : 0;
        if (sysf != NULL && gid > 0) { *out_font = sysf; *out_gid = gid; return 1; }
        return 0;
    }

    {
        int is_bold   = (font != NULL) ? font->is_bold   : 0;
        int is_italic = (font != NULL) ? font->is_italic : 0;
        int is_serif  = (font != NULL) ? font->is_serif  : 0;
        const pdf_ttf_font *sysf = pdf_ttf_find_system_font(base_name, is_bold, is_italic, is_serif);
        int gid = (sysf != NULL) ? pdf_ttf_gid_for_unicode(sysf, resolved_unicode) : 0;
        if (sysf != NULL && gid > 0) { *out_font = sysf; *out_gid = gid; return 1; }
    }

    return 0;
}

/* adaptador pdf_ttf_moveto_fn/pdf_ttf_lineto_fn -> pdf_path, aplicando
 * la matriz de render de texto (trm) + flip de eje Y (to_pixel) a cada
 * punto (ya normalizado a em por pdf_ttf_glyph_outline). 'xscale'
 * reescala el eje X ANTES de aplicar trm -- ver comentario junto a
 * pdf_ttf_glyph_advance_em (pdf_ttf.h): sin esto, un glyph angosto en
 * la fuente ORIGINAL del PDF (avance declarado en /Widths) dibujado
 * con el ancho NATURAL, mas grande, de una fuente sustituta de
 * sistema queda pisando al siguiente caracter -- 1.0 = sin cambio
 * (fuente embebida real, o fuente sustituta sin info de avance). */
typedef struct glyph_path_ctx_s
{
    pdf_render_device *dev;
    pdf_matrix          trm;
    pdf_path           *path;
    double              xscale;
} glyph_path_ctx;

static void glyph_moveto(void *user, double x, double y)
{
    glyph_path_ctx *ctx = (glyph_path_ctx *)user;
    pdf_point p = to_pixel(ctx->dev, mat_transform(ctx->trm, x * ctx->xscale, y));
    pdf_path_moveto(ctx->path, p.x, p.y);
}

static void glyph_lineto(void *user, double x, double y)
{
    glyph_path_ctx *ctx = (glyph_path_ctx *)user;
    pdf_point p = to_pixel(ctx->dev, mat_transform(ctx->trm, x * ctx->xscale, y));
    pdf_path_lineto(ctx->path, p.x, p.y);
}

/* adaptador pdf_cff_curveto_fn -> pdf_path_curveto (curvas cubicas
 * reales, a diferencia de TrueType que ya llega aplanada a lineas
 * desde pdf_ttf_glyph_outline -- pdf_path_curveto hace su propio
 * aplanado, ver pdf_path.h). Misma transformacion punto a punto que
 * glyph_moveto/glyph_lineto. */
static void glyph_curveto(void *user, double x1, double y1, double x2, double y2, double x3, double y3)
{
    glyph_path_ctx *ctx = (glyph_path_ctx *)user;
    pdf_point p1 = to_pixel(ctx->dev, mat_transform(ctx->trm, x1 * ctx->xscale, y1));
    pdf_point p2 = to_pixel(ctx->dev, mat_transform(ctx->trm, x2 * ctx->xscale, y2));
    pdf_point p3 = to_pixel(ctx->dev, mat_transform(ctx->trm, x3 * ctx->xscale, y3));
    pdf_path_curveto(ctx->path, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
}

/* --- mostrar texto -------------------------------------------------------
 * Dibuja el contorno real de cada glyph (ver resolve_glyph arriba), con
 * fallback a una caja rellena aproximando la caja de tinta cuando no
 * hay ningun glyph resoluble (ver pdf_render.h) -- la posicion/avance/
 * kerning son correctos en ambos casos. */

static void show_text_bytes(pdf_render_device *dev, pdf_gstate *gs,
                             const unsigned char *data, long len)
{
    long i;
    /* estatica en vez de local: pdf_path es un struct grande de
     * capacidad fija (~130KB, ver pdf_path.h) -- evitar ese tamanio en
     * la pila de una funcion que se llama por cada operador Tj/TJ/'/"
     * (el motor es de un solo hilo, sin reentrancia, igual criterio
     * que la cache de fuentes de sistema en pdf_ttf.c). */
    static pdf_path glyph_path;
    /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 62):
     * fuentes compuestas (/Type0, is_cid=1) usan 2 BYTES por
     * caracter (un CID de 16 bits, big-endian bajo /Encoding
     * /Identity-H) en vez de 1 -- sin esto, se leia "medio caracter"
     * por vez, produciendo texto con letras mezcladas/duplicadas sin
     * relacion aparente con el original (confirmado con un PDF
     * real).
     *
     * BUG REAL ENCONTRADO Y ARREGLADO (variante de 1 byte -- ver el
     * comentario grande junto a 'is_cid_one_byte' en pdf_font.h):
     * asumir SIEMPRE 2 bytes para toda fuente Type0 es a su vez
     * incorrecto para una variante real (BaseFont terminado en
     * "OneByteIdentityH") que declara Identity-H pero en realidad
     * codifica 1 byte por caracter -- confirmado con tests/
     * Los_Kajchas_y_los_proyectos_de_industria.pdf ("texto ilegible",
     * patron de barras: se estaban combinando pares de bytes en CIDs
     * sin sentido). */
    int cid_step = (gs->font != NULL && gs->font->is_cid && !gs->font->is_cid_one_byte) ? 2 : 1;

    for (i = 0; i + cid_step <= len; i += cid_step)
    {
        int code = (cid_step == 2) ? ((int)data[i] << 8) | (int)data[i + 1] : (int)data[i];
        double w0 = pdf_font_get_width(gs->font, code) / 1000.0;
        double tx;
        int resolved_unicode = pdf_font_get_unicode(gs->font, code);

        /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 56): esta
         * condicion decide si HAY que dibujar tinta para este
         * caracter -- "code != ' '" asumia que el codigo crudo del
         * PDF (0-255) coincide con el codepoint ASCII/ANSI real, cosa
         * cierta para /Encoding estandar pero FALSA para fuentes con
         * subconjunto embebido sin /Encoding (ver seccion 55): ahi el
         * codigo es un indice arbitrario que el generador reutiliza
         * de forma compacta, y CUALQUIER caracter (no solo el
         * espacio real) puede terminar en la posicion numerica 32 por
         * pura coincidencia -- confirmado en boiler_light_up_
         * procedure.pdf, donde el digito '3' de ">30%" tiene
         * justamente codigo 32, y se dibujaba como si fuera un
         * espacio en blanco (invisible), dejando ">  0%" en vez de
         * ">30%". El chequeo ahora usa el codepoint UNICODE ya
         * resuelto (identico a 'code' cuando no hay /ToUnicode --
         * mismo comportamiento de siempre para el caso normal) en vez
         * del byte crudo, para decidir si HAY que pintar. El word
         * spacing (Tw, mas abajo) es un caso aparte -- el estandar
         * PDF (32000-1, 9.3.3) dice que se aplica siempre al BYTE 32
         * literal sin importar que represente en la fuente, asi que
         * esa comparacion sigue usando 'code' crudo a proposito, sin
         * tocar. */
        /* matriz de render de texto: [Tfs*Th 0 0 Tfs 0 Trise] * Tm * CTM --
         * y las magnitudes derivadas (origen en pixeles, alto, ancho de
         * avance, rotacion) se calculan SIEMPRE aca afuera, no solo
         * cuando el caracter se va a pintar: el gancho de extraccion de
         * texto (dev->text_extract, ver DESIGN.md seccion 70) necesita
         * esto para TODO caracter mostrado, incluido texto invisible
         * (Tr 3/7 -- comun en capas OCR sobre un escaneo) y espacios,
         * que el gate de pintado de mas abajo justamente salta. Esta
         * elevacion es una reubicacion pura del calculo que antes vivia
         * solo dentro de 'if (dev->glyph_draw != NULL)' -- no cambia
         * ningun valor ni el comportamiento de pintado existente. */
        pdf_matrix font_scale;
        pdf_matrix trm;
        pdf_point p0, p1;
        pdf_point p_up, p_right;
        double pixel_height, advance_width_px, rotation_deg;

        font_scale.a = gs->font_size * gs->h_scale;
        font_scale.b = 0.0;
        font_scale.c = 0.0;
        font_scale.d = gs->font_size;
        font_scale.e = 0.0;
        font_scale.f = gs->rise;

        trm = mat_concat(font_scale, dev->text_matrix);
        trm = mat_concat(trm, gs->ctm);

        p0 = to_pixel(dev, mat_transform(trm, 0.0, 0.0));

        p_up    = to_pixel(dev, mat_transform(trm, 0.0, 1.0));
        p_right = to_pixel(dev, mat_transform(trm, w0,  0.0));
        {
            double dx = p_up.x - p0.x, dy = p_up.y - p0.y;
            double adx = p_right.x - p0.x, ady = p_right.y - p0.y;
            pixel_height = sqrt(dx*dx + dy*dy);
            advance_width_px = sqrt(adx*adx + ady*ady);
            /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md
             * seccion 61): angulo de rotacion del texto, en
             * grados, sentido antihorario tal como se ve en
             * pantalla -- se mide sobre el vector de AVANCE
             * (adx,ady, la direccion del eje X del espacio de
             * texto ya proyectada a pixeles). Se niega 'ady'
             * antes de atan2 porque el espacio de pixeles tiene Y
             * creciendo hacia abajo (ya con el flip de eje
             * aplicado, ver mas arriba), mientras que "antihorario
             * visual" (la convencion que espera GDI en
             * lfEscapement) corresponde a angulos positivos en un
             * sistema Y-hacia-arriba estandar. Para el caso normal
             * (texto horizontal, adx>0 ady=0) esto da 0.0 grados
             * exacto, sin cambio de comportamiento. */
            rotation_deg = (advance_width_px > 0.0001)
                ? atan2(-ady, adx) * (180.0 / 3.14159265358979323846)
                : 0.0;
            if (pixel_height <= 0.0)
                pixel_height = gs->font_size;
        }

        if (dev->text_extract != NULL)
            dev->text_extract(dev->text_extract_user, resolved_unicode, code,
                               p0.x, p0.y, advance_width_px, pixel_height,
                               rotation_deg, gs->render_mode);

        if (gs->render_mode != 3 && gs->render_mode != 7 && resolved_unicode != ' ' && code != '\0')
        {
            if (dev->glyph_draw != NULL)
            {
                /* gancho real (p.ej. GDI en el binding de Windows): le
                 * pasamos el origen del baseline en pixeles, un tamanio
                 * de fuente aproximado en pixeles, y el ancho de avance
                 * exacto que este caracter debe ocupar (todo por
                 * magnitud de mapear vectores unitarios a traves de trm
                 * -- exacto si no hay rotacion ni escala no uniforme). */
                dev->glyph_draw(dev->glyph_draw_user, (unsigned char)(code & 0xFF),
                                 resolved_unicode,
                                 p0.x, p0.y,
                                 pixel_height, advance_width_px,
                                 rotation_deg,
                                 gs->font ? gs->font->is_bold   : 0,
                                 gs->font ? gs->font->is_italic : 0,
                                 gs->font ? gs->font->is_serif  : 0,
                                 gs->fill_color,
                                 gs->font ? gs->font->base_font : "");
            }
            else
            {
                int drew_real = 0;
                const pdf_ttf_font *gfont = NULL;
                int gid = 0;

                /* Fuente embebida TrueType (/FontFile2) o sustituto de
                 * sistema tienen prioridad (ver resolve_glyph). El CFF
                 * embebido (/FontFile3) se prueba SOLO si esto no
                 * resuelve nada -- en la practica una fuente dada trae
                 * uno u otro, nunca ambos, asi que el orden no importa
                 * para el caso normal; es solo para no duplicar el
                 * intento de sustituto de sistema cuando el CFF ya
                 * dibujo el glyph. */
                if (resolve_glyph(gs->font, code, resolved_unicode, &gfont, &gid))
                {
                    glyph_path_ctx ctx;
                    /* reescala X para que el glyph (posiblemente de una
                     * fuente sustituta de proporciones distintas) ocupe
                     * el ancho REAL declarado en /Widths del PDF (w0),
                     * no su ancho natural -- ver comentario junto a
                     * pdf_ttf_glyph_advance_em en pdf_ttf.h. Clampeado
                     * para no desaparecer (glyph angosto real, p.ej.
                     * espacios) ni deformarse en exceso (metrica rara/
                     * incompleta) si w0 y el ancho natural difieren
                     * demasiado. */
                    double natural_w = pdf_ttf_glyph_advance_em(gfont, gid);
                    double xscale = (natural_w > 0.001 && w0 > 0.001) ? (w0 / natural_w) : 1.0;
                    if (xscale < 0.4) xscale = 0.4;
                    if (xscale > 2.5) xscale = 2.5;

                    pdf_path_reset(&glyph_path);
                    ctx.dev = dev; ctx.trm = trm; ctx.path = &glyph_path; ctx.xscale = xscale;
                    pdf_ttf_glyph_outline(gfont, gid, glyph_moveto, glyph_lineto, &ctx, 0);
                    if (glyph_path.n_subpaths > 0)
                    {
                        sync_paint_state(dev, gs, 0);
                        pdf_raster_fill_path_aa(dev->bitmap, &glyph_path, gs->fill_color, PDF_FILL_NONZERO);
                        drew_real = 1;
                    }
                }

                /* Contornos CFF reales (/FontFile3, ver pdf_cff.h) --
                 * a diferencia del TrueType embebido, no leemos el
                 * ancho natural del glyph (el CFF no expone eso de
                 * forma barata sin correr el charstring dos veces), asi
                 * que xscale queda en 1.0: el ancho declarado en
                 * /Widths del PDF y el ancho real del glyph de ESTA
                 * MISMA fuente embebida deberian coincidir por
                 * construccion (a diferencia del caso de sustitucion de
                 * sistema, donde SI pueden diferir bastante). */
                if (!drew_real && gs->font != NULL && gs->font->has_embedded_cff)
                {
                    int cff_gid = pdf_cff_gid_for_code(&gs->font->embedded_cff, code);
                    if (cff_gid > 0)
                    {
                        glyph_path_ctx ctx;
                        pdf_path_reset(&glyph_path);
                        ctx.dev = dev; ctx.trm = trm; ctx.path = &glyph_path; ctx.xscale = 1.0;
                        pdf_cff_glyph_outline(&gs->font->embedded_cff, cff_gid,
                                               glyph_moveto, glyph_lineto, glyph_curveto, &ctx);
                        if (glyph_path.n_subpaths > 0)
                        {
                            sync_paint_state(dev, gs, 0);
                            pdf_raster_fill_path_aa(dev->bitmap, &glyph_path, gs->fill_color, PDF_FILL_NONZERO);
                            drew_real = 1;
                        }
                    }
                }

                if (!drew_real)
                {
                    /* caja aproximada: de x=0.08..w0-0.08 (margen lateral tipico
                     * de un glyph dentro de su ancho de avance) y de y=0..0.62
                     * (aprox. altura de x-height/cap-height promedio) en espacio
                     * de texto normalizado (1.0 = un em) -- fallback cuando no
                     * hay fuente embebida NI sustituto de sistema resoluble
                     * (ver resolve_glyph arriba y pdf_render.h). */
                    p1 = to_pixel(dev, mat_transform(trm, (w0 > 0.16 ? w0 - 0.08 : w0 * 0.5), 0.62));
                    p0 = to_pixel(dev, mat_transform(trm, 0.08, 0.0));

                    {
                        int x0 = (int)(p0.x + 0.5), y0 = (int)(p0.y + 0.5);
                        int x1 = (int)(p1.x + 0.5), y1 = (int)(p1.y + 0.5);
                        pdf_bitmap_fill_rect(dev->bitmap, x0, y0, x1, y1, gs->fill_color);
                    }
                }
            }
        }

        /* T.32000-1 9.3.3: Tw (word spacing) se aplica SOLO al byte 32
         * literal en codigos de UN byte -- nunca a codigos de varios
         * bytes (fuentes CID, cid_step==2), sin importar que
         * represente ese CID. */
        tx = (w0 * gs->font_size + gs->char_space +
              ((cid_step == 1 && code == ' ') ? gs->word_space : 0.0)) * gs->h_scale;

        {
            pdf_matrix translate;
            translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
            translate.e = tx;  translate.f = 0.0;
            dev->text_matrix = mat_concat(translate, dev->text_matrix);
        }
    }
}

static void show_text_obj(pdf_render_device *dev, pdf_gstate *gs, pdf_obj *str)
{
    if (str == NULL || str->type != PDF_STRING)
        return;
    show_text_bytes(dev, gs, (const unsigned char *)str->u.str.data, str->u.str.len);
}

/* Dibuja un Form XObject ('xobj', ya confirmado /Subtype /Form) --
 * extraido del manejo inline del operador 'Do' para que
 * pdf_render_draw_annotations() (dibujo de apariencias de campos
 * AcroForm /AP/N, tambien Form XObjects por norma) lo pueda reusar
 * directo, sin pasar por /Resources/XObject de ninguna pagina. El
 * llamador es responsable de que 'dev's gstate actual (cur_gstate(dev))
 * tenga el CTM de colocacion correcto ANTES de llamar -- para un 'Do'
 * normal es el CTM vigente del content stream; para una anotacion es
 * la matriz que mapea /BBox al /Rect (ver pdf_render_draw_annotations).
 *
 * BUG REAL ENCONTRADO Y ARREGLADO (via este mismo trabajo de AcroForm,
 * no reportado por Arturo -- se noto al revisar este codigo para poder
 * reusarlo): la norma exige recortar el contenido del form a su /BBox
 * (transformado por /Matrix), pero esto NUNCA se implemento -- un form
 * XObject normal podia "pintar afuera" de su BBox declarado. Aplica
 * tanto a forms normales como (mas notorio) a apariencias de campos:
 * sin este recorte, un checkbox chico con una apariencia mal armada
 * puede mancurar contenido de campos vecinos. Se agrega el recorte acá
 * (transformar las 4 esquinas de /BBox a pixeles, interseccion con el
 * clip vigente -- mismo mecanismo que W/W*, ver finish_path) y TAMBIEN
 * se corrige un bug relacionado que ya existia: al terminar el form, el
 * clip del bitmap NUNCA se resincronizaba con el gstate restaurado
 * (a diferencia del operador 'Q', que si lo hace) -- un form que
 * usara W/W* internamente dejaba el clip mas angosto "pegado" para
 * todo lo que se dibujara despues en el content stream exterior. */
static void draw_form_xobject(pdf_render_device *dev, pdf_obj *xobj)
{
    pdf_gstate *gs = cur_gstate(dev);
    pdf_buf raw_content;
    unsigned char *raw;
    int got_ok = 0;

    /* Stream SINTETICO (AcroForm, ver pdf_obj_new_synthetic_stream en
     * pdf_object.h): contenido generado en memoria (apariencia de un
     * campo de texto recien editado, ver pdf_form.c), nunca leido de
     * ningun archivo -- ya viene decodificado/sin filtros, listo tal
     * cual para pdf_content_run. Se salta el read+decrypt+filtro
     * normal por completo. */
    if (xobj->u.stm.synthetic_data != NULL)
    {
        raw_content.data = (unsigned char *)xobj->u.stm.synthetic_data;
        raw_content.len = xobj->u.stm.synthetic_len;
        got_ok = 1;
    }
    else
    {
    raw = (unsigned char *)pdf_arena_alloc(dev->arena, (size_t)xobj->u.stm.raw_length);
    if (raw != NULL)
    {
        long got;
        const char *filter;

        pdf_stream_seek(dev->st, xobj->u.stm.raw_offset);
        got = pdf_stream_read(dev->st, raw, xobj->u.stm.raw_length);
        if (dev->xref != NULL && dev->xref->crypt.active)
            got = pdf_crypt_decrypt(&dev->xref->crypt, xobj->u.stm.obj_num, xobj->u.stm.obj_gen, raw, got);
        filter = pdf_dict_get_name(xobj, "Filter");

        if (filter == NULL)
        {
            raw_content.data = raw; raw_content.len = got;
            got_ok = 1;
        }
        else if (strcmp(filter, "FlateDecode") == 0)
        {
            got_ok = (pdf_filter_flate(dev->arena, raw, got, 0, &raw_content) == PDF_OK);
        }
        else if (strcmp(filter, "ASCII85Decode") == 0)
        {
            got_ok = (pdf_filter_ascii85(dev->arena, raw, got, &raw_content) == PDF_OK);
        }
    }
    }

    if (got_ok)
    {
        pdf_obj *form_res = pdf_dict_get(xobj, "Resources");
        pdf_obj *saved_resources = dev->resources;
        pdf_obj *matrix_obj = pdf_dict_get(xobj, "Matrix");

        if (form_res != NULL && form_res->type == PDF_REF)
            form_res = pdf_parser_load_object(dev->st, dev->xref, form_res->u.ref.num, dev->arena);
        if (form_res != NULL)
            dev->resources = form_res; /* si no tiene, se sigue usando el de la pagina (saved_resources) */

        if (dev->gstate_top + 1 < PDF_RENDER_MAX_GSTATE_DEPTH)
        {
            dev->gstate_stack[dev->gstate_top + 1] = *gs;
            dev->gstate_top++;
            gs = cur_gstate(dev);

            if (matrix_obj != NULL && matrix_obj->type == PDF_ARRAY && matrix_obj->u.arr.count == 6)
            {
                pdf_matrix fm;
                fm.a = pdf_obj_num(matrix_obj->u.arr.items[0], 1.0);
                fm.b = pdf_obj_num(matrix_obj->u.arr.items[1], 0.0);
                fm.c = pdf_obj_num(matrix_obj->u.arr.items[2], 0.0);
                fm.d = pdf_obj_num(matrix_obj->u.arr.items[3], 1.0);
                fm.e = pdf_obj_num(matrix_obj->u.arr.items[4], 0.0);
                fm.f = pdf_obj_num(matrix_obj->u.arr.items[5], 0.0);
                gs->ctm = mat_concat(fm, gs->ctm);
            }

            /* Recorte a /BBox (norma 8.10.2), transformado por el CTM
             * ya con /Matrix aplicado -- 4 esquinas (no solo 2: el CTM
             * puede rotar/sesgar) a pixeles, interseccion con el clip
             * heredado, mismo patron que finish_path con W/W*. */
            {
                pdf_obj *bbox_obj = pdf_dict_get(xobj, "BBox");
                if (bbox_obj != NULL && bbox_obj->type == PDF_ARRAY && bbox_obj->u.arr.count == 4)
                {
                    double bx0 = pdf_obj_num(bbox_obj->u.arr.items[0], 0.0);
                    double by0 = pdf_obj_num(bbox_obj->u.arr.items[1], 0.0);
                    double bx1 = pdf_obj_num(bbox_obj->u.arr.items[2], 0.0);
                    double by1 = pdf_obj_num(bbox_obj->u.arr.items[3], 0.0);
                    pdf_point c0 = to_pixel(dev, mat_transform(gs->ctm, bx0, by0));
                    pdf_point c1 = to_pixel(dev, mat_transform(gs->ctm, bx1, by0));
                    pdf_point c2 = to_pixel(dev, mat_transform(gs->ctm, bx1, by1));
                    pdf_point c3 = to_pixel(dev, mat_transform(gs->ctm, bx0, by1));
                    double minx = c0.x, maxx = c0.x, miny = c0.y, maxy = c0.y;
                    pdf_point pts[3]; pts[0]=c1; pts[1]=c2; pts[2]=c3;
                    {
                        int k;
                        for (k = 0; k < 3; k++)
                        {
                            if (pts[k].x < minx) minx = pts[k].x;
                            if (pts[k].x > maxx) maxx = pts[k].x;
                            if (pts[k].y < miny) miny = pts[k].y;
                            if (pts[k].y > maxy) maxy = pts[k].y;
                        }
                    }
                    {
                        int nx0 = (int)minx, ny0 = (int)miny;
                        int nx1 = (int)(maxx + 0.999), ny1 = (int)(maxy + 0.999);
                        if (nx0 > gs->clip_x0) gs->clip_x0 = nx0;
                        if (ny0 > gs->clip_y0) gs->clip_y0 = ny0;
                        if (nx1 < gs->clip_x1) gs->clip_x1 = nx1;
                        if (ny1 < gs->clip_y1) gs->clip_y1 = ny1;
                        if (gs->clip_x1 < gs->clip_x0) gs->clip_x1 = gs->clip_x0;
                        if (gs->clip_y1 < gs->clip_y0) gs->clip_y1 = gs->clip_y0;
                    }
                    pdf_bitmap_set_clip(dev->bitmap, gs->clip_x0, gs->clip_y0, gs->clip_x1, gs->clip_y1);
                }
            }

            {
                pdf_matrix saved_base_ctm = dev->base_ctm;
                pdf_bitmap *saved_bitmap = dev->bitmap;
                pdf_bitmap group_bmp;
                int using_group = 0;
                double group_alpha = gs->fill_alpha;
                int group_blend = gs->blend_mode;

                dev->base_ctm = gs->ctm; /* espacio de patrones DENTRO de este form -- ver campo en pdf_render.h */

                /* Grupo de transparencia (/Group << /S
                 * /Transparency >>, ver DESIGN.md seccion 68):
                 * el contenido del form se renderiza en un
                 * bitmap RGBA temporal APARTE (aislado) y se
                 * compone de vuelta con ca/BM del gstate
                 * vigente en ESTE 'Do' -- asi un rectangulo
                 * semitransparente que se solapa a si mismo
                 * DENTRO del grupo se ve como un solo blend,
                 * no uno por cada primitiva (que es lo que
                 * pasaba, y sigue pasando fuera de un grupo,
                 * sin esto). Acotado a form_depth<4 (mas
                 * estricto que el limite general de
                 * recursion de forms, <16): cada grupo cuesta
                 * width*height*4 bytes de arena, y una cadena
                 * profunda de grupos anidados podria agotar
                 * el presupuesto -- mas alla de la cota se
                 * corre el form directo sin aislar (degrada,
                 * no crashea). */
                if (dev->form_depth < 4)
                {
                    pdf_obj *group = pdf_dict_get(xobj, "Group");
                    if (group != NULL && group->type == PDF_REF)
                        group = pdf_parser_load_object(dev->st, dev->xref, group->u.ref.num, dev->arena);
                    if (group != NULL && (group->type == PDF_DICT || group->type == PDF_STREAM))
                    {
                        const char *gs_subtype = pdf_dict_get_name(group, "S");
                        if (gs_subtype != NULL && strcmp(gs_subtype, "Transparency") == 0 &&
                            pdf_bitmap_create_transparent(dev->arena, saved_bitmap->width, saved_bitmap->height, &group_bmp) == PDF_OK)
                        {
                            pdf_obj *k_obj = pdf_dict_get(group, "K");
                            pdf_bitmap_set_clip(&group_bmp, saved_bitmap->clip_x0, saved_bitmap->clip_y0,
                                                 saved_bitmap->clip_x1, saved_bitmap->clip_y1);
                            pdf_bitmap_set_clip_mask(&group_bmp, saved_bitmap->clip_mask, saved_bitmap->clip_mask_stride);
                            if (k_obj != NULL && k_obj->type == PDF_BOOL && k_obj->u.boolean)
                                pdf_bitmap_enable_knockout(&group_bmp, dev->arena, NULL);
                            dev->bitmap = &group_bmp;
                            using_group = 1;
                        }
                    }
                }

                if (getenv("PDF_CONTENT_DEBUG") != NULL)
                {
                    static long g_pdf_form_debug_count = 0;
                    g_pdf_form_debug_count++;
                    fprintf(stderr, "PDF_FORM_DEBUG: invocacion #%ld draw_form_xobject xobj=%p len=%ld form_depth=%d\n",
                            g_pdf_form_debug_count, (void*)xobj, raw_content.len, dev->form_depth);
                }
                dev->form_depth++;
                {
                    pdf_content_ops nested_ops;
                    nested_ops.op            = pdf_render_op;
                    nested_ops.inline_image   = NULL;
                    nested_ops.user           = dev;
                    pdf_content_run(raw_content.data, raw_content.len, dev->arena, &nested_ops);
                }
                dev->form_depth--;

                if (using_group)
                {
                    dev->bitmap = saved_bitmap;
                    pdf_bitmap_composite(saved_bitmap, &group_bmp, group_alpha, group_blend);
                }

                dev->base_ctm = saved_base_ctm;
            }

            dev->gstate_top--;
            /* BUG REAL ENCONTRADO Y ARREGLADO (ver comentario grande
             * arriba de esta funcion): resincronizar el clip del bitmap
             * con el gstate restaurado, igual que hace el operador 'Q'
             * -- si no, el recorte a /BBox agregado arriba (o cualquier
             * W/W* que el form haya hecho internamente) quedaba "pegado"
             * en dev->bitmap para todo lo que se dibuje despues afuera
             * de este 'Do'. */
            {
                pdf_gstate *outer_gs = cur_gstate(dev);
                pdf_bitmap_set_clip(dev->bitmap, outer_gs->clip_x0, outer_gs->clip_y0, outer_gs->clip_x1, outer_gs->clip_y1);
                pdf_bitmap_set_clip_mask(dev->bitmap, outer_gs->clip_mask, outer_gs->clip_mask_stride);
            }
        }

        dev->resources = saved_resources;
    }
}

extern char g_pdf_debug_last_op[32];

void pdf_render_op(void *user, const char *opname, pdf_obj **args, int nargs)
{
    pdf_render_device *dev = (pdf_render_device *)user;
    pdf_gstate *gs = cur_gstate(dev);

    if (getenv("PDF_ARENA_DEBUG") != NULL)
    {
        strncpy(g_pdf_debug_last_op, opname, sizeof(g_pdf_debug_last_op) - 1);
        g_pdf_debug_last_op[sizeof(g_pdf_debug_last_op) - 1] = 0;
    }

    /* --- pila de estado grafico ------------------------------------- */

    if (strcmp(opname, "q") == 0)
    {
        if (dev->gstate_top + 1 < PDF_RENDER_MAX_GSTATE_DEPTH)
        {
            dev->gstate_stack[dev->gstate_top + 1] = *gs;
            dev->gstate_top++;
        }
        return;
    }

    if (strcmp(opname, "Q") == 0)
    {
        if (dev->gstate_top > 0)
            dev->gstate_top--;
        gs = cur_gstate(dev);
        pdf_bitmap_set_clip(dev->bitmap, gs->clip_x0, gs->clip_y0, gs->clip_x1, gs->clip_y1);
        pdf_bitmap_set_clip_mask(dev->bitmap, gs->clip_mask, gs->clip_mask_stride);
        return;
    }

    if (strcmp(opname, "cm") == 0)
    {
        if (nargs >= 6)
        {
            pdf_matrix m;
            m.a = pdf_obj_num(args[0], 1.0);
            m.b = pdf_obj_num(args[1], 0.0);
            m.c = pdf_obj_num(args[2], 0.0);
            m.d = pdf_obj_num(args[3], 1.0);
            m.e = pdf_obj_num(args[4], 0.0);
            m.f = pdf_obj_num(args[5], 0.0);
            gs->ctm = mat_concat(m, gs->ctm);
        }
        return;
    }

    if (strcmp(opname, "w") == 0)
    {
        if (nargs >= 1)
            gs->line_width = pdf_obj_num(args[0], 1.0);
        return;
    }

    if (strcmp(opname, "gs") == 0)
    {
        /* ExtGState: /ca y /CA (alpha constante de relleno y trazo,
         * ver DESIGN.md seccion 51), y ahora tambien /BM (blend mode --
         * ver DESIGN.md seccion 68). /SMask se deja para una etapa
         * posterior (requiere renderizar el grupo /G a un bitmap
         * aparte). Si una clave no esta presente en el dict
         * referenciado, no se toca el valor actual -- asi es como
         * funciona 'gs' en el estandar (solo pisa lo que trae). */
        if (nargs >= 1 && args[0]->type == PDF_NAME && dev->resources != NULL)
        {
            pdf_obj *eg_res = pdf_dict_get(dev->resources, "ExtGState");
            pdf_obj *ref = (eg_res != NULL) ? pdf_dict_get(eg_res, args[0]->u.name) : NULL;
            pdf_obj *egs = NULL;

            if (ref != NULL && ref->type == PDF_REF)
                egs = pdf_parser_load_object(dev->st, dev->xref, ref->u.ref.num, dev->arena);
            else if (ref != NULL)
                egs = ref;

            if (egs != NULL)
            {
                pdf_obj *ca = pdf_dict_get(egs, "ca");
                pdf_obj *CA = pdf_dict_get(egs, "CA");
                pdf_obj *bm = pdf_dict_get(egs, "BM");
                if (ca != NULL) gs->fill_alpha   = pdf_obj_num(ca, gs->fill_alpha);
                if (CA != NULL) gs->stroke_alpha = pdf_obj_num(CA, gs->stroke_alpha);
                if (bm != NULL)
                {
                    /* /BM puede ser un nombre suelto o un array de
                     * nombres (el lector debe usar el primero que
                     * reconozca) -- tomamos el primer elemento en
                     * cualquiera de los dos casos. */
                    const char *name = NULL;
                    if (bm->type == PDF_NAME)
                        name = bm->u.name;
                    else if (bm->type == PDF_ARRAY && bm->u.arr.count > 0 &&
                             bm->u.arr.items[0] != NULL && bm->u.arr.items[0]->type == PDF_NAME)
                        name = bm->u.arr.items[0]->u.name;

                    if (name != NULL)
                    {
                        /* nombre no reconocido: se deja el blend mode
                         * actual sin cambiar (mismo criterio tolerante
                         * que ca/CA de arriba). */
                        if      (strcmp(name, "Normal") == 0 || strcmp(name, "Compatible") == 0) gs->blend_mode = PDF_BLEND_NORMAL;
                        else if (strcmp(name, "Multiply") == 0)    gs->blend_mode = PDF_BLEND_MULTIPLY;
                        else if (strcmp(name, "Screen") == 0)      gs->blend_mode = PDF_BLEND_SCREEN;
                        else if (strcmp(name, "Overlay") == 0)     gs->blend_mode = PDF_BLEND_OVERLAY;
                        else if (strcmp(name, "Darken") == 0)      gs->blend_mode = PDF_BLEND_DARKEN;
                        else if (strcmp(name, "Lighten") == 0)     gs->blend_mode = PDF_BLEND_LIGHTEN;
                        else if (strcmp(name, "ColorDodge") == 0)  gs->blend_mode = PDF_BLEND_COLOR_DODGE;
                        else if (strcmp(name, "ColorBurn") == 0)   gs->blend_mode = PDF_BLEND_COLOR_BURN;
                        else if (strcmp(name, "HardLight") == 0)   gs->blend_mode = PDF_BLEND_HARD_LIGHT;
                        else if (strcmp(name, "SoftLight") == 0)   gs->blend_mode = PDF_BLEND_SOFT_LIGHT;
                        else if (strcmp(name, "Difference") == 0)  gs->blend_mode = PDF_BLEND_DIFFERENCE;
                        else if (strcmp(name, "Exclusion") == 0)   gs->blend_mode = PDF_BLEND_EXCLUSION;
                        else if (strcmp(name, "Hue") == 0)         gs->blend_mode = PDF_BLEND_HUE;
                        else if (strcmp(name, "Saturation") == 0)  gs->blend_mode = PDF_BLEND_SATURATION;
                        else if (strcmp(name, "Color") == 0)       gs->blend_mode = PDF_BLEND_COLOR;
                        else if (strcmp(name, "Luminosity") == 0)  gs->blend_mode = PDF_BLEND_LUMINOSITY;
                    }
                }

                {
                    /* /SMask: /None limpia la mascara vigente; un dict
                     * con /S /Luminosity la reemplaza renderizando su
                     * /G aparte (ver load_soft_mask_group arriba). Tipo
                     * /Alpha y cualquier otro caso quedan fuera de
                     * alcance -- degradan a "sin soft mask" (mismo
                     * criterio tolerante que el resto de esta funcion). */
                    pdf_obj *smask = pdf_dict_get(egs, "SMask");
                    if (smask != NULL && smask->type == PDF_REF)
                        smask = pdf_parser_load_object(dev->st, dev->xref, smask->u.ref.num, dev->arena);
                    if (smask != NULL && smask->type == PDF_NAME && strcmp(smask->u.name, "None") == 0)
                    {
                        gs->smask_mask = NULL;
                        gs->smask_stride = 0;
                    }
                    else if (smask != NULL && smask->type == PDF_DICT)
                    {
                        load_soft_mask_group(dev, gs, smask);
                    }
                }
            }
        }
        return;
    }

    /* --- color --------------------------------------------------------- */

    if (strcmp(opname, "rg") == 0)
    {
        if (nargs >= 3)
        {
            gs->fill_color.r = pdf_obj_num(args[0], 0.0);
            gs->fill_color.g = pdf_obj_num(args[1], 0.0);
            gs->fill_color.b = pdf_obj_num(args[2], 0.0);
            gs->fill_cs_is_pattern = 0;
            gs->fill_pattern = NULL;
        }
        return;
    }

    if (strcmp(opname, "RG") == 0)
    {
        if (nargs >= 3)
        {
            gs->stroke_color.r = pdf_obj_num(args[0], 0.0);
            gs->stroke_color.g = pdf_obj_num(args[1], 0.0);
            gs->stroke_color.b = pdf_obj_num(args[2], 0.0);
        }
        return;
    }

    if (strcmp(opname, "g") == 0)
    {
        if (nargs >= 1)
        {
            double v = pdf_obj_num(args[0], 0.0);
            gs->fill_color.r = gs->fill_color.g = gs->fill_color.b = v;
            gs->fill_cs_is_pattern = 0;
            gs->fill_pattern = NULL;
        }
        return;
    }

    if (strcmp(opname, "G") == 0)
    {
        if (nargs >= 1)
        {
            double v = pdf_obj_num(args[0], 0.0);
            gs->stroke_color.r = gs->stroke_color.g = gs->stroke_color.b = v;
        }
        return;
    }

    if (strcmp(opname, "cs") == 0 || strcmp(opname, "CS") == 0)
    {
        /* Selecciona el color space con nombre (p.ej. /Cs6, casi siempre
         * ICCBased/Separation/DeviceN en PDFs generados por software
         * profesional) para el siguiente sc/scn/SC/SCN. No resolvemos el
         * color space en si (no interpretamos perfiles ICC ni funciones
         * de tinte de Separation) -- sc/scn se aproxima directamente
         * gris/RGB/CMYK segun la CANTIDAD de componentes numericos que
         * reciba, sin importar el espacio declarado aca. Alcanza para
         * que fondos blancos/de color no queden negros por accidente
         * (bug real encontrado contra v109n11p671.pdf: /Cs6 cs 1 1 1
         * scn para pintar un fondo BLANCO se ignoraba entero y quedaba
         * el negro por defecto, tapando el texto).
         *
         * Excepcion: /Pattern (literal o via nombre de recurso que
         * resuelve a /Pattern) SI se detecta -- marca que el proximo
         * 'scn' con un nombre debe intentar resolver un patron de
         * shading (ver DESIGN.md seccion 68), solo para 'cs' (relleno)
         * -- patrones de trazo quedan fuera de alcance. */
        if (strcmp(opname, "cs") == 0)
        {
            int is_pattern = 0;
            if (nargs >= 1 && args[0]->type == PDF_NAME)
            {
                if (strcmp(args[0]->u.name, "Pattern") == 0)
                {
                    is_pattern = 1;
                }
                else if (dev->resources != NULL)
                {
                    pdf_obj *csres = pdf_dict_get(dev->resources, "ColorSpace");
                    pdf_obj *cs_entry = (csres != NULL) ? pdf_dict_get(csres, args[0]->u.name) : NULL;
                    if (cs_entry != NULL && cs_entry->type == PDF_REF)
                        cs_entry = pdf_parser_load_object(dev->st, dev->xref, cs_entry->u.ref.num, dev->arena);
                    if (cs_entry != NULL && cs_entry->type == PDF_ARRAY && cs_entry->u.arr.count > 0 &&
                        cs_entry->u.arr.items[0] != NULL && cs_entry->u.arr.items[0]->type == PDF_NAME &&
                        strcmp(cs_entry->u.arr.items[0]->u.name, "Pattern") == 0)
                        is_pattern = 1;
                }
            }
            gs->fill_cs_is_pattern = is_pattern;
            if (!is_pattern) gs->fill_pattern = NULL;
        }
        return;
    }

    if (strcmp(opname, "sc") == 0 || strcmp(opname, "scn") == 0 ||
        strcmp(opname, "SC") == 0 || strcmp(opname, "SCN") == 0)
    {
        int is_stroke = (opname[0] == 'S');
        pdf_color *target = is_stroke ? &gs->stroke_color : &gs->fill_color;
        int n = nargs;

        /* Si el ultimo operando es un nombre, es un patron (Pattern
         * color space). Para 'scn' de relleno con /Pattern activo (via
         * 'cs', ver mas arriba) se intenta resolver un patron de
         * shading real (PatternType 2, ver DESIGN.md seccion 68) --
         * patrones de trazo y de mosaico (PatternType 1) quedan fuera
         * de alcance, mismo comportamiento de antes (se descarta el
         * nombre y se aproxima con los componentes numericos que
         * queden, si hay). */
        if (n > 0 && args[n - 1]->type == PDF_NAME)
        {
            if (!is_stroke && gs->fill_cs_is_pattern && strcmp(opname, "scn") == 0 && dev->resources != NULL)
            {
                pdf_obj *pat_res = pdf_dict_get(dev->resources, "Pattern");
                pdf_obj *ref, *pat_obj = NULL;

                if (pat_res != NULL && pat_res->type == PDF_REF)
                    pat_res = pdf_parser_load_object(dev->st, dev->xref, pat_res->u.ref.num, dev->arena);

                ref = (pat_res != NULL && pat_res->type == PDF_DICT) ? pdf_dict_get(pat_res, args[n - 1]->u.name) : NULL;
                if (ref != NULL && ref->type == PDF_REF)
                    pat_obj = pdf_parser_load_object(dev->st, dev->xref, ref->u.ref.num, dev->arena);
                else if (ref != NULL)
                    pat_obj = ref;

                gs->fill_pattern = NULL;
                if (pat_obj != NULL && (pat_obj->type == PDF_DICT || pat_obj->type == PDF_STREAM) &&
                    pdf_dict_get_int(pat_obj, "PatternType", -1) == 2)
                {
                    pdf_obj *matrix = pdf_dict_get(pat_obj, "Matrix");
                    pdf_matrix pm = PDF_MATRIX_IDENTITY_INIT;
                    if (matrix != NULL && matrix->type == PDF_ARRAY && matrix->u.arr.count >= 6)
                    {
                        pm.a = pdf_obj_num(matrix->u.arr.items[0], 1.0);
                        pm.b = pdf_obj_num(matrix->u.arr.items[1], 0.0);
                        pm.c = pdf_obj_num(matrix->u.arr.items[2], 0.0);
                        pm.d = pdf_obj_num(matrix->u.arr.items[3], 1.0);
                        pm.e = pdf_obj_num(matrix->u.arr.items[4], 0.0);
                        pm.f = pdf_obj_num(matrix->u.arr.items[5], 0.0);
                    }
                    gs->fill_pattern = pat_obj;
                    gs->fill_pattern_matrix = pm;
                }
            }
            n--;
        }

        if (n == 1)
        {
            double v = pdf_obj_num(args[0], 0.0);
            target->r = target->g = target->b = v;
        }
        else if (n == 3)
        {
            target->r = pdf_obj_num(args[0], 0.0);
            target->g = pdf_obj_num(args[1], 0.0);
            target->b = pdf_obj_num(args[2], 0.0);
        }
        else if (n == 4)
        {
            /* CMYK -> RGB, conversion naive (sin perfil ICC real, pero
             * mucho mejor que dejar negro): r=1-min(1,c+k), etc. */
            double c = pdf_obj_num(args[0], 0.0);
            double m = pdf_obj_num(args[1], 0.0);
            double y = pdf_obj_num(args[2], 0.0);
            double k = pdf_obj_num(args[3], 0.0);
            target->r = 1.0 - ((c + k > 1.0) ? 1.0 : c + k);
            target->g = 1.0 - ((m + k > 1.0) ? 1.0 : m + k);
            target->b = 1.0 - ((y + k > 1.0) ? 1.0 : y + k);
        }
        return;
    }

    if (strcmp(opname, "k") == 0 || strcmp(opname, "K") == 0)
    {
        if (nargs >= 4)
        {
            int is_stroke = (opname[0] == 'K');
            pdf_color *target = is_stroke ? &gs->stroke_color : &gs->fill_color;
            double c = pdf_obj_num(args[0], 0.0);
            double m = pdf_obj_num(args[1], 0.0);
            double y = pdf_obj_num(args[2], 0.0);
            double k = pdf_obj_num(args[3], 0.0);
            target->r = 1.0 - ((c + k > 1.0) ? 1.0 : c + k);
            target->g = 1.0 - ((m + k > 1.0) ? 1.0 : m + k);
            target->b = 1.0 - ((y + k > 1.0) ? 1.0 : y + k);
            if (!is_stroke) { gs->fill_cs_is_pattern = 0; gs->fill_pattern = NULL; }
        }
        return;
    }

    /* --- construccion de path -------------------------------------- */

    if (strcmp(opname, "m") == 0)
    {
        if (nargs >= 2)
        {
            pdf_point p = to_pixel(dev, mat_transform(gs->ctm,
                          pdf_obj_num(args[0], 0.0), pdf_obj_num(args[1], 0.0)));
            pdf_path_moveto(&dev->cur_path, p.x, p.y);
        }
        return;
    }

    if (strcmp(opname, "l") == 0)
    {
        if (nargs >= 2)
        {
            pdf_point p = to_pixel(dev, mat_transform(gs->ctm,
                          pdf_obj_num(args[0], 0.0), pdf_obj_num(args[1], 0.0)));
            pdf_path_lineto(&dev->cur_path, p.x, p.y);
        }
        return;
    }

    if (strcmp(opname, "c") == 0)
    {
        if (nargs >= 6)
        {
            pdf_point p1 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[0],0), pdf_obj_num(args[1],0)));
            pdf_point p2 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[2],0), pdf_obj_num(args[3],0)));
            pdf_point p3 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[4],0), pdf_obj_num(args[5],0)));
            pdf_path_curveto(&dev->cur_path, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        }
        return;
    }

    if (strcmp(opname, "v") == 0)
    {
        if (nargs >= 4)
        {
            pdf_point p2 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[0],0), pdf_obj_num(args[1],0)));
            pdf_point p3 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[2],0), pdf_obj_num(args[3],0)));
            double cx = dev->cur_path.has_current ? dev->cur_path.cur_x : p2.x;
            double cy = dev->cur_path.has_current ? dev->cur_path.cur_y : p2.y;
            pdf_path_curveto(&dev->cur_path, cx, cy, p2.x, p2.y, p3.x, p3.y);
        }
        return;
    }

    if (strcmp(opname, "y") == 0)
    {
        if (nargs >= 4)
        {
            pdf_point p1 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[0],0), pdf_obj_num(args[1],0)));
            pdf_point p3 = to_pixel(dev, mat_transform(gs->ctm, pdf_obj_num(args[2],0), pdf_obj_num(args[3],0)));
            pdf_path_curveto(&dev->cur_path, p1.x, p1.y, p3.x, p3.y, p3.x, p3.y);
        }
        return;
    }

    if (strcmp(opname, "h") == 0)
    {
        pdf_path_close(&dev->cur_path);
        return;
    }

    if (strcmp(opname, "re") == 0)
    {
        if (nargs >= 4)
        {
            double x = pdf_obj_num(args[0], 0.0);
            double y = pdf_obj_num(args[1], 0.0);
            double w = pdf_obj_num(args[2], 0.0);
            double h = pdf_obj_num(args[3], 0.0);
            pdf_point p0 = to_pixel(dev, mat_transform(gs->ctm, x,     y));
            pdf_point p1 = to_pixel(dev, mat_transform(gs->ctm, x + w, y));
            pdf_point p2 = to_pixel(dev, mat_transform(gs->ctm, x + w, y + h));
            pdf_point p3 = to_pixel(dev, mat_transform(gs->ctm, x,     y + h));
            pdf_path_rect_corners(&dev->cur_path, p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        }
        return;
    }

    /* --- pintado de path ------------------------------------------- */

    if (strcmp(opname, "S") == 0)
    {
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "s") == 0)
    {
        pdf_path_close(&dev->cur_path);
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "f") == 0 || strcmp(opname, "F") == 0)
    {
        fill_current_path(dev, gs, PDF_FILL_NONZERO);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "f*") == 0)
    {
        fill_current_path(dev, gs, PDF_FILL_EVENODD);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "B") == 0)
    {
        fill_current_path(dev, gs, PDF_FILL_NONZERO);
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "B*") == 0)
    {
        fill_current_path(dev, gs, PDF_FILL_EVENODD);
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "b") == 0)
    {
        pdf_path_close(&dev->cur_path);
        fill_current_path(dev, gs, PDF_FILL_NONZERO);
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "b*") == 0)
    {
        pdf_path_close(&dev->cur_path);
        fill_current_path(dev, gs, PDF_FILL_EVENODD);
        sync_paint_state(dev, gs, 1);
        pdf_raster_stroke_path_w(dev->bitmap, &dev->cur_path, gs->stroke_color,
                                  gs->line_width * ctm_scale(gs->ctm) * dev->scale);
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "n") == 0)
    {
        finish_path(dev, gs);
        return;
    }

    if (strcmp(opname, "W") == 0 || strcmp(opname, "W*") == 0)
    {
        /* El clip recien surte efecto DESPUES del proximo operador de
         * pintado (f/S/n/etc) -- ver finish_path(), que es donde
         * realmente se aplica usando el path tal cual queda en ese
         * momento. */
        dev->pending_clip = 1;
        dev->pending_clip_evenodd = (strcmp(opname, "W*") == 0);
        return;
    }

    if (strcmp(opname, "sh") == 0)
    {
        /* Pinta un shading (gradiente axial/radial) dentro del clip
         * vigente -- ver DESIGN.md seccion 68. /Coords del shading
         * estan en el espacio de usuario ACTUAL (post-CTM), asi que la
         * transformacion shading->pixel es la misma que ya se usa para
         * imagenes (CTM concatenado con el flip de eje Y a pixel), y
         * la que necesitamos para ir de pixel a shading es su inversa. */
        if (nargs >= 1 && args[0]->type == PDF_NAME && dev->resources != NULL)
        {
            pdf_obj *sh_res = pdf_dict_get(dev->resources, "Shading");
            pdf_obj *ref, *sh_obj = NULL;

            if (sh_res != NULL && sh_res->type == PDF_REF)
                sh_res = pdf_parser_load_object(dev->st, dev->xref, sh_res->u.ref.num, dev->arena);

            ref = (sh_res != NULL && sh_res->type == PDF_DICT) ? pdf_dict_get(sh_res, args[0]->u.name) : NULL;

            if (ref != NULL && ref->type == PDF_REF)
                sh_obj = pdf_parser_load_object(dev->st, dev->xref, ref->u.ref.num, dev->arena);
            else if (ref != NULL)
                sh_obj = ref;

            if (sh_obj != NULL)
            {
                pdf_shading shading;
                if (pdf_shading_load(dev->st, dev->xref, sh_obj, dev->arena, &shading) == PDF_OK &&
                    shading.kind != PDF_SHADING_UNSUPPORTED)
                {
                    pdf_matrix to_pixel_mat, shading_to_pixel, device_to_shading;

                    to_pixel_mat = rotation_to_pixel_matrix( dev );

                    shading_to_pixel = mat_concat(gs->ctm, to_pixel_mat);
                    if (mat_invert(shading_to_pixel, &device_to_shading))
                    {
                        sync_paint_state(dev, gs, 0);
                        pdf_shading_paint_clip(dev->bitmap, &shading, device_to_shading);
                    }
                }
                /* fallo de carga o tipo no soportado: no pintar nada,
                 * deja el fondo visible -- degradacion, no crash. */
            }
        }
        return;
    }

    /* --- texto: estado --------------------------------------------- */

    if (strcmp(opname, "BT") == 0)
    {
        pdf_matrix id = PDF_MATRIX_IDENTITY_INIT;
        dev->text_matrix = id;
        dev->line_matrix = id;
        return;
    }

    if (strcmp(opname, "ET") == 0)
    {
        return; /* nada que hacer: el estado de texto se resetea recien en el proximo BT */
    }

    if (strcmp(opname, "Tf") == 0)
    {
        if (nargs >= 2 && args[0]->type == PDF_NAME)
        {
            gs->font      = resolve_font(dev, args[0]->u.name);
            gs->font_size = pdf_obj_num(args[1], 12.0);
        }
        return;
    }

    if (strcmp(opname, "Tc") == 0)
    {
        if (nargs >= 1) gs->char_space = pdf_obj_num(args[0], 0.0);
        return;
    }

    if (strcmp(opname, "Tw") == 0)
    {
        if (nargs >= 1) gs->word_space = pdf_obj_num(args[0], 0.0);
        return;
    }

    if (strcmp(opname, "Tz") == 0)
    {
        if (nargs >= 1) gs->h_scale = pdf_obj_num(args[0], 100.0) / 100.0;
        return;
    }

    if (strcmp(opname, "TL") == 0)
    {
        if (nargs >= 1) gs->leading = pdf_obj_num(args[0], 0.0);
        return;
    }

    if (strcmp(opname, "Ts") == 0)
    {
        if (nargs >= 1) gs->rise = pdf_obj_num(args[0], 0.0);
        return;
    }

    if (strcmp(opname, "Tr") == 0)
    {
        if (nargs >= 1) gs->render_mode = (int)pdf_obj_num(args[0], 0.0);
        return;
    }

    /* --- texto: posicionamiento -------------------------------------- */

    if (strcmp(opname, "Td") == 0 || strcmp(opname, "TD") == 0)
    {
        if (nargs >= 2)
        {
            double tx = pdf_obj_num(args[0], 0.0);
            double ty = pdf_obj_num(args[1], 0.0);
            pdf_matrix translate;

            if (strcmp(opname, "TD") == 0)
                gs->leading = -ty;

            translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
            translate.e = tx;  translate.f = ty;

            dev->line_matrix = mat_concat(translate, dev->line_matrix);
            dev->text_matrix = dev->line_matrix;
        }
        return;
    }

    if (strcmp(opname, "Tm") == 0)
    {
        if (nargs >= 6)
        {
            pdf_matrix m;
            m.a = pdf_obj_num(args[0], 1.0);
            m.b = pdf_obj_num(args[1], 0.0);
            m.c = pdf_obj_num(args[2], 0.0);
            m.d = pdf_obj_num(args[3], 1.0);
            m.e = pdf_obj_num(args[4], 0.0);
            m.f = pdf_obj_num(args[5], 0.0);
            dev->line_matrix = m;
            dev->text_matrix = m;
        }
        return;
    }

    if (strcmp(opname, "T*") == 0)
    {
        pdf_matrix translate;
        translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
        translate.e = 0.0; translate.f = -gs->leading;
        dev->line_matrix = mat_concat(translate, dev->line_matrix);
        dev->text_matrix = dev->line_matrix;
        return;
    }

    /* --- texto: mostrar ---------------------------------------------- */

    if (strcmp(opname, "Tj") == 0)
    {
        if (nargs >= 1)
            show_text_obj(dev, gs, args[0]);
        return;
    }

    if (strcmp(opname, "'") == 0)
    {
        pdf_matrix translate;
        translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
        translate.e = 0.0; translate.f = -gs->leading;
        dev->line_matrix = mat_concat(translate, dev->line_matrix);
        dev->text_matrix = dev->line_matrix;
        if (nargs >= 1)
            show_text_obj(dev, gs, args[0]);
        return;
    }

    if (strcmp(opname, "\"") == 0)
    {
        pdf_matrix translate;
        if (nargs >= 3)
        {
            gs->word_space = pdf_obj_num(args[0], gs->word_space);
            gs->char_space = pdf_obj_num(args[1], gs->char_space);
        }
        translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
        translate.e = 0.0; translate.f = -gs->leading;
        dev->line_matrix = mat_concat(translate, dev->line_matrix);
        dev->text_matrix = dev->line_matrix;
        if (nargs >= 3)
            show_text_obj(dev, gs, args[2]);
        return;
    }

    if (strcmp(opname, "TJ") == 0)
    {
        if (nargs >= 1 && args[0]->type == PDF_ARRAY)
        {
            int i;
            for (i = 0; i < args[0]->u.arr.count; i++)
            {
                pdf_obj *item = args[0]->u.arr.items[i];
                if (item == NULL) continue;

                if (item->type == PDF_STRING)
                {
                    show_text_obj(dev, gs, item);
                }
                else if (item->type == PDF_INT || item->type == PDF_REAL)
                {
                    double adj = pdf_obj_num(item, 0.0);
                    double tx  = -(adj / 1000.0) * gs->font_size * gs->h_scale;
                    pdf_matrix translate;
                    translate.a = 1.0; translate.b = 0.0; translate.c = 0.0; translate.d = 1.0;
                    translate.e = tx;  translate.f = 0.0;
                    dev->text_matrix = mat_concat(translate, dev->text_matrix);
                }
            }
        }
        return;
    }

    /* --- imagenes -------------------------------------------------- */

    if (strcmp(opname, "Do") == 0)
    {
        if (nargs >= 1 && args[0]->type == PDF_NAME && dev->resources != NULL)
        {
            pdf_obj *xobj_res = pdf_dict_get(dev->resources, "XObject");
            pdf_obj *ref, *xobj = NULL;

            /* BUG REAL ENCONTRADO (confirmado contra
             * 615_89_Escorias_y_cementos_siderurgicos.pdf, un PDF
             * escaneado con una imagen CCITT de pagina completa mas una
             * capa de texto OCR invisible): igual que /Resources/Font
             * (ver find_font_dict arriba), /Resources/XObject puede
             * venir ELLA MISMA como referencia indirecta ("12 0 R"), no
             * solo cada entrada individual dentro de ella. Sin
             * resolverla, 'pdf_dict_get(xobj_res, name)' trataba un
             * PDF_REF como si fuera un PDF_DICT y nunca encontraba
             * ninguna clave -- la imagen de fondo (y CUALQUIER XObject,
             * incluidos Form XObjects) se saltaba en silencio, dejando
             * la pagina en blanco salvo el texto que SI estuviera fuera
             * de este camino (p.ej. un pie de pagina en el content
             * stream principal). */
            if (xobj_res != NULL && xobj_res->type == PDF_REF)
                xobj_res = pdf_parser_load_object(dev->st, dev->xref, xobj_res->u.ref.num, dev->arena);

            ref = (xobj_res != NULL && xobj_res->type == PDF_DICT) ? pdf_dict_get(xobj_res, args[0]->u.name) : NULL;

            if (ref != NULL && ref->type == PDF_REF)
                xobj = pdf_parser_load_object(dev->st, dev->xref, ref->u.ref.num, dev->arena);
            else if (ref != NULL)
                xobj = ref;

            if (xobj != NULL && xobj->type == PDF_STREAM)
            {
                const char *subtype = pdf_dict_get_name(xobj, "Subtype");
                if (subtype != NULL && strcmp(subtype, "Image") == 0)
                {
                    pdf_image img;
                    if (pdf_image_decode(dev->st, dev->xref, xobj, dev->arena, &img) == PDF_OK)
                    {
                        /* el cuadrado unitario [0,1]x[0,1] de espacio de
                         * imagen se mapea a traves del CTM vigente, igual
                         * que cualquier otro path -- despues se convierte
                         * a espacio de pixel para la composicion. */
                        pdf_matrix unit_to_pixel_pdf = gs->ctm;
                        pdf_matrix to_pixel_mat;
                        pdf_matrix unit_to_pixel;

                        to_pixel_mat = rotation_to_pixel_matrix( dev );

                        unit_to_pixel = mat_concat(unit_to_pixel_pdf, to_pixel_mat);
                        /* Etapa 10 (ver DESIGN.md seccion 68): las
                         * imagenes ahora pasan por el mismo compositor
                         * real que fills/stroke/texto -- sync_paint_state
                         * sincroniza opacity/blend_mode/soft_mask del
                         * bitmap ANTES de dibujar, y pdf_image_blend_pixel
                         * (dentro de pdf_image_draw, para imagenes con
                         * /SMask) ya delega en pdf_bitmap_set_pixel_
                         * coverage en vez de su propio blend manual. */
                        sync_paint_state(dev, gs, 0);
                        pdf_image_draw(dev->bitmap, &img, unit_to_pixel, gs->fill_color);
                    }
                    /* si pdf_image_decode falla (filtro no soportado,
                     * presupuesto de memoria agotado para una imagen
                     * grande, etc.) se ignora silenciosamente -- tolerante,
                     * como el resto del motor. */
                }
                /* /Subtype /Form: ver draw_form_xobject() arriba de
                 * pdf_render_op -- extraido a helper reusable para que
                 * pdf_render_draw_annotations() (dibujo de campos
                 * AcroForm, ver DESIGN.md) tambien lo pueda invocar
                 * directo sobre un stream de apariencia (/AP/N) que no
                 * necesariamente esta en /Resources/XObject de ninguna
                 * pagina. */
                else if (subtype != NULL && strcmp(subtype, "Form") == 0 &&
                         dev->form_depth < 16 &&
                         xobj->u.stm.raw_length > 0 && xobj->u.stm.raw_length <= 500L * 1024L * 1024L)
                {
                    draw_form_xobject(dev, xobj);
                }
            }
        }
        return;
    }

    /* Cualquier otro operador (color spaces exoticos, texto Type0/CID,
     * etc.) se ignora silenciosamente: son los puntos de extension que
     * quedan. */
}

/* ========================================================================
 * AcroForm: dibujo de la apariencia actual de cada campo Widget de la
 * pagina (ver DESIGN.md, fase AcroForm del roadmap de potencialidad
 * MuPDF). Se llama DESPUES de correr el content stream de la pagina
 * (mismo orden que exige la norma: las anotaciones se pintan encima
 * del contenido de pagina). Reusa draw_form_xobject() -- las
 * apariencias (/AP/N) SON Form XObjects por norma, con la diferencia
 * de que su posicion no viene de 'cm'/'Do' sino de mapear su propio
 * /BBox al /Rect del widget (Algoritmo "Appearance streams", norma
 * 12.5.5).
 * ========================================================================= */

static void reset_gstate_default(pdf_gstate *gs, int bitmap_w, int bitmap_h)
{
    pdf_matrix id = PDF_MATRIX_IDENTITY_INIT;

    gs->ctm             = id;
    gs->fill_color.r     = 0.0;
    gs->fill_color.g     = 0.0;
    gs->fill_color.b     = 0.0;
    gs->stroke_color     = gs->fill_color;
    gs->line_width       = 1.0;
    gs->fill_alpha        = 1.0;
    gs->stroke_alpha      = 1.0;
    gs->blend_mode        = PDF_BLEND_NORMAL;
    gs->clip_x0           = 0;
    gs->clip_y0           = 0;
    gs->clip_x1           = bitmap_w;
    gs->clip_y1           = bitmap_h;
    gs->clip_mask         = NULL;
    gs->clip_mask_stride  = 0;
    gs->smask_mask        = NULL;
    gs->smask_stride      = 0;
    gs->fill_pattern      = NULL;
    gs->fill_pattern_matrix = id;
    gs->fill_cs_is_pattern  = 0;
    gs->font              = NULL;
    gs->font_size         = 12.0;
    gs->char_space        = 0.0;
    gs->word_space        = 0.0;
    gs->h_scale           = 1.0;
    gs->leading           = 0.0;
    gs->rise              = 0.0;
    gs->render_mode       = 0;
}

/* Cuerpo compartido de "resolver /AP/N (con /AS si es dict de estados),
 * mapear /BBox transformado por /Matrix al /Rect via el algoritmo Appearance Streams
 * (norma 12.5.5), y dibujar" -- extraido de pdf_render_draw_annotations
 * en la fase de resaltado de texto para reusarlo tambien con
 * anotaciones /Highlight (pdf_render_draw_highlight_annotations, mas
 * abajo). Nada aca es especifico de AcroForm: 'annot_obj' es cualquier
 * dict de anotacion con /F/AP/AS, 'rect' es su /Rect ya resuelto -- el
 * unico uso de /AS (estado actual para un dict de sub-apariencias) es
 * el mecanismo GENERAL de la norma, no algo propio de campos Widget
 * (aunque en la practica solo los Widget checkbox lo usan). */
static void draw_annot_appearance(pdf_render_device *dev, pdf_obj *annot_obj, pdf_rect rect)
{
    pdf_obj *flags_obj, *ap, *ap_n, *appearance;
    pdf_obj *bbox_obj, *matrix_obj;
    long annot_flags;
    double bx0 = 0.0, by0 = 0.0, bx1 = 1.0, by1 = 1.0;
    pdf_matrix fm;
    pdf_point t0, t1, t2, t3;
    double tminx, tmaxx, tminy, tmaxy;
    pdf_matrix a_mat;
    double sx, sy;
    pdf_gstate *gs;

    flags_obj = pdf_dict_get(annot_obj, "F");
    annot_flags = (flags_obj != NULL) ? (long)pdf_obj_num(flags_obj, 0.0) : 0;
    if (annot_flags & 0x2)  return; /* Hidden */
    if (annot_flags & 0x20) return; /* NoView */

    ap = pdf_dict_get(annot_obj, "AP");
    if (ap != NULL && ap->type == PDF_REF)
        ap = pdf_parser_load_object(dev->st, dev->xref, ap->u.ref.num, dev->arena);
    if (ap == NULL) return;

    ap_n = pdf_dict_get(ap, "N");
    if (ap_n != NULL && ap_n->type == PDF_REF)
        ap_n = pdf_parser_load_object(dev->st, dev->xref, ap_n->u.ref.num, dev->arena);
    if (ap_n == NULL) return;

    appearance = NULL;
    if (ap_n->type == PDF_STREAM)
    {
        appearance = ap_n;
    }
    else if (ap_n->type == PDF_DICT)
    {
        /* checkbox u otro campo de estados: /AS (estado actual del
         * widget) elige la sub-entrada; sin /AS, /Off por defecto. */
        const char *as_name = pdf_dict_get_name(annot_obj, "AS");
        pdf_obj *sub = (as_name != NULL) ? pdf_dict_get(ap_n, as_name) : NULL;
        if (sub == NULL) sub = pdf_dict_get(ap_n, "Off");
        if (sub != NULL && sub->type == PDF_REF)
            sub = pdf_parser_load_object(dev->st, dev->xref, sub->u.ref.num, dev->arena);
        if (sub != NULL && sub->type == PDF_STREAM)
            appearance = sub;
    }
    if (appearance == NULL) return;

    bbox_obj = pdf_dict_get(appearance, "BBox");
    if (bbox_obj != NULL && bbox_obj->type == PDF_ARRAY && bbox_obj->u.arr.count == 4)
    {
        bx0 = pdf_obj_num(bbox_obj->u.arr.items[0], 0.0);
        by0 = pdf_obj_num(bbox_obj->u.arr.items[1], 0.0);
        bx1 = pdf_obj_num(bbox_obj->u.arr.items[2], 1.0);
        by1 = pdf_obj_num(bbox_obj->u.arr.items[3], 1.0);
    }

    fm.a = 1.0; fm.b = 0.0; fm.c = 0.0; fm.d = 1.0; fm.e = 0.0; fm.f = 0.0;
    matrix_obj = pdf_dict_get(appearance, "Matrix");
    if (matrix_obj != NULL && matrix_obj->type == PDF_ARRAY && matrix_obj->u.arr.count == 6)
    {
        fm.a = pdf_obj_num(matrix_obj->u.arr.items[0], 1.0);
        fm.b = pdf_obj_num(matrix_obj->u.arr.items[1], 0.0);
        fm.c = pdf_obj_num(matrix_obj->u.arr.items[2], 0.0);
        fm.d = pdf_obj_num(matrix_obj->u.arr.items[3], 1.0);
        fm.e = pdf_obj_num(matrix_obj->u.arr.items[4], 0.0);
        fm.f = pdf_obj_num(matrix_obj->u.arr.items[5], 0.0);
    }

    /* Algoritmo "Appearance streams" (norma 12.5.5): transformar
     * las 4 esquinas de /BBox por /Matrix, tomar el bounding box
     * resultante, y calcular la matriz A (solo escala+traslacion,
     * sin rotacion -- la norma no la pide aca) que mapea ESE
     * bounding box al /Rect de la anotacion. draw_form_xobject()
     * concatena /Matrix DE NUEVO sobre lo que le pasemos como CTM
     * -- si le damos ctm=A, el resultado es Matrix*A, exactamente
     * lo que exige la norma (no es una doble aplicacion). */
    t0 = mat_transform(fm, bx0, by0);
    t1 = mat_transform(fm, bx1, by0);
    t2 = mat_transform(fm, bx1, by1);
    t3 = mat_transform(fm, bx0, by1);
    tminx = t0.x; tmaxx = t0.x; tminy = t0.y; tmaxy = t0.y;
    if (t1.x < tminx) tminx = t1.x; if (t1.x > tmaxx) tmaxx = t1.x;
    if (t1.y < tminy) tminy = t1.y; if (t1.y > tmaxy) tmaxy = t1.y;
    if (t2.x < tminx) tminx = t2.x; if (t2.x > tmaxx) tmaxx = t2.x;
    if (t2.y < tminy) tminy = t2.y; if (t2.y > tmaxy) tmaxy = t2.y;
    if (t3.x < tminx) tminx = t3.x; if (t3.x > tmaxx) tmaxx = t3.x;
    if (t3.y < tminy) tminy = t3.y; if (t3.y > tmaxy) tmaxy = t3.y;

    sx = ((tmaxx - tminx) > 1e-6) ? (rect.x1 - rect.x0) / (tmaxx - tminx) : 1.0;
    sy = ((tmaxy - tminy) > 1e-6) ? (rect.y1 - rect.y0) / (tmaxy - tminy) : 1.0;

    a_mat.a = sx; a_mat.b = 0.0; a_mat.c = 0.0; a_mat.d = sy;
    a_mat.e = rect.x0 - tminx * sx;
    a_mat.f = rect.y0 - tminy * sy;

    /* Gstate "limpio" en el tope del stack para ESTA anotacion --
     * nunca hereda nada de la anotacion anterior ni del final del
     * content stream de la pagina. */
    dev->gstate_top = 0;
    gs = cur_gstate(dev);
    reset_gstate_default(gs, dev->bitmap->width, dev->bitmap->height);
    gs->ctm = a_mat;
    pdf_bitmap_set_clip(dev->bitmap, gs->clip_x0, gs->clip_y0, gs->clip_x1, gs->clip_y1);
    pdf_bitmap_set_clip_mask(dev->bitmap, NULL, 0);

    draw_form_xobject(dev, appearance);
}

void pdf_render_draw_annotations(pdf_render_device *dev, pdf_obj *page_obj)
{
    pdf_form_field fields[PDF_FORM_MAX_FIELDS];
    int n, i;

    if (dev == NULL || page_obj == NULL || dev->st == NULL || dev->xref == NULL ||
        dev->arena == NULL || dev->bitmap == NULL)
        return;

    n = pdf_form_list_fields(dev->st, dev->xref, dev->arena, page_obj, fields, PDF_FORM_MAX_FIELDS);

    for (i = 0; i < n; i++)
    {
        pdf_form_field *f = &fields[i];
        if (f->widget_obj == NULL) continue;
        draw_annot_appearance(dev, f->widget_obj, f->rect);
    }
}

/* Ver comentario grande junto a la declaracion, pdf_render.h. */
void pdf_render_draw_highlight_annotations(pdf_render_device *dev, pdf_obj *page_obj)
{
    pdf_obj *annots;
    int i;

    if (dev == NULL || page_obj == NULL || dev->st == NULL || dev->xref == NULL ||
        dev->arena == NULL || dev->bitmap == NULL)
        return;

    annots = pdf_dict_get(page_obj, "Annots");
    if (annots != NULL && annots->type == PDF_REF)
        annots = pdf_parser_load_object(dev->st, dev->xref, annots->u.ref.num, dev->arena);
    if (annots == NULL || annots->type != PDF_ARRAY)
        return;

    for (i = 0; i < annots->u.arr.count; i++)
    {
        pdf_obj *annot = annots->u.arr.items[i];
        const char *subtype;
        pdf_obj *rect_obj;
        pdf_rect rect;
        double rx0, ry0, rx1, ry1;

        if (annot != NULL && annot->type == PDF_REF)
            annot = pdf_parser_load_object(dev->st, dev->xref, annot->u.ref.num, dev->arena);
        if (annot == NULL || (annot->type != PDF_DICT && annot->type != PDF_STREAM))
            continue;

        subtype = pdf_dict_get_name(annot, "Subtype");
        if (subtype == NULL || strcmp(subtype, "Highlight") != 0)
            continue;

        rect_obj = pdf_dict_get(annot, "Rect");
        if (rect_obj == NULL || rect_obj->type != PDF_ARRAY || rect_obj->u.arr.count != 4)
            continue;

        rx0 = pdf_obj_num(rect_obj->u.arr.items[0], 0.0);
        ry0 = pdf_obj_num(rect_obj->u.arr.items[1], 0.0);
        rx1 = pdf_obj_num(rect_obj->u.arr.items[2], 0.0);
        ry1 = pdf_obj_num(rect_obj->u.arr.items[3], 0.0);
        rect.x0 = (rx0 < rx1) ? rx0 : rx1;
        rect.x1 = (rx0 < rx1) ? rx1 : rx0;
        rect.y0 = (ry0 < ry1) ? ry0 : ry1;
        rect.y1 = (ry0 < ry1) ? ry1 : ry0;

        draw_annot_appearance(dev, annot, rect);
    }
}

/* Igual que las 2 funciones de arriba (mismo scan de /Annots), pero
 * filtrando /Subtype /FreeText (globo de tip, ver pdf_annot.h/
 * pdf_annot.c) -- se llama DESPUES de pdf_render_draw_shape_
 * annotations (mismo call site) para que un tip quede dibujado por
 * encima de todo lo demas. */
void pdf_render_draw_tip_annotations(pdf_render_device *dev, pdf_obj *page_obj)
{
    pdf_obj *annots;
    int i;

    if (dev == NULL || page_obj == NULL || dev->st == NULL || dev->xref == NULL ||
        dev->arena == NULL || dev->bitmap == NULL)
        return;

    annots = pdf_dict_get(page_obj, "Annots");
    if (annots != NULL && annots->type == PDF_REF)
        annots = pdf_parser_load_object(dev->st, dev->xref, annots->u.ref.num, dev->arena);
    if (annots == NULL || annots->type != PDF_ARRAY)
        return;

    for (i = 0; i < annots->u.arr.count; i++)
    {
        pdf_obj *annot = annots->u.arr.items[i];
        const char *subtype;
        pdf_obj *rect_obj;
        pdf_rect rect;
        double rx0, ry0, rx1, ry1;

        if (annot != NULL && annot->type == PDF_REF)
            annot = pdf_parser_load_object(dev->st, dev->xref, annot->u.ref.num, dev->arena);
        if (annot == NULL || (annot->type != PDF_DICT && annot->type != PDF_STREAM))
            continue;

        subtype = pdf_dict_get_name(annot, "Subtype");
        if (subtype == NULL || strcmp(subtype, "FreeText") != 0)
            continue;

        rect_obj = pdf_dict_get(annot, "Rect");
        if (rect_obj == NULL || rect_obj->type != PDF_ARRAY || rect_obj->u.arr.count != 4)
            continue;

        rx0 = pdf_obj_num(rect_obj->u.arr.items[0], 0.0);
        ry0 = pdf_obj_num(rect_obj->u.arr.items[1], 0.0);
        rx1 = pdf_obj_num(rect_obj->u.arr.items[2], 0.0);
        ry1 = pdf_obj_num(rect_obj->u.arr.items[3], 0.0);
        rect.x0 = (rx0 < rx1) ? rx0 : rx1;
        rect.x1 = (rx0 < rx1) ? rx1 : rx0;
        rect.y0 = (ry0 < ry1) ? ry0 : ry1;
        rect.y1 = (ry0 < ry1) ? ry1 : ry0;

        draw_annot_appearance(dev, annot, rect);
    }
}

/* Igual que pdf_render_draw_highlight_annotations arriba (mismo scan
 * de /Annots byte a byte, ref-o-inline, /Rect min/max-normalizado),
 * cambiando solo el filtro de Subtype a las 4 formas libres nuevas
 * (ver pdf_annot.h/pdf_annot.c) -- reusa draw_annot_appearance() sin
 * cambios, ya es generico para cualquier anotacion con /Rect+/AP. Se
 * llama DESPUES de pdf_render_draw_highlight_annotations (mismo call
 * site en HB_FUNC(PDF_RENDERTOHBITMAP), pdf_hbfunc.c) para que formas
 * (flechas/circulos senalando algo) queden por encima de cualquier
 * resaltado debajo. */
void pdf_render_draw_shape_annotations(pdf_render_device *dev, pdf_obj *page_obj)
{
    pdf_obj *annots;
    int i;

    if (dev == NULL || page_obj == NULL || dev->st == NULL || dev->xref == NULL ||
        dev->arena == NULL || dev->bitmap == NULL)
        return;

    annots = pdf_dict_get(page_obj, "Annots");
    if (annots != NULL && annots->type == PDF_REF)
        annots = pdf_parser_load_object(dev->st, dev->xref, annots->u.ref.num, dev->arena);
    if (annots == NULL || annots->type != PDF_ARRAY)
        return;

    for (i = 0; i < annots->u.arr.count; i++)
    {
        pdf_obj *annot = annots->u.arr.items[i];
        const char *subtype;
        pdf_obj *rect_obj;
        pdf_rect rect;
        double rx0, ry0, rx1, ry1;

        if (annot != NULL && annot->type == PDF_REF)
            annot = pdf_parser_load_object(dev->st, dev->xref, annot->u.ref.num, dev->arena);
        if (annot == NULL || (annot->type != PDF_DICT && annot->type != PDF_STREAM))
            continue;

        subtype = pdf_dict_get_name(annot, "Subtype");
        if (subtype == NULL ||
            (strcmp(subtype, "Line") != 0 && strcmp(subtype, "Square") != 0 &&
             strcmp(subtype, "Circle") != 0 && strcmp(subtype, "Ink") != 0))
            continue;

        rect_obj = pdf_dict_get(annot, "Rect");
        if (rect_obj == NULL || rect_obj->type != PDF_ARRAY || rect_obj->u.arr.count != 4)
            continue;

        rx0 = pdf_obj_num(rect_obj->u.arr.items[0], 0.0);
        ry0 = pdf_obj_num(rect_obj->u.arr.items[1], 0.0);
        rx1 = pdf_obj_num(rect_obj->u.arr.items[2], 0.0);
        ry1 = pdf_obj_num(rect_obj->u.arr.items[3], 0.0);
        rect.x0 = (rx0 < rx1) ? rx0 : rx1;
        rect.x1 = (rx0 < rx1) ? rx1 : rx0;
        rect.y0 = (ry0 < ry1) ? ry0 : ry1;
        rect.y1 = (ry0 < ry1) ? ry1 : ry0;

        draw_annot_appearance(dev, annot, rect);
    }
}
