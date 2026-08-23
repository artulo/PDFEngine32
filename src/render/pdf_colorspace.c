#include "pdf_colorspace.h"
#include <string.h>
#include <math.h>

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double obj_num(const pdf_obj *o, double def)
{
    if (o == NULL) return def;
    if (o->type == PDF_INT) return (double)o->u.integer;
    if (o->type == PDF_REAL) return o->u.real;
    return def;
}

static int array_num(const pdf_obj *a, int i, double *v)
{
    const pdf_obj *o;
    if (a == NULL || a->type != PDF_ARRAY || a->u.arr.items == NULL ||
        i < 0 || i >= a->u.arr.count || v == NULL)
        return 0;
    o = a->u.arr.items[i];
    if (o == NULL) return 0;
    if (o->type == PDF_INT) { *v = (double)o->u.integer; return 1; }
    if (o->type == PDF_REAL) { *v = o->u.real; return 1; }
    return 0;
}

static void array3(const pdf_obj *a, double *x, double *y, double *z,
                   double dx, double dy, double dz)
{
    double v;
    *x = dx; *y = dy; *z = dz;
    if (array_num(a, 0, &v)) *x = v;
    if (array_num(a, 1, &v)) *y = v;
    if (array_num(a, 2, &v)) *z = v;
}

static void array9(const pdf_obj *a, double *m)
{
    int i;
    double v;
    for (i = 0; i < 9; i++)
    {
        m[i] = 0.0;
        if (array_num(a, i, &v)) m[i] = v;
    }
}

void pdf_colorspace_init(pdf_colorspace *cs)
{
    int i;
    pdf_colorspace_kind k = PDF_CS_DEVICE_RGB;
    int comp = 3;
    if (cs == NULL) return;
    memcpy(&cs->kind, &k, sizeof(k));
    memcpy(&cs->components, &comp, sizeof(comp));
    cs->gamma = 1.0;
    cs->white_x = 0.95047;
    cs->white_y = 1.0;
    cs->white_z = 1.08883;
    cs->black_x = 0.0;
    cs->black_y = 0.0;
    cs->black_z = 0.0;
    for (i = 0; i < 9; i++) cs->matrix[i] = 0.0;
    cs->matrix[0] = 1.0;
    cs->matrix[4] = 1.0;
    cs->matrix[8] = 1.0;
    cs->lab_amin = -100.0;
    cs->lab_amax = 100.0;
    cs->lab_bmin = -100.0;
    cs->lab_bmax = 100.0;
}

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * documentada extensamente en pdf_object.c/pdf_xref.c/pdf_filter.c/
 * pdf_stream.c): escribir 'cs->kind' seguido de 'cs->components' en la
 * misma linea/statement consecutivo dispara la miscompilacion de
 * bcc32 7.70 -- confirmado end-to-end (pdf_colorspace_from_name(
 * "DeviceGray", ...) devolvia 0 pese a que el nombre llegaba
 * perfectamente intacto, rompiendo la carga de cualquier shading con
 * colorspace DeviceGray). Mismo fix: memcpy() por campo. */
static void set_kind_components(pdf_colorspace *cs, pdf_colorspace_kind kind, int components)
{
    memcpy(&cs->kind, &kind, sizeof(kind));
    memcpy(&cs->components, &components, sizeof(components));
}

int pdf_colorspace_from_name(const char *name, pdf_colorspace *cs)
{
    if (name == NULL || cs == NULL) return 0;
    pdf_colorspace_init(cs);
    if (strcmp(name, "DeviceGray") == 0 || strcmp(name, "G") == 0)
    {
        set_kind_components(cs, PDF_CS_DEVICE_GRAY, 1); return 1;
    }
    if (strcmp(name, "DeviceRGB") == 0 || strcmp(name, "RGB") == 0 ||
        strcmp(name, "DefaultRGB") == 0)
    {
        set_kind_components(cs, PDF_CS_DEVICE_RGB, 3); return 1;
    }
    if (strcmp(name, "DeviceCMYK") == 0 || strcmp(name, "CMYK") == 0)
    {
        set_kind_components(cs, PDF_CS_DEVICE_CMYK, 4); return 1;
    }
    if (strcmp(name, "Pattern") == 0)
    {
        set_kind_components(cs, PDF_CS_PATTERN, 0); return 1;
    }
    return 0;
}

int pdf_colorspace_from_obj(const pdf_obj *obj, pdf_colorspace *cs)
{
    const pdf_obj *base;
    const pdf_obj *n;
    const pdf_obj *wp;
    const pdf_obj *bp;
    const pdf_obj *g;
    const pdf_obj *m;
    const pdf_obj *range;
    const pdf_obj *arr;
    const char *name;

    if (obj == NULL || cs == NULL) return 0;

    if (obj->type == PDF_NAME)
        return pdf_colorspace_from_name(obj->u.name, cs);

    if (obj->type != PDF_ARRAY || obj->u.arr.count <= 0 ||
        obj->u.arr.items == NULL)
        return 0;

    arr = obj;
    if (arr->u.arr.items[0] == NULL ||
        arr->u.arr.items[0]->type != PDF_NAME)
        return 0;

    name = arr->u.arr.items[0]->u.name;

    if (strcmp(name, "CalGray") == 0)
    {
        pdf_colorspace_init(cs);
        set_kind_components(cs, PDF_CS_CAL_GRAY, 1);

        base = arr->u.arr.count > 1 ? arr->u.arr.items[1] : NULL;
        if (base != NULL && base->type == PDF_DICT)
        {
            wp = pdf_dict_get((pdf_obj *)base, "WhitePoint");
            bp = pdf_dict_get((pdf_obj *)base, "BlackPoint");
            g = pdf_dict_get((pdf_obj *)base, "Gamma");
            array3(wp, &cs->white_x, &cs->white_y, &cs->white_z,
                   0.95047, 1.0, 1.08883);
            array3(bp, &cs->black_x, &cs->black_y, &cs->black_z,
                   0.0, 0.0, 0.0);
            cs->gamma = obj_num(g, 1.0);
            if (cs->gamma <= 0.0) cs->gamma = 1.0;
        }
        return 1;
    }

    if (strcmp(name, "CalRGB") == 0)
    {
        pdf_colorspace_init(cs);
        set_kind_components(cs, PDF_CS_CAL_RGB, 3);

        base = arr->u.arr.count > 1 ? arr->u.arr.items[1] : NULL;
        if (base != NULL && base->type == PDF_DICT)
        {
            wp = pdf_dict_get((pdf_obj *)base, "WhitePoint");
            bp = pdf_dict_get((pdf_obj *)base, "BlackPoint");
            g = pdf_dict_get((pdf_obj *)base, "Gamma");
            m = pdf_dict_get((pdf_obj *)base, "Matrix");

            array3(wp, &cs->white_x, &cs->white_y, &cs->white_z,
                   0.95047, 1.0, 1.08883);
            array3(bp, &cs->black_x, &cs->black_y, &cs->black_z,
                   0.0, 0.0, 0.0);

            cs->gamma = 1.0;
            if (g != NULL && g->type == PDF_ARRAY)
                cs->gamma = obj_num(g->u.arr.items[0], 1.0);

            if (g != NULL && g->type == PDF_ARRAY)
            {
                /* CalRGB permits independent gamma values. Store the
                 * first value for legacy callers; conversion below
                 * reads the complete Gamma array through the object only
                 * when this function is extended. */
                if (cs->gamma <= 0.0) cs->gamma = 1.0;
            }

            if (m != NULL && m->type == PDF_ARRAY && m->u.arr.count >= 9)
                array9(m, cs->matrix);
        }
        return 1;
    }

    if (strcmp(name, "Lab") == 0)
    {
        pdf_colorspace_init(cs);
        set_kind_components(cs, PDF_CS_LAB, 3);

        base = arr->u.arr.count > 1 ? arr->u.arr.items[1] : NULL;
        if (base != NULL && base->type == PDF_DICT)
        {
            wp = pdf_dict_get((pdf_obj *)base, "WhitePoint");
            bp = pdf_dict_get((pdf_obj *)base, "BlackPoint");
            range = pdf_dict_get((pdf_obj *)base, "Range");

            array3(wp, &cs->white_x, &cs->white_y, &cs->white_z,
                   0.95047, 1.0, 1.08883);
            array3(bp, &cs->black_x, &cs->black_y, &cs->black_z,
                   0.0, 0.0, 0.0);

            if (range != NULL && range->type == PDF_ARRAY &&
                range->u.arr.count >= 4)
            {
                cs->lab_amin = obj_num(range->u.arr.items[0], -100.0);
                cs->lab_amax = obj_num(range->u.arr.items[1], 100.0);
                cs->lab_bmin = obj_num(range->u.arr.items[2], -100.0);
                cs->lab_bmax = obj_num(range->u.arr.items[3], 100.0);
            }
        }
        return 1;
    }

    if (strcmp(name, "ICCBased") == 0)
    {
        pdf_colorspace_init(cs);
        set_kind_components(cs, PDF_CS_ICC_BASED, 3);
        base = arr->u.arr.count > 1 ? arr->u.arr.items[1] : NULL;
        if (base != NULL && base->type == PDF_DICT)
        {
            n = pdf_dict_get((pdf_obj *)base, "N");
            if (n != NULL && (n->type == PDF_INT || n->type == PDF_REAL))
            {
                int nc = (int)obj_num(n, 3.0);
                if (nc >= 1 && nc <= 4) memcpy(&cs->components, &nc, sizeof(nc));
            }
            wp = pdf_dict_get((pdf_obj *)base, "WhitePoint");
            bp = pdf_dict_get((pdf_obj *)base, "BlackPoint");
            array3(wp, &cs->white_x, &cs->white_y, &cs->white_z,
                   0.95047, 1.0, 1.08883);
            array3(bp, &cs->black_x, &cs->black_y, &cs->black_z,
                   0.0, 0.0, 0.0);
        }
        return 1;
    }

    if (pdf_colorspace_from_name(name, cs))
        return 1;

    pdf_colorspace_init(cs);
    set_kind_components(cs, PDF_CS_UNKNOWN, 3);
    return 1;
}

static double lab_f_inv(double t)
{
    const double delta = 6.0 / 29.0;
    if (t > delta) return t * t * t;
    return 3.0 * delta * delta * (t - 4.0 / 29.0);
}

static void lab_to_xyz(const pdf_colorspace *cs, const double *v,
                       double *x, double *y, double *z)
{
    double L, A, B, fy, fx, fz;
    L = v[0] < 0.0 ? 0.0 : (v[0] > 100.0 ? 100.0 : v[0]);
    A = v[1] < cs->lab_amin ? cs->lab_amin :
        (v[1] > cs->lab_amax ? cs->lab_amax : v[1]);
    B = v[2] < cs->lab_bmin ? cs->lab_bmin :
        (v[2] > cs->lab_bmax ? cs->lab_bmax : v[2]);

    fy = (L + 16.0) / 116.0;
    fx = fy + A / 500.0;
    fz = fy - B / 200.0;

    *x = cs->white_x * lab_f_inv(fx);
    *y = cs->white_y * lab_f_inv(fy);
    *z = cs->white_z * lab_f_inv(fz);
}

static void xyz_to_srgb(double x, double y, double z, pdf_color *out)
{
    double r, g, b;
    double rr, gg, bb;

    /* Adapt XYZ from the PDF profile white point to D65 is performed
     * by the caller. These are the standard D65 XYZ -> linear sRGB
     * coefficients. */
    r =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    b =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    if (r <= 0.0031308) rr = 12.92 * r;
    else rr = 1.055 * pow(r, 1.0 / 2.4) - 0.055;
    if (g <= 0.0031308) gg = 12.92 * g;
    else gg = 1.055 * pow(g, 1.0 / 2.4) - 0.055;
    if (b <= 0.0031308) bb = 12.92 * b;
    else bb = 1.055 * pow(b, 1.0 / 2.4) - 0.055;

    out->r = clamp01(rr);
    out->g = clamp01(gg);
    out->b = clamp01(bb);
}

static void adapt_to_d65(double sx, double sy, double sz,
                         double wx, double wy, double wz,
                         double *x, double *y, double *z)
{
    double l, m, s;
    double sl, sm, ss;
    double dl, dm, ds;
    double d65x = 0.95047, d65y = 1.0, d65z = 1.08883;

    /* Bradford source transform. */
    l =  0.8951 * sx + 0.2664 * sy - 0.1614 * sz;
    m = -0.7502 * sx + 1.7135 * sy + 0.0367 * sz;
    s =  0.0389 * sx - 0.0685 * sy + 1.0296 * sz;

    /* Cone responses of the source and D65 reference whites. */
    sl =  0.8951 * wx + 0.2664 * wy - 0.1614 * wz;
    sm = -0.7502 * wx + 1.7135 * wy + 0.0367 * wz;
    ss =  0.0389 * wx - 0.0685 * wy + 1.0296 * wz;

    dl =  0.8951 * d65x + 0.2664 * d65y - 0.1614 * d65z;
    dm = -0.7502 * d65x + 1.7135 * d65y + 0.0367 * d65z;
    ds =  0.0389 * d65x - 0.0685 * d65y + 1.0296 * d65z;

    l *= dl / (sl == 0.0 ? 1.0 : sl);
    m *= dm / (sm == 0.0 ? 1.0 : sm);
    s *= ds / (ss == 0.0 ? 1.0 : ss);

    *x =  0.9869929 * l - 0.1470543 * m + 0.1599627 * s;
    *y =  0.4323053 * l + 0.5183603 * m + 0.0492912 * s;
    *z = -0.0085287 * l + 0.0400428 * m + 0.9684867 * s;
}

void pdf_colorspace_convert(const pdf_colorspace *cs, const double *v,
                            int n, pdf_color *out)
{
    double a, b, c, k;
    double x, y, z;
    double lin[3];
    double m0, m1, m2;
    double g0, g1, g2;

    if (out == NULL) return;
    out->r = out->g = out->b = 0.0;
    if (cs == NULL || v == NULL || n <= 0) return;

    switch (cs->kind)
    {
    case PDF_CS_DEVICE_GRAY:
        a = clamp01(v[0]);
        out->r = out->g = out->b = a;
        break;

    case PDF_CS_CAL_GRAY:
        a = clamp01(v[0]);
        if (cs->gamma > 0.0) a = pow(a, cs->gamma);
        x = cs->white_x * a;
        y = cs->white_y * a;
        z = cs->white_z * a;
        adapt_to_d65(x, y, z, cs->white_x, cs->white_y, cs->white_z, &x, &y, &z);
        xyz_to_srgb(x, y, z, out);
        break;

    case PDF_CS_DEVICE_CMYK:
        a = clamp01(v[0]);
        b = n > 1 ? clamp01(v[1]) : 0.0;
        c = n > 2 ? clamp01(v[2]) : 0.0;
        k = n > 3 ? clamp01(v[3]) : 0.0;
        out->r = 1.0 - (a + k > 1.0 ? 1.0 : a + k);
        out->g = 1.0 - (b + k > 1.0 ? 1.0 : b + k);
        out->b = 1.0 - (c + k > 1.0 ? 1.0 : c + k);
        break;

    case PDF_CS_CAL_RGB:
        a = clamp01(v[0]);
        b = n > 1 ? clamp01(v[1]) : a;
        c = n > 2 ? clamp01(v[2]) : b;
        g0 = cs->gamma > 0.0 ? cs->gamma : 1.0;
        g1 = g0;
        g2 = g0;
        lin[0] = pow(a, g0);
        lin[1] = pow(b, g1);
        lin[2] = pow(c, g2);
        m0 = cs->matrix[0] * lin[0] + cs->matrix[1] * lin[1] + cs->matrix[2] * lin[2];
        m1 = cs->matrix[3] * lin[0] + cs->matrix[4] * lin[1] + cs->matrix[5] * lin[2];
        m2 = cs->matrix[6] * lin[0] + cs->matrix[7] * lin[1] + cs->matrix[8] * lin[2];
        adapt_to_d65(m0, m1, m2, cs->white_x, cs->white_y, cs->white_z, &x, &y, &z);
        xyz_to_srgb(x, y, z, out);
        break;

    case PDF_CS_LAB:
        a = n > 0 ? v[0] : 0.0;
        b = n > 1 ? v[1] : 0.0;
        c = n > 2 ? v[2] : 0.0;
        {
            double lv[3];
            lv[0] = a; lv[1] = b; lv[2] = c;
            lab_to_xyz(cs, lv, &x, &y, &z);
        }
        adapt_to_d65(x, y, z, cs->white_x, cs->white_y, cs->white_z, &x, &y, &z);
        xyz_to_srgb(x, y, z, out);
        break;

    case PDF_CS_DEVICE_RGB:
    case PDF_CS_ICC_BASED:
    default:
        out->r = clamp01(v[0]);
        out->g = n > 1 ? clamp01(v[1]) : out->r;
        out->b = n > 2 ? clamp01(v[2]) : out->g;
        break;
    }
}
