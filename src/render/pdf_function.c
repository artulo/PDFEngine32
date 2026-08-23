/* pdf_function.c
 *
 * Ver pdf_function.h.
 */

#include "pdf_function.h"
#include "pdf_parser.h"
#include "pdf_filter.h"
#include <string.h>
#include <math.h>

static pdf_obj *resolve_ref(pdf_stream *st, const pdf_xref_table *xref,
                            pdf_arena *arena, pdf_obj *obj)
{
    if (obj != NULL && obj->type == PDF_REF)
        return pdf_xref_load_object(st, xref, obj->u.ref.num, arena);
    return obj;
}

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

static double clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- carga --------------------------------------------------------------- */

static void load_domain_range(pdf_function *out, pdf_obj *dict)
{
    pdf_obj *domain = pdf_dict_get(dict, "Domain");
    pdf_obj *range  = pdf_dict_get(dict, "Range");
    int i;

    out->n_in = 1;
    out->domain[0] = 0.0; out->domain[1] = 1.0;
    if (domain != NULL && domain->type == PDF_ARRAY && domain->u.arr.count >= 2)
    {
        out->n_in = domain->u.arr.count / 2;
        if (out->n_in > PDF_FUNCTION_MAX_INPUTS) out->n_in = PDF_FUNCTION_MAX_INPUTS;
        for (i = 0; i < out->n_in; i++)
        {
            out->domain[i * 2]     = array_num_at(domain, i * 2, 0.0);
            out->domain[i * 2 + 1] = array_num_at(domain, i * 2 + 1, 1.0);
        }
    }

    out->has_range = 0;
    out->n_out = 0;
    if (range != NULL && range->type == PDF_ARRAY && range->u.arr.count >= 2)
    {
        int n = range->u.arr.count / 2;
        if (n > PDF_FUNCTION_MAX_OUTPUTS) n = PDF_FUNCTION_MAX_OUTPUTS;
        out->has_range = 1;
        out->n_out = n;
        for (i = 0; i < n; i++)
        {
            out->range[i * 2]     = array_num_at(range, i * 2, 0.0);
            out->range[i * 2 + 1] = array_num_at(range, i * 2 + 1, 1.0);
        }
    }
}

static int load_exponential(pdf_function *out, pdf_obj *dict)
{
    pdf_obj *c0 = pdf_dict_get(dict, "C0");
    pdf_obj *c1 = pdf_dict_get(dict, "C1");
    pdf_obj *n  = pdf_dict_get(dict, "N");
    int i, n_out;

    n_out = 1;
    if (c0 != NULL && c0->type == PDF_ARRAY && c0->u.arr.count > n_out) n_out = c0->u.arr.count;
    if (c1 != NULL && c1->type == PDF_ARRAY && c1->u.arr.count > n_out) n_out = c1->u.arr.count;
    if (n_out > PDF_FUNCTION_MAX_OUTPUTS) n_out = PDF_FUNCTION_MAX_OUTPUTS;
    if (out->n_out == 0) out->n_out = n_out;

    /* Defaults del estandar: C0=[0.0], C1=[1.0] si las claves faltan. */
    for (i = 0; i < out->n_out; i++)
    {
        double c0_default = 0.0;
        double c1_default = (i == 0) ? 1.0 : 0.0;
        out->c0[i] = array_num_at(c0, i, c0_default);
        out->c1[i] = array_num_at(c1, i, c1_default);
    }
    out->exp_n = pdf_obj_num(n, 1.0);
    return PDF_OK;
}

static int load_sampled(pdf_function *out, pdf_obj *dict, pdf_stream *st,
                        const pdf_xref_table *xref, pdf_arena *arena)
{
    pdf_obj *size   = pdf_dict_get(dict, "Size");
    pdf_obj *bps    = pdf_dict_get(dict, "BitsPerSample");
    pdf_obj *encode = pdf_dict_get(dict, "Encode");
    pdf_obj *decode = pdf_dict_get(dict, "Decode");
    unsigned char *raw;
    long raw_len;
    int i;

    if (dict->type != PDF_STREAM || dict->u.stm.raw_length <= 0 ||
        dict->u.stm.raw_length > 50L * 1024L * 1024L)
        return PDF_ERR_BADARG;

    /* Alcance deliberado: solo funciones de 1 entrada (m=1) -- es lo
     * unico que necesitan shadings (el parametro t). Un Type 0 real de
     * mas dimensiones cae a UNSUPPORTED (degradar, no crashear). */
    if (out->n_in != 1)
        return PDF_ERR_UNSUPPORTED;

    out->size0 = (long)array_num_at(size, 0, 2.0);
    if (out->size0 < 2) out->size0 = 2;

    out->bits_per_sample = (int)pdf_obj_num(bps, 8.0);
    if (out->bits_per_sample != 1 && out->bits_per_sample != 2 &&
        out->bits_per_sample != 4 && out->bits_per_sample != 8 &&
        out->bits_per_sample != 16 && out->bits_per_sample != 24 &&
        out->bits_per_sample != 32)
        return PDF_ERR_UNSUPPORTED;

    out->encode0 = array_num_at(encode, 0, 0.0);
    out->encode1 = array_num_at(encode, 1, (double)(out->size0 - 1));

    for (i = 0; i < out->n_out; i++)
    {
        double lo = out->has_range ? out->range[i * 2]     : 0.0;
        double hi = out->has_range ? out->range[i * 2 + 1] : 1.0;
        out->sample_decode_lo[i] = array_num_at(decode, i * 2, lo);
        out->sample_decode_hi[i] = array_num_at(decode, i * 2 + 1, hi);
    }

    /* Descompresion del stream de muestras -- mismo patron que
     * src/image/pdf_image.c (offset+length crudos, desencriptar si
     * corresponde, FlateDecode si /Filter lo pide). */
    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)dict->u.stm.raw_length);
    if (raw == NULL) return PDF_ERR_NOMEM;
    pdf_stream_seek(st, dict->u.stm.raw_offset);
    raw_len = pdf_stream_read(st, raw, dict->u.stm.raw_length);
    if (xref != NULL && xref->crypt.active)
        raw_len = pdf_crypt_decrypt(&xref->crypt, dict->u.stm.obj_num, dict->u.stm.obj_gen, raw, raw_len);

    {
        const char *filter = pdf_dict_get_name(dict, "Filter");
        if (filter != NULL && strcmp(filter, "FlateDecode") == 0)
        {
            pdf_buf dec;
            if (pdf_filter_flate(arena, raw, raw_len, 0, &dec) != PDF_OK)
                return PDF_ERR_UNSUPPORTED;
            out->samples = dec.data;
            out->n_samples_bytes = dec.len;
        }
        else if (filter == NULL)
        {
            out->samples = raw;
            out->n_samples_bytes = raw_len;
        }
        else
        {
            return PDF_ERR_UNSUPPORTED; /* otro filtro: fuera de alcance */
        }
    }

    return PDF_OK;
}

static int load_stitching(pdf_function *out, pdf_obj *dict, pdf_stream *st,
                          const pdf_xref_table *xref, pdf_arena *arena, int depth)
{
    pdf_obj *functions = pdf_dict_get(dict, "Functions");
    pdf_obj *bounds    = pdf_dict_get(dict, "Bounds");
    pdf_obj *encode    = pdf_dict_get(dict, "Encode");
    int i, n;

    if (functions == NULL || functions->type != PDF_ARRAY)
        return PDF_ERR_BADARG;

    n = functions->u.arr.count;
    if (n > PDF_FUNCTION_MAX_SUBFUNCTIONS) n = PDF_FUNCTION_MAX_SUBFUNCTIONS;
    out->n_sub = n;

    for (i = 0; i < n; i++)
    {
        pdf_obj *sub_obj = resolve_ref(st, xref, arena, functions->u.arr.items[i]);
        pdf_function *sub = (pdf_function *)pdf_arena_alloc(arena, sizeof(pdf_function));
        if (sub == NULL) return PDF_ERR_NOMEM;
        if (pdf_function_load(st, xref, sub_obj, arena, depth + 1, sub) != PDF_OK)
            sub->kind = PDF_FUNCTION_UNSUPPORTED;
        out->subfns[i] = sub;

        out->sub_encode[i * 2]     = array_num_at(encode, i * 2, 0.0);
        out->sub_encode[i * 2 + 1] = array_num_at(encode, i * 2 + 1, 1.0);
    }
    for (i = 0; i < n - 1; i++)
        out->bounds[i] = array_num_at(bounds, i, out->domain[1]);

    return PDF_OK;
}

int pdf_function_load(pdf_stream *st, const pdf_xref_table *xref,
                      pdf_obj *fn_obj, pdf_arena *arena, int depth,
                      pdf_function *out)
{
    long type;

    if (out == NULL) return PDF_ERR_BADARG;
    memset(out, 0, sizeof(*out));
    out->kind = PDF_FUNCTION_UNSUPPORTED;
    out->n_in = 1;
    out->domain[0] = 0.0; out->domain[1] = 1.0;

    if (fn_obj == NULL || arena == NULL)
        return PDF_ERR_BADARG;

    fn_obj = resolve_ref(st, xref, arena, fn_obj);
    if (fn_obj == NULL)
        return PDF_ERR_BADARG;

    /* Array de funciones de 1 salida c/u (comun en tint transforms, y
     * a veces en /Function de un shading en vez de una unica funcion
     * multi-salida). */
    if (fn_obj->type == PDF_ARRAY)
    {
        int i, n = fn_obj->u.arr.count;
        if (n > PDF_FUNCTION_MAX_SUBFUNCTIONS) n = PDF_FUNCTION_MAX_SUBFUNCTIONS;
        out->kind = PDF_FUNCTION_ARRAY;
        out->n_sub = n;
        out->n_out = n;
        for (i = 0; i < n; i++)
        {
            pdf_obj *sub_obj = resolve_ref(st, xref, arena, fn_obj->u.arr.items[i]);
            pdf_function *sub = (pdf_function *)pdf_arena_alloc(arena, sizeof(pdf_function));
            if (sub == NULL) return PDF_ERR_NOMEM;
            if (pdf_function_load(st, xref, sub_obj, arena, depth + 1, sub) != PDF_OK)
                sub->kind = PDF_FUNCTION_UNSUPPORTED;
            out->subfns[i] = sub;
        }
        if (n > 0)
        {
            out->n_in = out->subfns[0]->n_in;
            memcpy(out->domain, out->subfns[0]->domain, sizeof(out->domain));
        }
        return PDF_OK;
    }

    if (fn_obj->type != PDF_DICT && fn_obj->type != PDF_STREAM)
        return PDF_ERR_BADARG;

    type = pdf_dict_get_int(fn_obj, "FunctionType", -1);
    load_domain_range(out, fn_obj);

    if (depth > PDF_FUNCTION_LOAD_MAX_DEPTH)
    {
        out->kind = PDF_FUNCTION_UNSUPPORTED;
        return PDF_OK; /* degradar, no fallar la carga entera */
    }

    if (type == 0)
    {
        out->kind = PDF_FUNCTION_SAMPLED;
        if (load_sampled(out, fn_obj, st, xref, arena) != PDF_OK)
            out->kind = PDF_FUNCTION_UNSUPPORTED;
    }
    else if (type == 2)
    {
        out->kind = PDF_FUNCTION_EXPONENTIAL;
        load_exponential(out, fn_obj);
    }
    else if (type == 3)
    {
        out->kind = PDF_FUNCTION_STITCHING;
        if (load_stitching(out, fn_obj, st, xref, arena, depth) != PDF_OK)
            out->kind = PDF_FUNCTION_UNSUPPORTED;
    }
    else
    {
        /* Type 4 (PostScript calculator) u otro: se describe pero no
         * se evalua -- ver comentario en pdf_function.h. */
        out->kind = PDF_FUNCTION_UNSUPPORTED;
    }

    return PDF_OK;
}

/* --- evaluacion ------------------------------------------------------------ */

static long read_sample_raw(const unsigned char *samples, long n_bytes,
                            int bits_per_sample, long sample_index, int out_index, int n_out)
{
    long bit_offset = (sample_index * (long)n_out + out_index) * (long)bits_per_sample;
    long byte_offset = bit_offset / 8;
    long v = 0;
    int bits_left = bits_per_sample;
    int shift = (int)(bit_offset % 8);

    while (bits_left > 0 && byte_offset < n_bytes)
    {
        int avail = 8 - shift;
        int take = (bits_left < avail) ? bits_left : avail;
        int byte = samples[byte_offset];
        int mask = ((1 << take) - 1) << (avail - take);
        int bits = (byte & mask) >> (avail - take);

        v = (v << take) | bits;
        bits_left -= take;
        shift = 0;
        byte_offset++;
    }
    return v;
}

static int eval_sampled(const pdf_function *fn, double x, double *out, int max_out, int *n_out)
{
    double max_sample;
    double e;
    long s0, s1;
    double frac;
    int j, no;

    x = clampd(x, fn->domain[0], fn->domain[1]);
    e = (fn->domain[1] > fn->domain[0])
        ? fn->encode0 + (x - fn->domain[0]) * (fn->encode1 - fn->encode0) / (fn->domain[1] - fn->domain[0])
        : fn->encode0;
    e = clampd(e, 0.0, (double)(fn->size0 - 1));

    s0 = (long)floor(e);
    s1 = (s0 + 1 < fn->size0) ? s0 + 1 : s0;
    frac = e - (double)s0;

    max_sample = pow(2.0, (double)fn->bits_per_sample) - 1.0;
    no = fn->n_out;
    if (no > max_out) no = max_out;
    if (no > PDF_FUNCTION_MAX_OUTPUTS) no = PDF_FUNCTION_MAX_OUTPUTS;

    for (j = 0; j < no; j++)
    {
        long raw0 = read_sample_raw(fn->samples, fn->n_samples_bytes, fn->bits_per_sample, s0, j, fn->n_out);
        long raw1 = read_sample_raw(fn->samples, fn->n_samples_bytes, fn->bits_per_sample, s1, j, fn->n_out);
        double v0 = (double)raw0 / max_sample;
        double v1 = (double)raw1 / max_sample;
        double v = v0 + frac * (v1 - v0);
        double lo = fn->sample_decode_lo[j];
        double hi = fn->sample_decode_hi[j];
        out[j] = lo + v * (hi - lo);
    }
    *n_out = no;
    return PDF_OK;
}

static int eval_exponential(const pdf_function *fn, double x, double *out, int max_out, int *n_out)
{
    int j, no;
    double xn;

    x = clampd(x, fn->domain[0], fn->domain[1]);
    xn = (fn->exp_n == 1.0) ? x : pow(x, fn->exp_n);

    no = fn->n_out;
    if (no > max_out) no = max_out;
    if (no > PDF_FUNCTION_MAX_OUTPUTS) no = PDF_FUNCTION_MAX_OUTPUTS;

    for (j = 0; j < no; j++)
        out[j] = fn->c0[j] + xn * (fn->c1[j] - fn->c0[j]);
    *n_out = no;
    return PDF_OK;
}

static int eval_stitching(const pdf_function *fn, double x, double *out, int max_out, int *n_out)
{
    int k;
    double lo, hi, ex;

    x = clampd(x, fn->domain[0], fn->domain[1]);
    if (fn->n_sub <= 0) { *n_out = 0; return PDF_ERR_UNSUPPORTED; }

    for (k = 0; k < fn->n_sub - 1; k++)
    {
        if (x < fn->bounds[k]) break;
    }
    /* k es el indice de la sub-funcion elegida: el ultimo bucle deja k
     * en n_sub-1 si x no cayo antes de ningun limite. */

    lo = (k == 0) ? fn->domain[0] : fn->bounds[k - 1];
    hi = (k == fn->n_sub - 1) ? fn->domain[1] : fn->bounds[k];
    ex = (hi > lo)
        ? fn->sub_encode[k * 2] + (x - lo) * (fn->sub_encode[k * 2 + 1] - fn->sub_encode[k * 2]) / (hi - lo)
        : fn->sub_encode[k * 2];

    return pdf_function_eval(fn->subfns[k], &ex, 1, out, max_out, n_out);
}

int pdf_function_eval(const pdf_function *fn, const double *in, int n_in,
                      double *out, int max_out, int *n_out)
{
    if (fn == NULL || in == NULL || out == NULL || n_out == NULL)
        return PDF_ERR_BADARG;

    *n_out = 0;

    if (fn->kind == PDF_FUNCTION_ARRAY)
    {
        int i, total = 0;
        for (i = 0; i < fn->n_sub && total < max_out; i++)
        {
            double sub_out[PDF_FUNCTION_MAX_OUTPUTS];
            int sub_n = 0;
            if (pdf_function_eval(fn->subfns[i], in, n_in, sub_out, PDF_FUNCTION_MAX_OUTPUTS, &sub_n) != PDF_OK)
                return PDF_ERR_UNSUPPORTED;
            if (sub_n > 0 && total < max_out)
                out[total++] = sub_out[0];
        }
        *n_out = total;
        return PDF_OK;
    }

    if (n_in < 1) return PDF_ERR_BADARG;

    switch (fn->kind)
    {
        case PDF_FUNCTION_SAMPLED:     return eval_sampled(fn, in[0], out, max_out, n_out);
        case PDF_FUNCTION_EXPONENTIAL: return eval_exponential(fn, in[0], out, max_out, n_out);
        case PDF_FUNCTION_STITCHING:   return eval_stitching(fn, in[0], out, max_out, n_out);
        default: return PDF_ERR_UNSUPPORTED;
    }
}
