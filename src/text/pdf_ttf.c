/* pdf_ttf.c
 *
 * Ver pdf_ttf.h.
 */

#include "pdf_ttf.h"
#include "pdf_error.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- utilidades big-endian (mismo idioma que jpx_u16/jpx_u32 en
 * pdf_jpx.c y el parseo inline de pdf_font.c -- sin helper compartido
 * entre modulos, ver pdf_jpx.c/pdf_font.c). --------------------------- */

static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static long be32(const unsigned char *p)
{
    return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | (long)p[3];
}

/* ---- pdf_ttf_load: directorio sfnt + head/maxp/loca/glyf/cmap ------- */

static void parse_cmap_directory(const unsigned char *data, long data_len,
                                  long cmap_off, pdf_ttf_font *out)
{
    long num_sub, i;
    long best_off = -1;
    int  best_fmt = 0;
    int  best_priority = -1;

    if (cmap_off + 4 > data_len) return;
    num_sub = (long)be16(data + cmap_off + 2);
    if (num_sub <= 0 || num_sub > 64) return;

    for (i = 0; i < num_sub; i++)
    {
        long rec = cmap_off + 4 + i * 8;
        int  platform, encoding, priority;
        long sub_off, fmt;

        if (rec + 8 > data_len) break;
        platform = (int)be16(data + rec);
        encoding = (int)be16(data + rec + 2);
        sub_off  = cmap_off + be32(data + rec + 4);
        if (sub_off < 0 || sub_off + 2 > data_len) continue;
        fmt = (long)be16(data + sub_off);

        /* (3,0) "Symbol" -- se guarda aparte, nunca compite por
         * cmap_unicode_off (ver DESIGN.md seccion 36: Wingdings/Symbol
         * necesitan su propia convencion code+0xF000). Se asume
         * formato 4 (universal en la practica para subtablas symbol
         * reales). */
        if (platform == 3 && encoding == 0)
        {
            out->cmap_symbol_off = sub_off;
            continue;
        }

        priority = -1;
        if (platform == 3 && encoding == 1 && fmt == 4) priority = 4;      /* Windows Unicode BMP, preferido */
        else if (platform == 0 && fmt == 4) priority = 3;                   /* Unicode generico, fmt4 */
        else if (platform == 3 && encoding == 10 && fmt == 12) priority = 2;/* Windows Unicode completo */
        else if (platform == 0 && fmt == 12) priority = 2;
        else if (fmt == 4) priority = 1;                                    /* cualquier otra fmt4 (Mac, etc.) */

        if (priority > best_priority)
        {
            best_priority = priority;
            best_off = sub_off;
            best_fmt = (int)fmt;
        }
    }

    if (best_off >= 0)
    {
        out->cmap_unicode_off    = best_off;
        out->cmap_unicode_format = best_fmt;
    }
}

int pdf_ttf_load(const unsigned char *data, long data_len, pdf_ttf_font *out)
{
    long num_tables, i;
    long head_off = -1;
    long maxp_off = -1;
    long loca_off = -1, loca_len = 0;
    long glyf_off = -1, glyf_len = 0;
    long cmap_off = -1;
    long hhea_off = -1;
    long hmtx_off = -1;

    if (data == NULL || out == NULL || data_len < 12) return PDF_ERR_BADARG;

    memset(out, 0, sizeof(*out));
    out->data     = data;
    out->data_len = data_len;

    num_tables = (long)be16(data + 4);
    if (num_tables <= 0 || num_tables > 256) return PDF_ERR_NOTFOUND;

    for (i = 0; i < num_tables; i++)
    {
        long rec = 12 + i * 16;
        long toff, tlen;

        if (rec + 16 > data_len) break;
        toff = be32(data + rec + 8);
        tlen = be32(data + rec + 12);

        if (memcmp(data + rec, "head", 4) == 0) head_off = toff;
        else if (memcmp(data + rec, "maxp", 4) == 0) maxp_off = toff;
        else if (memcmp(data + rec, "loca", 4) == 0) { loca_off = toff; loca_len = tlen; }
        else if (memcmp(data + rec, "glyf", 4) == 0) { glyf_off = toff; glyf_len = tlen; }
        else if (memcmp(data + rec, "cmap", 4) == 0) cmap_off = toff;
        else if (memcmp(data + rec, "hhea", 4) == 0) hhea_off = toff;
        else if (memcmp(data + rec, "hmtx", 4) == 0) hmtx_off = toff;
    }

    /* sin 'glyf'/'loca' (p.ej. una fuente CFF/OpenType real, sin
     * contornos TrueType clasicos) -- fuera de alcance de este modulo,
     * ver pdf_ttf.h. */
    if (head_off < 0 || head_off + 54 > data_len) return PDF_ERR_NOTFOUND;
    if (maxp_off < 0 || maxp_off + 6 > data_len) return PDF_ERR_NOTFOUND;
    if (loca_off < 0 || glyf_off < 0) return PDF_ERR_UNSUPPORTED;

    out->units_per_em = (int)be16(data + head_off + 18);
    out->loca_long    = (int)(short)be16(data + head_off + 50);
    out->num_glyphs    = (int)be16(data + maxp_off + 4);
    out->loca_off = loca_off; out->loca_len = loca_len;
    out->glyf_off = glyf_off; out->glyf_len = glyf_len;

    if (out->units_per_em <= 0) out->units_per_em = 1000; /* tolerante: valor tipico por defecto */
    if (out->num_glyphs <= 0) return PDF_ERR_NOTFOUND;

    if (cmap_off >= 0)
        parse_cmap_directory(data, data_len, cmap_off, out);

    /* 'hhea'.numberOfHMetrics vive en el offset fijo 34 (ver Apple/MS
     * OpenType spec, tabla hhea de 36 bytes) -- ver pdf_ttf_glyph_advance_em.
     * Tolerante: si falta cualquiera de las dos tablas, o el valor no
     * tiene sentido, 'hmtx_off' queda en 0 (falsy) y el llamador no
     * reescala (mismo criterio que el resto del modulo). */
    if (hhea_off >= 0 && hhea_off + 36 <= data_len && hmtx_off >= 0)
    {
        int n = (int)be16(data + hhea_off + 34);
        if (n > 0 && n <= out->num_glyphs && hmtx_off + (long)n * 4 <= data_len)
        {
            out->hmtx_off = hmtx_off;
            out->num_h_metrics = n;
        }
    }

    return PDF_OK;
}

/* ---- cmap: unicode/codigo -> glyph index ---------------------------- */

static int cmap_lookup_format4(const unsigned char *data, long data_len, long sub_off, int unicode)
{
    long seg_count_x2, seg_count;
    long end_codes, start_codes, id_deltas, id_range_offsets;
    long i;

    if (unicode < 0 || unicode > 0xFFFF) return 0;
    if (sub_off + 14 > data_len) return 0;
    if (be16(data + sub_off) != 4) return 0;

    seg_count_x2 = (long)be16(data + sub_off + 6);
    seg_count = seg_count_x2 / 2;
    if (seg_count <= 0 || seg_count > 20000) return 0;

    end_codes        = sub_off + 14;
    start_codes      = end_codes + seg_count_x2 + 2; /* +2 = reservedPad */
    id_deltas        = start_codes + seg_count_x2;
    id_range_offsets = id_deltas + seg_count_x2;

    if (id_range_offsets + seg_count_x2 > data_len) return 0;

    for (i = 0; i < seg_count; i++)
    {
        int end_code = (int)be16(data + end_codes + i * 2);

        if (unicode <= end_code)
        {
            int start_code = (int)be16(data + start_codes + i * 2);
            int id_delta   = (int)(short)be16(data + id_deltas + i * 2);
            int id_range_off = (int)be16(data + id_range_offsets + i * 2);
            int gid;

            if (unicode < start_code) return 0;

            if (id_range_off == 0)
            {
                gid = (unicode + id_delta) & 0xFFFF;
            }
            else
            {
                long addr = id_range_offsets + i * 2 + id_range_off + (long)(unicode - start_code) * 2;
                if (addr + 2 > data_len) return 0;
                gid = (int)be16(data + addr);
                if (gid != 0) gid = (gid + id_delta) & 0xFFFF;
            }
            return gid;
        }
    }
    return 0;
}

static int cmap_lookup_format12(const unsigned char *data, long data_len, long sub_off, int unicode)
{
    long n_groups, i;

    if (unicode < 0) return 0;
    if (sub_off + 16 > data_len) return 0;
    if (be16(data + sub_off) != 12) return 0;

    n_groups = be32(data + sub_off + 12);
    if (n_groups <= 0 || n_groups > 100000) return 0;
    if (sub_off + 16 + n_groups * 12 > data_len) return 0;

    for (i = 0; i < n_groups; i++)
    {
        long rec = sub_off + 16 + i * 12;
        long start = be32(data + rec);
        long end   = be32(data + rec + 4);
        long start_gid = be32(data + rec + 8);

        if ((long)unicode >= start && (long)unicode <= end)
            return (int)(start_gid + ((long)unicode - start));
    }
    return 0;
}

int pdf_ttf_gid_for_unicode(const pdf_ttf_font *font, int unicode)
{
    if (font == NULL || font->cmap_unicode_off <= 0) return 0;
    if (font->cmap_unicode_format == 4)
        return cmap_lookup_format4(font->data, font->data_len, font->cmap_unicode_off, unicode);
    if (font->cmap_unicode_format == 12)
        return cmap_lookup_format12(font->data, font->data_len, font->cmap_unicode_off, unicode);
    return 0;
}

int pdf_ttf_gid_for_symbol_code(const pdf_ttf_font *font, int code)
{
    int gid;

    if (font == NULL || font->cmap_symbol_off <= 0) return 0;

    gid = cmap_lookup_format4(font->data, font->data_len, font->cmap_symbol_off, 0xF000 + (code & 0xFF));
    if (gid == 0)
        gid = cmap_lookup_format4(font->data, font->data_len, font->cmap_symbol_off, code & 0xFF);
    return gid;
}

/* ---- glyf: contorno de un glyph -------------------------------------- */

#define PDF_TTF_MAX_CONTOURS      64
#define PDF_TTF_MAX_GLYPH_POINTS  512
#define PDF_TTF_QUAD_SEGMENTS     8

typedef struct raw_pt_s { double x, y; } raw_pt;

static int get_glyph_range(const pdf_ttf_font *font, int gid, long *off, long *len)
{
    long o0, o1;

    if (font->loca_long)
    {
        long idx = font->loca_off + (long)gid * 4;
        if (idx + 8 > font->data_len) return 0;
        o0 = be32(font->data + idx);
        o1 = be32(font->data + idx + 4);
    }
    else
    {
        long idx = font->loca_off + (long)gid * 2;
        if (idx + 4 > font->data_len) return 0;
        o0 = (long)be16(font->data + idx) * 2;
        o1 = (long)be16(font->data + idx + 2) * 2;
    }
    if (o1 < o0) return 0;
    if (font->glyf_off + o1 > font->data_len) return 0;

    *off = font->glyf_off + o0;
    *len = o1 - o0;
    return 1;
}

/* subdivide la cuadratica (x0,y0)-(cx,cy)-(x1,y1) (unidades crudas de
 * fuente) en PDF_TTF_QUAD_SEGMENTS segmentos de recta, emitidos ya
 * normalizados a em (1.0 = un em) via 'lineto'. */
static void quad_flatten(double x0, double y0, double cx, double cy, double x1, double y1,
                          double inv_upm, pdf_ttf_lineto_fn lineto, void *user)
{
    int i;
    for (i = 1; i <= PDF_TTF_QUAD_SEGMENTS; i++)
    {
        double t  = (double)i / (double)PDF_TTF_QUAD_SEGMENTS;
        double mt = 1.0 - t;
        double x  = mt * mt * x0 + 2.0 * mt * t * cx + t * t * x1;
        double y  = mt * mt * y0 + 2.0 * mt * t * cy + t * t * y1;
        lineto(user, x * inv_upm, y * inv_upm);
    }
}

/* Decodifica UN contorno (n puntos crudos + flags on-curve) aplicando
 * la regla estandar de TrueType: dos puntos off-curve consecutivos
 * implican un punto on-curve en su punto medio. Ver comentario de
 * diseño en pdf_ttf.h -- algoritmo clasico de "decomposicion glyf"
 * (equivalente al usado por FreeType/stb_truetype para este mismo
 * paso), sin atajos. */
static void emit_contour(const raw_pt *pts, const int *on_curve, int n, double inv_upm,
                          pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto, void *user)
{
    int start_idx;
    double sx, sy, cx, cy;
    int have_ctrl = 0;
    double ctrl_x = 0.0, ctrl_y = 0.0;
    int k, count;

    if (n <= 0) return;

    if (on_curve[0])       { start_idx = 0;   sx = pts[0].x;   sy = pts[0].y; }
    else if (on_curve[n-1]) { start_idx = n-1; sx = pts[n-1].x; sy = pts[n-1].y; }
    else                     { start_idx = -1; sx = (pts[0].x + pts[n-1].x) * 0.5;
                                                  sy = (pts[0].y + pts[n-1].y) * 0.5; }

    moveto(user, sx * inv_upm, sy * inv_upm);
    cx = sx; cy = sy;

    count = (start_idx >= 0) ? n - 1 : n;
    for (k = 0; k < count; k++)
    {
        int idx = (start_idx >= 0) ? (start_idx + 1 + k) % n : k;
        double px = pts[idx].x, py = pts[idx].y;

        if (on_curve[idx])
        {
            if (have_ctrl)
            {
                quad_flatten(cx, cy, ctrl_x, ctrl_y, px, py, inv_upm, lineto, user);
                have_ctrl = 0;
            }
            else
            {
                lineto(user, px * inv_upm, py * inv_upm);
            }
            cx = px; cy = py;
        }
        else
        {
            if (have_ctrl)
            {
                double mx = (ctrl_x + px) * 0.5, my = (ctrl_y + py) * 0.5;
                quad_flatten(cx, cy, ctrl_x, ctrl_y, mx, my, inv_upm, lineto, user);
                cx = mx; cy = my;
            }
            ctrl_x = px; ctrl_y = py;
            have_ctrl = 1;
        }
    }

    if (have_ctrl)
        quad_flatten(cx, cy, ctrl_x, ctrl_y, sx, sy, inv_upm, lineto, user);
    else if (cx != sx || cy != sy)
        lineto(user, sx * inv_upm, sy * inv_upm);
}

static int decode_simple_glyph(const unsigned char *g, long glen, double inv_upm,
                                pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto, void *user)
{
    int num_contours;
    long pos;
    int end_pts[PDF_TTF_MAX_CONTOURS];
    int num_points, i, c, start;
    int instr_len;
    unsigned char flags[PDF_TTF_MAX_GLYPH_POINTS];
    int on_curve[PDF_TTF_MAX_GLYPH_POINTS];
    raw_pt pts[PDF_TTF_MAX_GLYPH_POINTS];
    double x, y;

    if (glen < 10) return PDF_ERR_BADARG;
    num_contours = (int)(short)be16(g);
    if (num_contours <= 0 || num_contours > PDF_TTF_MAX_CONTOURS) return PDF_ERR_UNSUPPORTED;

    pos = 10;
    if (pos + num_contours * 2 + 2 > glen) return PDF_ERR_BADARG;
    for (i = 0; i < num_contours; i++)
    {
        end_pts[i] = (int)be16(g + pos);
        pos += 2;
    }

    num_points = end_pts[num_contours - 1] + 1;
    if (num_points <= 0 || num_points > PDF_TTF_MAX_GLYPH_POINTS) return PDF_ERR_UNSUPPORTED;

    instr_len = (int)be16(g + pos);
    pos += 2 + instr_len;
    if (pos > glen) return PDF_ERR_BADARG;

    /* flags, con repeat-count (bit 0x08) */
    i = 0;
    while (i < num_points)
    {
        unsigned char f;
        int repeat;

        if (pos >= glen) return PDF_ERR_BADARG;
        f = g[pos++];
        flags[i] = f;
        on_curve[i] = (f & 0x01) ? 1 : 0;
        i++;

        if (f & 0x08)
        {
            if (pos >= glen) return PDF_ERR_BADARG;
            repeat = g[pos++];
            while (repeat-- > 0 && i < num_points)
            {
                flags[i] = f;
                on_curve[i] = (f & 0x01) ? 1 : 0;
                i++;
            }
        }
    }

    /* deltas X (0x02=short vector, 0x10=positivo si short / "mismo valor" si no) */
    x = 0.0;
    for (i = 0; i < num_points; i++)
    {
        unsigned char f = flags[i];
        if (f & 0x02)
        {
            int dx;
            if (pos >= glen) return PDF_ERR_BADARG;
            dx = g[pos++];
            x += (f & 0x10) ? dx : -dx;
        }
        else if (!(f & 0x10))
        {
            if (pos + 2 > glen) return PDF_ERR_BADARG;
            x += (double)(short)be16(g + pos);
            pos += 2;
        }
        pts[i].x = x;
    }

    /* deltas Y (0x04=short vector, 0x20=positivo si short / "mismo valor" si no) */
    y = 0.0;
    for (i = 0; i < num_points; i++)
    {
        unsigned char f = flags[i];
        if (f & 0x04)
        {
            int dy;
            if (pos >= glen) return PDF_ERR_BADARG;
            dy = g[pos++];
            y += (f & 0x20) ? dy : -dy;
        }
        else if (!(f & 0x20))
        {
            if (pos + 2 > glen) return PDF_ERR_BADARG;
            y += (double)(short)be16(g + pos);
            pos += 2;
        }
        pts[i].y = y;
    }

    start = 0;
    for (c = 0; c < num_contours; c++)
    {
        int cend = end_pts[c];
        int n = cend - start + 1;
        if (n > 0)
            emit_contour(&pts[start], &on_curve[start], n, inv_upm, moveto, lineto, user);
        start = cend + 1;
    }

    return PDF_OK;
}

/* adaptador que aplica la transformacion afin (a,b,c,d,dx,dy -- dx/dy
 * ya normalizados a em) de un componente de glyph compuesto antes de
 * reenviar al moveto/lineto reales del llamador original. */
typedef struct comp_ctx_s
{
    pdf_ttf_moveto_fn moveto;
    pdf_ttf_lineto_fn lineto;
    void *user;
    double a, b, c, d, dx, dy;
} comp_ctx;

static void comp_moveto(void *u, double x, double y)
{
    comp_ctx *cc = (comp_ctx *)u;
    cc->moveto(cc->user, cc->a * x + cc->c * y + cc->dx, cc->b * x + cc->d * y + cc->dy);
}

static void comp_lineto(void *u, double x, double y)
{
    comp_ctx *cc = (comp_ctx *)u;
    cc->lineto(cc->user, cc->a * x + cc->c * y + cc->dx, cc->b * x + cc->d * y + cc->dy);
}

int pdf_ttf_glyph_outline(const pdf_ttf_font *font, int gid,
                           pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto,
                           void *user, int depth);

static int decode_composite_glyph(const pdf_ttf_font *font, const unsigned char *g, long glen,
                                   double inv_upm, pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto,
                                   void *user, int depth)
{
    long pos = 10;
    int more = 1;

    while (more)
    {
        unsigned int flags, glyph_index;
        double arg1, arg2;
        double a, b, c, d;
        comp_ctx cc;

        if (pos + 4 > glen) return PDF_ERR_BADARG;
        flags = be16(g + pos); pos += 2;
        glyph_index = be16(g + pos); pos += 2;

        if (flags & 0x0001) /* ARG_1_AND_2_ARE_WORDS */
        {
            if (pos + 4 > glen) return PDF_ERR_BADARG;
            arg1 = (double)(short)be16(g + pos); pos += 2;
            arg2 = (double)(short)be16(g + pos); pos += 2;
        }
        else
        {
            if (pos + 2 > glen) return PDF_ERR_BADARG;
            arg1 = (double)(signed char)g[pos]; pos += 1;
            arg2 = (double)(signed char)g[pos]; pos += 1;
        }

        a = 1.0; b = 0.0; c = 0.0; d = 1.0;
        if (flags & 0x0008) /* WE_HAVE_A_SCALE */
        {
            if (pos + 2 > glen) return PDF_ERR_BADARG;
            a = d = (double)(short)be16(g + pos) / 16384.0; pos += 2;
        }
        else if (flags & 0x0040) /* WE_HAVE_AN_X_AND_Y_SCALE */
        {
            if (pos + 4 > glen) return PDF_ERR_BADARG;
            a = (double)(short)be16(g + pos) / 16384.0; pos += 2;
            d = (double)(short)be16(g + pos) / 16384.0; pos += 2;
        }
        else if (flags & 0x0080) /* WE_HAVE_A_TWO_BY_TWO */
        {
            if (pos + 8 > glen) return PDF_ERR_BADARG;
            a = (double)(short)be16(g + pos) / 16384.0; pos += 2;
            b = (double)(short)be16(g + pos) / 16384.0; pos += 2;
            c = (double)(short)be16(g + pos) / 16384.0; pos += 2;
            d = (double)(short)be16(g + pos) / 16384.0; pos += 2;
        }

        /* ARGS_ARE_XY_VALUES (0x0002): si no esta seteado, arg1/arg2
         * son indices de "point matching" (alineacion por punto en vez
         * de traslacion directa) -- caso raro, fuera de alcance
         * (mejor esfuerzo: se procesa igual el sub-glyph pero sin
         * traslacion, en vez de abortar todo el glyph compuesto). */
        cc.moveto = moveto; cc.lineto = lineto; cc.user = user;
        cc.a = a; cc.b = b; cc.c = c; cc.d = d;
        cc.dx = (flags & 0x0002) ? arg1 * inv_upm : 0.0;
        cc.dy = (flags & 0x0002) ? arg2 * inv_upm : 0.0;
        pdf_ttf_glyph_outline(font, (int)glyph_index, comp_moveto, comp_lineto, &cc, depth + 1);

        more = (flags & 0x0020) ? 1 : 0; /* MORE_COMPONENTS */
    }

    return PDF_OK;
}

int pdf_ttf_glyph_outline(const pdf_ttf_font *font, int gid,
                           pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto,
                           void *user, int depth)
{
    long goff, glen;
    int num_contours;
    double inv_upm;

    if (font == NULL || moveto == NULL || lineto == NULL) return PDF_ERR_BADARG;
    if (depth > 8) return PDF_ERR_UNSUPPORTED; /* guarda contra recursion infinita, ver pdf_ttf.h */
    if (font->units_per_em <= 0) return PDF_ERR_BADARG;
    if (gid < 0 || gid >= font->num_glyphs) return PDF_ERR_NOTFOUND;

    if (!get_glyph_range(font, gid, &goff, &glen)) return PDF_ERR_NOTFOUND;
    if (glen < 10) return PDF_ERR_NOTFOUND; /* glyph vacio (p.ej. espacio): nada que dibujar, no es error */

    inv_upm = 1.0 / (double)font->units_per_em;
    num_contours = (int)(short)be16(font->data + goff);

    if (num_contours >= 0)
        return decode_simple_glyph(font->data + goff, glen, inv_upm, moveto, lineto, user);

    return decode_composite_glyph(font, font->data + goff, glen, inv_upm, moveto, lineto, user, depth);
}

double pdf_ttf_glyph_advance_em(const pdf_ttf_font *font, int gid)
{
    long idx;
    unsigned int aw;

    if (font == NULL || font->hmtx_off <= 0 || font->units_per_em <= 0) return -1.0;
    if (gid < 0) return -1.0;

    /* T.OpenType hmtx: array de {advanceWidth(u16), lsb(s16)} de
     * tamanio 'num_h_metrics' -- glyphs con gid >= num_h_metrics
     * reusan el ULTIMO advanceWidth de esa tabla (monospace implicito
     * para el resto, solo el lsb individual seguiria despues -- no
     * nos hace falta el lsb aca). */
    idx = (gid < font->num_h_metrics) ? gid : (font->num_h_metrics - 1);
    if (idx < 0) return -1.0;

    aw = be16(font->data + font->hmtx_off + idx * 4);
    return (double)aw / (double)font->units_per_em;
}

/* ---- sustitucion por fuente de sistema, sin GDI ---------------------- */

static int name_contains_ci_local(const char *haystack, const char *needle)
{
    size_t hn, nn, i;

    if (haystack == NULL) return 0;
    hn = strlen(haystack);
    nn = strlen(needle);
    if (nn == 0 || nn > hn) return 0;

    for (i = 0; i + nn <= hn; i++)
    {
        size_t j;
        int match = 1;
        for (j = 0; j < nn; j++)
        {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

#define TTF_CACHE_MAX          16
#define TTF_CACHE_BUDGET_BYTES (32UL * 1024UL * 1024UL)
#define TTF_CACHE_BLOCK_BYTES  (256UL * 1024UL)
#define TTF_MAX_FILE_BYTES     (16L * 1024L * 1024L)

typedef struct ttf_cache_entry_s
{
    char          path[260];
    pdf_ttf_font  font;
    int           loaded; /* 0=vacio, 1=ok, -1=intento fallido (no reintentar) */
} ttf_cache_entry;

/* Cache de vida de PROCESO (arena+ledger dedicados, nunca reseteada) --
 * excepcion deliberada y acotada al patron arena=doc/page/decode del
 * resto del motor, ver pdf_ttf.h y DESIGN.md. */
static pdf_ledger      g_ttf_ledger;
static pdf_arena       g_ttf_arena;
static int             g_ttf_arena_ready = 0;
static ttf_cache_entry g_ttf_cache[TTF_CACHE_MAX];
static int             g_ttf_cache_count = 0;

static void ttf_cache_ensure_init(void)
{
    if (g_ttf_arena_ready) return;
    pdf_ledger_init(&g_ttf_ledger, TTF_CACHE_BUDGET_BYTES);
    pdf_arena_init(&g_ttf_arena, &g_ttf_ledger, TTF_CACHE_BLOCK_BYTES, "ttf_syscache");
    g_ttf_arena_ready = 1;
}

static const pdf_ttf_font *ttf_cache_load(const char *path)
{
    int i;
    FILE *fp;
    long len;
    unsigned char *buf;
    ttf_cache_entry *e;

    for (i = 0; i < g_ttf_cache_count; i++)
    {
        if (strcmp(g_ttf_cache[i].path, path) == 0)
            return (g_ttf_cache[i].loaded == 1) ? &g_ttf_cache[i].font : NULL;
    }
    if (g_ttf_cache_count >= TTF_CACHE_MAX) return NULL;

    ttf_cache_ensure_init();

    e = &g_ttf_cache[g_ttf_cache_count];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = 0;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        e->loaded = -1;
        g_ttf_cache_count++;
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0 || len > TTF_MAX_FILE_BYTES)
    {
        fclose(fp);
        e->loaded = -1;
        g_ttf_cache_count++;
        return NULL;
    }

    buf = (unsigned char *)pdf_arena_alloc(&g_ttf_arena, (size_t)len);
    if (buf == NULL)
    {
        fclose(fp);
        e->loaded = -1;
        g_ttf_cache_count++;
        return NULL;
    }

    if (fread(buf, 1, (size_t)len, fp) != (size_t)len)
    {
        fclose(fp);
        e->loaded = -1;
        g_ttf_cache_count++;
        return NULL;
    }
    fclose(fp);

    if (pdf_ttf_load(buf, len, &e->font) != PDF_OK)
    {
        e->loaded = -1;
        g_ttf_cache_count++;
        return NULL;
    }

    e->loaded = 1;
    g_ttf_cache_count++;
    return &e->font;
}

typedef struct family_map_s { const char *keyword; const char *file_base; } family_map;

static const family_map FAMILY_MAP[] = {
    { "courier",   "cour" },
    { "consolas",  "cour" },
    { "mono",      "cour" },
    { "times",     "times" },
    { "georgia",   "times" },
    { "garamond",  "times" },
    { "cambria",   "times" },
    { "minion",    "times" },
    { "arial",     "arial" },
    { "helvetica", "arial" },
    { "calibri",   "arial" },
    { "segoe",     "arial" },
    { "verdana",   "arial" },
    { "tahoma",    "arial" },
    { NULL, NULL }
};

static void fonts_dir(char *out, size_t out_size)
{
    const char *root = getenv("SystemRoot");
    if (root == NULL || root[0] == '\0') root = "C:\\Windows";
    strncpy(out, root, out_size - 1);
    out[out_size - 1] = 0;
    strncat(out, "\\Fonts\\", out_size - strlen(out) - 1);
}

static const pdf_ttf_font *load_family_file(const char *dir, const char *base, const char *suffix)
{
    char fname[32];
    char path[260];

    strncpy(fname, base, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = 0;
    strncat(fname, suffix, sizeof(fname) - strlen(fname) - 1);
    strncat(fname, ".ttf", sizeof(fname) - strlen(fname) - 1);

    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    strncat(path, fname, sizeof(path) - strlen(path) - 1);

    return ttf_cache_load(path);
}

const pdf_ttf_font *pdf_ttf_find_system_font(const char *base_font_name,
                                              int is_bold, int is_italic, int is_serif)
{
    char dir[240];
    const char *base = NULL;
    const char *suffix;
    const pdf_ttf_font *f;
    int i;

    fonts_dir(dir, sizeof(dir));

    if (base_font_name != NULL)
    {
        for (i = 0; FAMILY_MAP[i].keyword != NULL; i++)
        {
            if (name_contains_ci_local(base_font_name, FAMILY_MAP[i].keyword))
            {
                base = FAMILY_MAP[i].file_base;
                break;
            }
        }
    }
    if (base == NULL)
        base = is_serif ? "times" : "arial";

    /* arial/times/cour comparten la misma convencion de sufijo en
     * Windows desde Win95: "" regular, "bd" bold, "i" italic, "bi"
     * bold+italic -- las 3 familias soportadas con sufijo caen aca. */
    if (is_bold && is_italic) suffix = "bi";
    else if (is_bold)         suffix = "bd";
    else if (is_italic)       suffix = "i";
    else                       suffix = "";

    f = load_family_file(dir, base, suffix);
    if (f != NULL) return f;

    /* degradar a variante regular de la misma familia antes de
     * rendirse (algunos sistemas no tienen las 4 variantes de cada
     * familia instaladas). */
    if (suffix[0] != '\0')
    {
        f = load_family_file(dir, base, "");
        if (f != NULL) return f;
    }

    /* ultimo fallback: arial.ttf regular */
    if (strcmp(base, "arial") != 0)
        f = load_family_file(dir, "arial", "");

    return f;
}

const pdf_ttf_font *pdf_ttf_find_system_symbol_font(const char *base_font_name)
{
    char dir[240];
    char path[260];
    const char *file = "symbol.ttf";

    if (base_font_name != NULL)
    {
        if (name_contains_ci_local(base_font_name, "wingding")) file = "wingding.ttf";
        else if (name_contains_ci_local(base_font_name, "webding")) file = "webding.ttf";
    }

    fonts_dir(dir, sizeof(dir));
    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    strncat(path, file, sizeof(path) - strlen(path) - 1);

    return ttf_cache_load(path);
}
