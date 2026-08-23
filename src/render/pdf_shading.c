/* pdf_shading.c
 *
 * Ver pdf_shading.h.
 */

#include "pdf_shading.h"
#include "pdf_object.h"
#include "pdf_parser.h"
#include <math.h>
#include <string.h>

static double array_num_at(const pdf_obj *a, int i, double def)
{
    const pdf_obj *o;
    if (a == NULL || a->type != PDF_ARRAY || a->u.arr.items == NULL ||
        i < 0 || i >= a->u.arr.count)
        return def;
    o = a->u.arr.items[i];
    if (o == NULL) return def;
    if (o->type == PDF_INT) return (double)o->u.integer;
    if (o->type == PDF_REAL) return o->u.real;
    return def;
}

static int array_bool_at(const pdf_obj *a, int i, int def)
{
    const pdf_obj *o;
    if (a == NULL || a->type != PDF_ARRAY || a->u.arr.items == NULL ||
        i < 0 || i >= a->u.arr.count)
        return def;
    o = a->u.arr.items[i];
    if (o == NULL || o->type != PDF_BOOL) return def;
    return o->u.boolean ? 1 : 0;
}

static pdf_obj *resolve(pdf_stream *st, const pdf_xref_table *xref,
                        pdf_arena *arena, pdf_obj *obj)
{
    if (obj != NULL && obj->type == PDF_REF)
        return pdf_xref_load_object(st, xref, obj->u.ref.num, arena);
    return obj;
}

int pdf_shading_load(pdf_stream *st, const pdf_xref_table *xref,
                     pdf_obj *sh_obj, pdf_arena *arena, pdf_shading *out)
{
    long type;
    pdf_obj *cs_obj, *coords, *domain, *extend, *fn_obj, *bg;

    if (out == NULL) return PDF_ERR_BADARG;
    memset(out, 0, sizeof(*out));
    out->kind = PDF_SHADING_UNSUPPORTED;
    out->domain[0] = 0.0; out->domain[1] = 1.0;
    pdf_colorspace_init(&out->cs);

    if (sh_obj == NULL || arena == NULL)
        return PDF_ERR_BADARG;
    sh_obj = resolve(st, xref, arena, sh_obj);
    if (sh_obj == NULL || (sh_obj->type != PDF_DICT && sh_obj->type != PDF_STREAM))
        return PDF_ERR_BADARG;

    type = pdf_dict_get_int(sh_obj, "ShadingType", -1);
    if (type != 2 && type != 3)
        return PDF_OK; /* mesh (4-7) / function-based (1): degradar, no soportado */

    cs_obj = resolve(st, xref, arena, pdf_dict_get(sh_obj, "ColorSpace"));
    if (cs_obj == NULL || !pdf_colorspace_from_obj(cs_obj, &out->cs))
        return PDF_OK; /* colorspace no reconocido: degradar, no adivinar */

    coords = resolve(st, xref, arena, pdf_dict_get(sh_obj, "Coords"));
    if (coords == NULL || coords->type != PDF_ARRAY)
        return PDF_OK;
    if (type == 2 && coords->u.arr.count < 4)
        return PDF_OK;
    if (type == 3 && coords->u.arr.count < 6)
        return PDF_OK;

    {
        int i, n = (type == 2) ? 4 : 6;
        for (i = 0; i < n; i++)
            out->coords[i] = array_num_at(coords, i, 0.0);
    }

    domain = resolve(st, xref, arena, pdf_dict_get(sh_obj, "Domain"));
    out->domain[0] = array_num_at(domain, 0, 0.0);
    out->domain[1] = array_num_at(domain, 1, 1.0);

    extend = resolve(st, xref, arena, pdf_dict_get(sh_obj, "Extend"));
    out->extend0 = array_bool_at(extend, 0, 0);
    out->extend1 = array_bool_at(extend, 1, 0);

    fn_obj = pdf_dict_get(sh_obj, "Function");
    if (fn_obj == NULL ||
        pdf_function_load(st, xref, fn_obj, arena, 0, &out->fn) != PDF_OK ||
        out->fn.kind == PDF_FUNCTION_UNSUPPORTED)
    {
        return PDF_OK; /* sin funcion evaluable: degradar (kind sigue UNSUPPORTED) */
    }

    bg = resolve(st, xref, arena, pdf_dict_get(sh_obj, "Background"));
    if (bg != NULL && bg->type == PDF_ARRAY)
    {
        double comps[8];
        int i, n = bg->u.arr.count;
        if (n > 8) n = 8;
        for (i = 0; i < n; i++) comps[i] = array_num_at(bg, i, 0.0);
        pdf_colorspace_convert(&out->cs, comps, n, &out->background);
        out->has_background = 1;
    }

    out->kind = (type == 2) ? PDF_SHADING_AXIAL : PDF_SHADING_RADIAL;
    return PDF_OK;
}

static int eval_axial_t(const pdf_shading *sh, double x, double y, double *t_out)
{
    double x0 = sh->coords[0], y0 = sh->coords[1];
    double x1 = sh->coords[2], y1 = sh->coords[3];
    double dx = x1 - x0, dy = y1 - y0;
    double denom = dx * dx + dy * dy;
    double s;

    if (denom < 1e-12)
    {
        s = 0.0; /* x0==x1 (degenerado): todo el plano tiene el mismo color */
    }
    else
    {
        s = ((x - x0) * dx + (y - y0) * dy) / denom;
    }

    if (s < 0.0)
    {
        if (!sh->extend0) return 0;
        s = 0.0;
    }
    else if (s > 1.0)
    {
        if (!sh->extend1) return 0;
        s = 1.0;
    }

    *t_out = sh->domain[0] + s * (sh->domain[1] - sh->domain[0]);
    return 1;
}

/* Algoritmo estandar de "cono de circulos" para shadings radiales (ver
 * ISO 32000-1 8.7.4.5.4): se busca el mayor 's' (extendido segun
 * Extend[]) tal que el punto (x,y) caiga sobre el circulo de centro
 * C(s)=(1-s)*C0+s*C1 y radio R(s)=(1-s)*r0+s*r1 con R(s)>=0. Se
 * resuelve la ecuacion cuadratica en 's' resultante de
 * |P-C(s)|^2 = R(s)^2 y se prueban las raices de mayor a menor. */
static int eval_radial_t(const pdf_shading *sh, double x, double y, double *t_out)
{
    double x0 = sh->coords[0], y0 = sh->coords[1], r0 = sh->coords[2];
    double x1 = sh->coords[3], y1 = sh->coords[4], r1 = sh->coords[5];
    double dx = x1 - x0, dy = y1 - y0, dr = r1 - r0;
    double px = x - x0, py = y - y0;
    double a = dx * dx + dy * dy - dr * dr;
    double b = -2.0 * (px * dx + py * dy + r0 * dr);
    double c = px * px + py * py - r0 * r0;
    double cand[2];
    int n_cand = 0;
    int i;

    if (fabs(a) < 1e-9)
    {
        if (fabs(b) > 1e-12)
        {
            cand[n_cand++] = -c / b;
        }
    }
    else
    {
        double disc = b * b - 4.0 * a * c;
        if (disc >= 0.0)
        {
            double sq = sqrt(disc);
            double s1 = (-b + sq) / (2.0 * a);
            double s2 = (-b - sq) / (2.0 * a);
            if (s1 > s2) { cand[0] = s1; cand[1] = s2; }
            else         { cand[0] = s2; cand[1] = s1; }
            n_cand = 2;
        }
    }

    for (i = 0; i < n_cand; i++)
    {
        double s = cand[i];
        double radius_at_s = r0 + s * dr;
        double s_clamped = s;

        if (radius_at_s < 0.0) continue;

        if (s_clamped < 0.0)
        {
            if (!sh->extend0) continue;
            s_clamped = 0.0;
        }
        else if (s_clamped > 1.0)
        {
            if (!sh->extend1) continue;
            s_clamped = 1.0;
        }

        /* con el 's' extendido/clampeado, el radio real en ese punto de
         * evaluacion debe seguir siendo >= 0 (un s clampeado puede
         * corresponder a un radio negativo si Extend fuerza mas alla
         * del cono real). */
        if (r0 + s_clamped * dr < 0.0) continue;

        *t_out = sh->domain[0] + s_clamped * (sh->domain[1] - sh->domain[0]);
        return 1;
    }

    return 0;
}

int pdf_shading_eval(const pdf_shading *sh, double x, double y, pdf_color *out)
{
    double t;
    double fn_out[PDF_FUNCTION_MAX_OUTPUTS];
    int n_out;

    if (sh == NULL || out == NULL || sh->kind == PDF_SHADING_UNSUPPORTED)
        return 0;

    if (sh->kind == PDF_SHADING_AXIAL)
    {
        if (!eval_axial_t(sh, x, y, &t))
            return sh->has_background ? (*out = sh->background, 1) : 0;
    }
    else
    {
        if (!eval_radial_t(sh, x, y, &t))
            return sh->has_background ? (*out = sh->background, 1) : 0;
    }

    if (pdf_function_eval(&sh->fn, &t, 1, fn_out, PDF_FUNCTION_MAX_OUTPUTS, &n_out) != PDF_OK)
        return 0;

    pdf_colorspace_convert(&sh->cs, fn_out, n_out, out);
    return 1;
}

void pdf_shading_paint_clip(pdf_bitmap *bmp, const pdf_shading *sh,
                            pdf_matrix device_to_shading)
{
    int x, y, x0, y0, x1, y1;

    if (bmp == NULL || sh == NULL || sh->kind == PDF_SHADING_UNSUPPORTED)
        return;

    x0 = bmp->clip_x0; y0 = bmp->clip_y0;
    x1 = bmp->clip_x1; y1 = bmp->clip_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bmp->width)  x1 = bmp->width;
    if (y1 > bmp->height) y1 = bmp->height;

    for (y = y0; y < y1; y++)
    {
        for (x = x0; x < x1; x++)
        {
            /* centro del pixel (x+0.5,y+0.5) en espacio de dispositivo,
             * transformado a espacio del shading via la matriz inversa
             * que ya calculo el llamador. */
            double dx = (double)x + 0.5, dy = (double)y + 0.5;
            double sx = dx * device_to_shading.a + dy * device_to_shading.c + device_to_shading.e;
            double sy = dx * device_to_shading.b + dy * device_to_shading.d + device_to_shading.f;
            pdf_color c;

            if (pdf_shading_eval(sh, sx, sy, &c))
                pdf_bitmap_set_pixel_coverage(bmp, x, y, c, 1.0);
        }
    }
}
