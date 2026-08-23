/* pdf_bitmap.c
 *
 * Ver pdf_bitmap.h.
 */

#include "pdf_bitmap.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

int pdf_bitmap_create(pdf_arena *arena, int width, int height, pdf_bitmap *bmp)
{
    long size;

    if (arena == NULL || bmp == NULL || width <= 0 || height <= 0)
        return PDF_ERR_BADARG;

    bmp->width  = width;
    bmp->height = height;
    bmp->stride = width * 3;

    size = (long)bmp->stride * (long)height;
    bmp->pixels = (unsigned char *)pdf_arena_alloc(arena, (size_t)size);
    if (bmp->pixels == NULL)
        return PDF_ERR_NOMEM;

    bmp->alpha = (unsigned char *)pdf_arena_alloc(arena, (size_t)((long)width * (long)height));
    if (bmp->alpha == NULL)
        return PDF_ERR_NOMEM;

    /* blanco */
    {
        long i;
        for (i = 0; i < size; i++)
            bmp->pixels[i] = 255;
        for (i = 0; i < (long)width * (long)height; i++)
            bmp->alpha[i] = 255;
    }

    /* sin clip por defecto: todo el bitmap */
    bmp->clip_x0 = 0;
    bmp->clip_y0 = 0;
    bmp->clip_x1 = width;
    bmp->clip_y1 = height;
    bmp->clip_mask = NULL;
    bmp->clip_mask_stride = 0;
    bmp->soft_mask = NULL;
    bmp->soft_mask_stride = 0;
    bmp->opacity = 1.0;
    bmp->blend_mode = PDF_BLEND_NORMAL;
    bmp->knockout = 0;
    bmp->knockout_mask = NULL;
    bmp->knockout_mask_stride = 0;
    bmp->knockout_backdrop_pixels = NULL;
    bmp->knockout_backdrop_alpha = NULL;
    bmp->knockout_backdrop_stride = 0;

    return PDF_OK;
}

int pdf_bitmap_create_transparent(pdf_arena *arena, int width, int height, pdf_bitmap *bmp)
{
    long size;
    long i;
    if (arena == NULL || bmp == NULL || width <= 0 || height <= 0) return PDF_ERR_BADARG;
    bmp->width = width;
    bmp->height = height;
    bmp->stride = width * 3;
    size = (long)width * (long)height;
    bmp->pixels = (unsigned char *)pdf_arena_alloc(arena, (size_t)(size * 3L));
    if (bmp->pixels == NULL) return PDF_ERR_NOMEM;
    bmp->alpha = (unsigned char *)pdf_arena_alloc(arena, (size_t)size);
    if (bmp->alpha == NULL) return PDF_ERR_NOMEM;
    for (i = 0; i < size * 3L; i++) bmp->pixels[i] = 0;
    for (i = 0; i < size; i++) bmp->alpha[i] = 0;
    bmp->clip_x0 = 0; bmp->clip_y0 = 0; bmp->clip_x1 = width; bmp->clip_y1 = height;
    bmp->clip_mask = NULL; bmp->clip_mask_stride = 0;
    bmp->soft_mask = NULL; bmp->soft_mask_stride = 0;
    bmp->opacity = 1.0; bmp->blend_mode = PDF_BLEND_NORMAL;
    bmp->knockout = 0; bmp->knockout_mask = NULL; bmp->knockout_mask_stride = 0;
    bmp->knockout_backdrop_pixels = NULL; bmp->knockout_backdrop_alpha = NULL;
    bmp->knockout_backdrop_stride = 0;
    return PDF_OK;
}

void pdf_bitmap_set_clip_mask(pdf_bitmap *bmp, unsigned char *mask, int stride)
{
    if (bmp == NULL) return;
    bmp->clip_mask = mask;
    bmp->clip_mask_stride = (mask != NULL && stride > 0) ? stride : 0;
}

void pdf_bitmap_set_soft_mask(pdf_bitmap *bmp, unsigned char *mask, int stride)
{
    if (bmp == NULL) return;
    bmp->soft_mask = mask;
    bmp->soft_mask_stride = (mask != NULL && stride > 0) ? stride : 0;
}

void pdf_bitmap_set_opacity(pdf_bitmap *bmp, double opacity)
{
    if (bmp == NULL) return;
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    bmp->opacity = opacity;
}

void pdf_bitmap_set_blend_mode(pdf_bitmap *bmp, int mode)
{
    if (bmp == NULL) return;
    if (mode < PDF_BLEND_NORMAL || mode > PDF_BLEND_LUMINOSITY)
        mode = PDF_BLEND_NORMAL;
    bmp->blend_mode = mode;
}

int pdf_bitmap_enable_knockout(pdf_bitmap *bmp, pdf_arena *arena,
                               const pdf_bitmap *backdrop)
{
    long count;
    long rgb_size;
    long i;

    if (bmp == NULL || arena == NULL || bmp->width <= 0 || bmp->height <= 0)
        return PDF_ERR_BADARG;

    count = (long)bmp->width * (long)bmp->height;
    rgb_size = count * 3L;

    bmp->knockout_mask = (unsigned char *)pdf_arena_alloc(arena, (size_t)count);
    if (bmp->knockout_mask == NULL)
        return PDF_ERR_NOMEM;
    for (i = 0; i < count; i++)
        bmp->knockout_mask[i] = 0;
    bmp->knockout_mask_stride = bmp->width;

    bmp->knockout_backdrop_pixels = (unsigned char *)pdf_arena_alloc(arena, (size_t)rgb_size);
    if (bmp->knockout_backdrop_pixels == NULL)
        return PDF_ERR_NOMEM;
    bmp->knockout_backdrop_alpha = (unsigned char *)pdf_arena_alloc(arena, (size_t)count);
    if (bmp->knockout_backdrop_alpha == NULL)
        return PDF_ERR_NOMEM;
    bmp->knockout_backdrop_stride = bmp->width * 3;

    if (backdrop != NULL && backdrop->pixels != NULL && backdrop->alpha != NULL)
    {
        int y;
        int rows = bmp->height < backdrop->height ? bmp->height : backdrop->height;
        int cols = bmp->width < backdrop->width ? bmp->width : backdrop->width;
        for (y = 0; y < rows; y++)
        {
            memcpy(bmp->knockout_backdrop_pixels + (long)y * bmp->knockout_backdrop_stride,
                   backdrop->pixels + (long)y * backdrop->stride,
                   (size_t)((long)cols * 3L));
            memcpy(bmp->knockout_backdrop_alpha + (long)y * bmp->knockout_mask_stride,
                   backdrop->alpha + (long)y * backdrop->width,
                   (size_t)cols);
        }
        /* Si las dimensiones coinciden, todo queda cubierto. Para una
         * diferencia de tamaño, las zonas restantes permanecen transparentes. */
    }
    else
    {
        for (i = 0; i < rgb_size; i++)
            bmp->knockout_backdrop_pixels[i] = 0;
        for (i = 0; i < count; i++)
            bmp->knockout_backdrop_alpha[i] = 0;
    }

    bmp->knockout = 1;
    return PDF_OK;
}

void pdf_bitmap_set_knockout(pdf_bitmap *bmp, int enabled)
{
    if (bmp == NULL) return;
    bmp->knockout = enabled ? 1 : 0;
}

static double hsl_max3(double a, double b, double c)
{
    double v = a > b ? a : b;
    return v > c ? v : c;
}

static double hsl_min3(double a, double b, double c)
{
    double v = a < b ? a : b;
    return v < c ? v : c;
}

static double hsl_hue2rgb(double p, double q, double t)
{
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

static void rgb_to_hsl(double r, double g, double b,
                       double *h, double *s, double *l)
{
    double maxv, minv, d;
    maxv = hsl_max3(r, g, b);
    minv = hsl_min3(r, g, b);
    *l = (maxv + minv) * 0.5;
    d = maxv - minv;
    if (d == 0.0)
    {
        *h = 0.0;
        *s = 0.0;
        return;
    }
    *s = d / (1.0 - fabs(2.0 * (*l) - 1.0));
    if (maxv == r)
    {
        *h = (g - b) / d;
        if (g < b) *h += 6.0;
    }
    else if (maxv == g)
        *h = (b - r) / d + 2.0;
    else
        *h = (r - g) / d + 4.0;
    *h /= 6.0;
}

static void hsl_to_rgb(double h, double s, double l,
                       double *r, double *g, double *b)
{
    double q, p;
    if (s == 0.0)
    {
        *r = *g = *b = l;
        return;
    }
    q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    p = 2.0 * l - q;
    *r = hsl_hue2rgb(p, q, h + 1.0 / 3.0);
    *g = hsl_hue2rgb(p, q, h);
    *b = hsl_hue2rgb(p, q, h - 1.0 / 3.0);
}

static double color_luminance(double r, double g, double b)
{
    return 0.3 * r + 0.59 * g + 0.11 * b;
}

static double color_clip(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static void set_saturation_value(double *r, double *g, double *b, double sat)
{
    double minv = hsl_min3(*r, *g, *b);
    double maxv = hsl_max3(*r, *g, *b);
    double d = maxv - minv;
    double scale;
    if (d <= 0.0)
    {
        *r = *g = *b = 0.0;
        return;
    }
    scale = sat / d;
    *r = color_clip((*r - minv) * scale);
    *g = color_clip((*g - minv) * scale);
    *b = color_clip((*b - minv) * scale);
}

static double set_lum_component(double n, double d)
{
    return color_clip(n + d);
}

static void blend_hsl(double cb_r, double cb_g, double cb_b,
                      double cs_r, double cs_g, double cs_b,
                      int mode, double *r, double *g, double *b)
{
    double bh, bs, bl, sh, ss, sl;
    double h, s, l;
    double base_r, base_g, base_b;
    double lum, d;

    rgb_to_hsl(cb_r, cb_g, cb_b, &bh, &bs, &bl);
    rgb_to_hsl(cs_r, cs_g, cs_b, &sh, &ss, &sl);

    if (mode == PDF_BLEND_HUE)
    {
        h = sh; s = bs; l = bl;
        hsl_to_rgb(h, s, l, r, g, b);
        return;
    }

    if (mode == PDF_BLEND_SATURATION)
    {
        base_r = cb_r; base_g = cb_g; base_b = cb_b;
        set_saturation_value(&base_r, &base_g, &base_b, ss);
        lum = color_luminance(cb_r, cb_g, cb_b);
        d = lum - color_luminance(base_r, base_g, base_b);
        *r = set_lum_component(base_r, d);
        *g = set_lum_component(base_g, d);
        *b = set_lum_component(base_b, d);
        return;
    }

    if (mode == PDF_BLEND_COLOR)
    {
        h = sh; s = ss; l = bl;
        hsl_to_rgb(h, s, l, r, g, b);
        return;
    }

    /* Luminosity: preserve backdrop hue/saturation and replace luminance. */
    h = bh; s = bs; l = sl;
    hsl_to_rgb(h, s, l, r, g, b);
}


static double blend_channel(double cb, double cs, int mode)
{
    double v;
    switch (mode)
    {
    case PDF_BLEND_MULTIPLY:
        return cb * cs;
    case PDF_BLEND_SCREEN:
        return cb + cs - cb * cs;
    case PDF_BLEND_OVERLAY:
        if (cb <= 0.5) return 2.0 * cb * cs;
        return 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    case PDF_BLEND_DARKEN:
        return cb < cs ? cb : cs;
    case PDF_BLEND_LIGHTEN:
        return cb > cs ? cb : cs;
    case PDF_BLEND_COLOR_DODGE:
        if (cs >= 1.0) return 1.0;
        v = cb / (1.0 - cs);
        return v > 1.0 ? 1.0 : v;
    case PDF_BLEND_COLOR_BURN:
        if (cs <= 0.0) return 0.0;
        v = 1.0 - ((1.0 - cb) / cs);
        return v < 0.0 ? 0.0 : v;
    case PDF_BLEND_HARD_LIGHT:
        if (cs <= 0.5) return 2.0 * cb * cs;
        return 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
    case PDF_BLEND_SOFT_LIGHT:
        if (cs <= 0.5)
            return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        if (cb <= 0.25)
            v = ((16.0 * cb - 12.0) * cb + 4.0) * cb;
        else
            v = sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (v - cb);
    case PDF_BLEND_DIFFERENCE:
        return cb > cs ? cb - cs : cs - cb;
    case PDF_BLEND_EXCLUSION:
        return cb + cs - 2.0 * cb * cs;
    case PDF_BLEND_HUE:
    case PDF_BLEND_SATURATION:
    case PDF_BLEND_COLOR:
    case PDF_BLEND_LUMINOSITY:
        /* HSL modes are handled per RGB triplet by blend_rgb_triplet. */
        return cs;
    default:
        v = cs;
        return v;
    }
}

void pdf_bitmap_set_clip(pdf_bitmap *bmp, int x0, int y0, int x1, int y1)
{
    if (bmp == NULL) return;

    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bmp->width)  x1 = bmp->width;
    if (y1 > bmp->height) y1 = bmp->height;

    bmp->clip_x0 = x0;
    bmp->clip_y0 = y0;
    bmp->clip_x1 = x1;
    bmp->clip_y1 = y1;
}

static unsigned char clamp_u8(double v)
{
    if (v < 0.0) return 0;
    if (v > 1.0) return 255;
    return (unsigned char)(v * 255.0 + 0.5);
}

void pdf_bitmap_set_pixel(pdf_bitmap *bmp, int x, int y, pdf_color c)
{
    pdf_bitmap_set_pixel_coverage(bmp, x, y, c, 1.0);
}

void pdf_bitmap_set_pixel_coverage(pdf_bitmap *bmp, int x, int y,
                                   pdf_color c, double coverage)
{
    unsigned char *p;
    double sa, da, oa;
    double dr, dg, db, br, bg, bb;

    if (bmp == NULL || bmp->pixels == NULL || bmp->alpha == NULL) return;
    if (x < 0 || y < 0 || x >= bmp->width || y >= bmp->height) return;
    if (x < bmp->clip_x0 || x >= bmp->clip_x1 || y < bmp->clip_y0 || y >= bmp->clip_y1) return;

    if (coverage < 0.0) coverage = 0.0;
    if (coverage > 1.0) coverage = 1.0;

    sa = bmp->opacity * coverage;
    if (bmp->clip_mask != NULL)
        sa *= (double)bmp->clip_mask[y * bmp->clip_mask_stride + x] / 255.0;
    if (bmp->soft_mask != NULL)
        sa *= (double)bmp->soft_mask[y * bmp->soft_mask_stride + x] / 255.0;
    if (sa <= 0.0) return;
    if (sa > 1.0) sa = 1.0;

    p = bmp->pixels + (long)y * bmp->stride + (long)x * 3;
    dr = (double)p[0] / 255.0;
    dg = (double)p[1] / 255.0;
    db = (double)p[2] / 255.0;
    da = (double)bmp->alpha[(long)y * bmp->width + x] / 255.0;

    /* En knockout solo se elimina la contribucion de objetos anteriores
     * del grupo. El backdrop se conserva y se restaura antes de pintar el
     * nuevo objeto. */
    if (bmp->knockout && bmp->knockout_mask != NULL &&
        bmp->knockout_mask[(long)y * bmp->knockout_mask_stride + x] != 0)
    {
        long bi = (long)y * bmp->knockout_backdrop_stride + (long)x * 3L;
        long ai = (long)y * bmp->knockout_mask_stride + x;
        p[0] = bmp->knockout_backdrop_pixels[bi];
        p[1] = bmp->knockout_backdrop_pixels[bi + 1];
        p[2] = bmp->knockout_backdrop_pixels[bi + 2];
        bmp->alpha[ai] = bmp->knockout_backdrop_alpha[ai];
        dr = (double)p[0] / 255.0;
        dg = (double)p[1] / 255.0;
        db = (double)p[2] / 255.0;
        da = (double)bmp->alpha[ai] / 255.0;
    }

    if (bmp->blend_mode >= PDF_BLEND_HUE && bmp->blend_mode <= PDF_BLEND_LUMINOSITY)
        blend_hsl(dr, dg, db, c.r, c.g, c.b, bmp->blend_mode, &br, &bg, &bb);
    else
    {
        br = blend_channel(dr, c.r, bmp->blend_mode);
        bg = blend_channel(dg, c.g, bmp->blend_mode);
        bb = blend_channel(db, c.b, bmp->blend_mode);
    }
    oa = sa + da * (1.0 - sa);
    if (oa <= 0.0) return;
    p[0] = clamp_u8((br * sa * da + c.r * sa * (1.0 - da) + dr * da * (1.0 - sa)) / oa);
    p[1] = clamp_u8((bg * sa * da + c.g * sa * (1.0 - da) + dg * da * (1.0 - sa)) / oa);
    p[2] = clamp_u8((bb * sa * da + c.b * sa * (1.0 - da) + db * da * (1.0 - sa)) / oa);
    bmp->alpha[(long)y * bmp->width + x] = clamp_u8(oa);
    if (bmp->knockout && bmp->knockout_mask != NULL && sa > 0.0)
        bmp->knockout_mask[(long)y * bmp->knockout_mask_stride + x] = 255;
}

void pdf_bitmap_copy(pdf_bitmap *dst, const pdf_bitmap *src)
{
    int y;
    int rows;
    if (dst == NULL || src == NULL || dst->pixels == NULL || src->pixels == NULL ||
        dst->alpha == NULL || src->alpha == NULL) return;
    rows = dst->height < src->height ? dst->height : src->height;
    for (y = 0; y < rows; y++)
    {
        int cols = dst->width < src->width ? dst->width : src->width;
        long bytes = (long)cols * 3L;
        long ai = (long)y * dst->width;
        long si = (long)y * src->width;
        memcpy(dst->pixels + (long)y * dst->stride,
               src->pixels + (long)y * src->stride, (size_t)bytes);
        memcpy(dst->alpha + ai, src->alpha + si, (size_t)cols);
    }
}

void pdf_bitmap_composite(pdf_bitmap *dst, const pdf_bitmap *src, double opacity, int blend_mode)
{
    int x, y;
    if (dst == NULL || src == NULL || dst->pixels == NULL || src->pixels == NULL ||
        dst->alpha == NULL || src->alpha == NULL) return;
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    if (blend_mode < PDF_BLEND_NORMAL || blend_mode > PDF_BLEND_LUMINOSITY) blend_mode = PDF_BLEND_NORMAL;
    for (y = 0; y < dst->height && y < src->height; y++)
    {
        for (x = 0; x < dst->width && x < src->width; x++)
        {
            unsigned char *dp;
            long di = (long)y * dst->width + x;
            long si = (long)y * src->width + x;
            double sa = ((double)src->alpha[si] / 255.0) * opacity;
            double da = (double)dst->alpha[di] / 255.0;
            double oa, sr, sg, sb, dr, dg, db, br, bg, bb;
            double clip = 1.0;
            if (x < dst->clip_x0 || x >= dst->clip_x1 || y < dst->clip_y0 || y >= dst->clip_y1) continue;
            if (dst->clip_mask != NULL) clip *= (double)dst->clip_mask[y * dst->clip_mask_stride + x] / 255.0;
            if (dst->soft_mask != NULL) clip *= (double)dst->soft_mask[y * dst->soft_mask_stride + x] / 255.0;
            sa *= clip;
            if (sa <= 0.0) continue;
            dp = dst->pixels + (long)y * dst->stride + (long)x * 3;
            sr = (double)src->pixels[(long)y * src->stride + (long)x * 3] / 255.0;
            sg = (double)src->pixels[(long)y * src->stride + (long)x * 3 + 1] / 255.0;
            sb = (double)src->pixels[(long)y * src->stride + (long)x * 3 + 2] / 255.0;
            dr = (double)dp[0] / 255.0; dg = (double)dp[1] / 255.0; db = (double)dp[2] / 255.0;
            if (blend_mode >= PDF_BLEND_HUE && blend_mode <= PDF_BLEND_LUMINOSITY)
                blend_hsl(dr, dg, db, sr, sg, sb, blend_mode, &br, &bg, &bb);
            else
            {
                br = blend_channel(dr, sr, blend_mode);
                bg = blend_channel(dg, sg, blend_mode);
                bb = blend_channel(db, sb, blend_mode);
            }
            oa = sa + da * (1.0 - sa);
            if (oa <= 0.0) continue;
            dp[0] = clamp_u8((br * sa * da + sr * sa * (1.0 - da) + dr * da * (1.0 - sa)) / oa);
            dp[1] = clamp_u8((bg * sa * da + sg * sa * (1.0 - da) + dg * da * (1.0 - sa)) / oa);
            dp[2] = clamp_u8((bb * sa * da + sb * sa * (1.0 - da) + db * da * (1.0 - sa)) / oa);
            dst->alpha[di] = clamp_u8(oa);
        }
    }
}

void pdf_bitmap_fill_rect(pdf_bitmap *bmp, int x0, int y0, int x1, int y1, pdf_color c)
{
    int x, y, xs, xe, ys, ye;

    if (bmp == NULL) return;

    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }

    xs = x0 < 0 ? 0 : x0;
    ys = y0 < 0 ? 0 : y0;
    xe = x1 > bmp->width  ? bmp->width  : x1;
    ye = y1 > bmp->height ? bmp->height : y1;

    for (y = ys; y < ye; y++)
        for (x = xs; x < xe; x++)
            pdf_bitmap_set_pixel(bmp, x, y, c);
}

int pdf_bitmap_write_ppm(const pdf_bitmap *bmp, const char *path)
{
    FILE *fp;

    if (bmp == NULL || bmp->pixels == NULL || path == NULL)
        return PDF_ERR_BADARG;

    fp = fopen(path, "wb");
    if (fp == NULL)
        return PDF_ERR_IO;

    fprintf(fp, "P6\n%d %d\n255\n", bmp->width, bmp->height);
    fwrite(bmp->pixels, 1, (size_t)((long)bmp->stride * bmp->height), fp);
    fclose(fp);

    return PDF_OK;
}
