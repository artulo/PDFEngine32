/* pdf_function.h
 *
 * PDF Functions (ISO 32000-1 7.10): mapean N valores de entrada a M de
 * salida. Usadas por shadings (parametro t -> color) y tint transforms
 * de Separation/DeviceN (fuera de alcance por ahora).
 *
 * Soportado: Type 0 (sampled, solo 1 entrada -- suficiente para
 * shadings, que siempre llaman con m=1), Type 2 (exponential
 * interpolation), Type 3 (stitching). Type 4 (PostScript calculator)
 * se detecta pero no se evalua -- ver pdf_function_load.
 */

#ifndef PDF_FUNCTION_H
#define PDF_FUNCTION_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_xref.h"
#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_FUNCTION_MAX_INPUTS       2
#define PDF_FUNCTION_MAX_OUTPUTS      8
#define PDF_FUNCTION_MAX_SUBFUNCTIONS 8
#define PDF_FUNCTION_LOAD_MAX_DEPTH   4

typedef enum pdf_function_kind_e
{
    PDF_FUNCTION_SAMPLED     = 0,
    PDF_FUNCTION_EXPONENTIAL = 2,
    PDF_FUNCTION_STITCHING   = 3,
    PDF_FUNCTION_ARRAY       = 100, /* array de N funciones de 1 salida c/u, concatenadas */
    PDF_FUNCTION_UNSUPPORTED = -1   /* Type 4 u otro caso no soportado */
} pdf_function_kind;

typedef struct pdf_function_s pdf_function;

struct pdf_function_s
{
    pdf_function_kind kind;
    int n_in;   /* aridad real (clampeada a PDF_FUNCTION_MAX_INPUTS) */
    int n_out;  /* salidas reales (clampeada a PDF_FUNCTION_MAX_OUTPUTS) */
    double domain[PDF_FUNCTION_MAX_INPUTS * 2]; /* [lo0,hi0,lo1,hi1,...] */

    int has_range;
    double range[PDF_FUNCTION_MAX_OUTPUTS * 2];

    /* Type 2 (exponential): out[j] = C0[j] + x^N * (C1[j]-C0[j]) */
    double c0[PDF_FUNCTION_MAX_OUTPUTS];
    double c1[PDF_FUNCTION_MAX_OUTPUTS];
    double exp_n;

    /* Type 0 (sampled, 1-D solamente -- shadings solo usan m=1) */
    unsigned char *samples; /* bytes crudos decodificados, en 'arena' */
    long  n_samples_bytes;
    long  size0;            /* Size[0]: cantidad de muestras a lo largo del unico eje */
    int   bits_per_sample;
    double encode0, encode1; /* Encode[0],[1] -- default 0..Size0-1 */
    double sample_decode_lo[PDF_FUNCTION_MAX_OUTPUTS]; /* Decode por salida, default = Range */
    double sample_decode_hi[PDF_FUNCTION_MAX_OUTPUTS];

    /* Type 3 (stitching) y Type "array" (PDF_FUNCTION_ARRAY) comparten
     * el almacenamiento de sub-funciones -- para stitching se elige UNA
     * segun 'bounds'/'encode'; para array se evaluan TODAS y se
     * concatenan las salidas (cada una debe tener n_out==1). */
    pdf_function *subfns[PDF_FUNCTION_MAX_SUBFUNCTIONS];
    int n_sub;
    double bounds[PDF_FUNCTION_MAX_SUBFUNCTIONS - 1]; /* n_sub-1 valores, solo stitching */
    double sub_encode[PDF_FUNCTION_MAX_SUBFUNCTIONS * 2]; /* Encode por sub-funcion, solo stitching */
};

/* Carga una Function desde 'fn_obj' -- puede ser un dict/stream Function
 * unico, o un PDF_ARRAY de funciones de 1 salida cada una (patron comun
 * en tint transforms y a veces en /Function de shadings). Devuelve
 * PDF_OK si se pudo describir la funcion (incluso si su tipo real no es
 * evaluable -- ver 'kind==PDF_FUNCTION_UNSUPPORTED', pensado para que
 * el llamador distinga "no se pudo ni parsear" de "bien formada pero
 * este motor no la evalua"), o un codigo de error si 'fn_obj' esta
 * demasiado malformado para describirlo en absoluto. 'depth' es el
 * contador de recursion para Type 3 anidado -- pasar 0 desde afuera. */
int pdf_function_load(pdf_stream *st, const pdf_xref_table *xref,
                      pdf_obj *fn_obj, pdf_arena *arena, int depth,
                      pdf_function *out);

/* Evalua 'fn' en 'in' (n_in valores) y escribe hasta 'max_out' salidas
 * en 'out' (fija '*n_out' a la cantidad real escrita). Devuelve PDF_OK,
 * o PDF_ERR_UNSUPPORTED si fn->kind==PDF_FUNCTION_UNSUPPORTED -- el
 * llamador debe degradar con gracia (no pintar, o mantener el color
 * plano previo) en vez de inventar un valor. */
int pdf_function_eval(const pdf_function *fn, const double *in, int n_in,
                      double *out, int max_out, int *n_out);

#ifdef __cplusplus
}
#endif

#endif /* PDF_FUNCTION_H */
