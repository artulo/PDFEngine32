/* pdf_jbig2.c -- ver pdf_jbig2.h para el contexto/alcance/metodologia
 * de verificacion completos (DESIGN.md secciones 91-92). */

#include "pdf_jbig2.h"
#include "pdf_error.h"
#include <string.h>

/* ========================================================================
 * 1. Codificador aritmetico MQ -- IDENTICO al de pdf_jpx.c (JBIG2 Annex E
 *    y JPEG2000 Annex C son el MISMO algoritmo, norma publica). Se
 *    DUPLICA a proposito en vez de compartirlo con pdf_jpx.c: evita
 *    cualquier riesgo de tocar ese decoder, ya extensamente verificado
 *    contra archivos reales en producciones anteriores de este motor.
 * ======================================================================== */

typedef struct { unsigned short qe; unsigned char nmps, nlps, sw; } mq_state_row;

static const mq_state_row MQ_TABLE[47] = {
    {0x5601, 1, 1, 1}, {0x3401, 2, 6, 0}, {0x1801, 3, 9, 0}, {0x0AC1, 4, 12, 0},
    {0x0521, 5, 29, 0}, {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1}, {0x5401, 8, 14, 0},
    {0x4801, 9, 14, 0}, {0x3801, 10, 14, 0}, {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
    {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1}, {0x5401, 16, 14, 0},
    {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
    {0x1C01, 25, 22, 0}, {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0},
    {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0}, {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0},
    {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
    {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0},
    {0x0005, 45, 42, 0}, {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}
};

typedef struct
{
    const unsigned char *data;
    long len;
    long bp;
    unsigned int c;
    unsigned int a;
    int ct;
} mq_dec;

/* Contexto MQ: un par (indice de estado, bit MPS actual). Un decode de
 * region generica puede necesitar hasta 2^16 de estos (GBTEMPLATE 0,
 * 16 bits de contexto) -- se alocan en la arena, no en la pila. */
typedef struct { unsigned char i, mps; } mq_ctx;

static unsigned char mq_byte_at(const mq_dec *d, long i)
{
    return (i >= 0 && i < d->len) ? d->data[i] : 0xFF;
}

static void mq_bytein(mq_dec *d)
{
    if (mq_byte_at(d, d->bp) == 0xFF)
    {
        if (mq_byte_at(d, d->bp + 1) > 0x8F)
        {
            d->c += 0xFF00;
            d->ct = 8;
        }
        else
        {
            d->bp++;
            d->c += (unsigned int)mq_byte_at(d, d->bp) << 9;
            d->ct = 7;
        }
    }
    else
    {
        d->bp++;
        d->c += (unsigned int)mq_byte_at(d, d->bp) << 8;
        d->ct = 8;
    }
}

static void mq_init(mq_dec *d, const unsigned char *data, long len)
{
    d->data = data;
    d->len = len;
    d->bp = 0;
    d->c = (unsigned int)mq_byte_at(d, 0) << 16;
    mq_bytein(d);
    d->c <<= 7;
    d->ct -= 7;
    d->a = 0x8000;
}

static int mq_decode(mq_dec *d, mq_ctx *cx)
{
    unsigned int qe = MQ_TABLE[cx->i].qe;
    int dbit;

    d->a -= qe;

    if (((d->c >> 16) & 0xFFFF) < qe)
    {
        /* LPS_EXCHANGE */
        if (d->a < qe)
        {
            d->a = qe;
            dbit = cx->mps;
            cx->i = MQ_TABLE[cx->i].nmps;
        }
        else
        {
            d->a = qe;
            dbit = 1 - cx->mps;
            if (MQ_TABLE[cx->i].sw) cx->mps = (unsigned char)(1 - cx->mps);
            cx->i = MQ_TABLE[cx->i].nlps;
        }
    }
    else
    {
        d->c -= qe << 16;
        if ((d->a & 0x8000) != 0)
            return cx->mps; /* MPS directo, sin renormalizar */

        /* MPS_EXCHANGE */
        if (d->a < qe)
        {
            dbit = 1 - cx->mps;
            if (MQ_TABLE[cx->i].sw) cx->mps = (unsigned char)(1 - cx->mps);
            cx->i = MQ_TABLE[cx->i].nlps;
        }
        else
        {
            dbit = cx->mps;
            cx->i = MQ_TABLE[cx->i].nmps;
        }
    }

    /* RENORMD */
    do
    {
        if (d->ct == 0) mq_bytein(d);
        d->a <<= 1;
        d->c <<= 1;
        d->ct--;
    } while ((d->a & 0x8000) == 0);

    return dbit;
}

/* ========================================================================
 * 2. Plantillas de contexto para "region generica" (Annex 6.2 de T.88).
 *
 *    BUG REAL EVITADO (ver pdf_jbig2.h): el orden exacto en que los
 *    pixeles vecinos + los pixeles adaptativos (AT) se combinan en el
 *    entero de contexto es MUY especifico -- una fuente equivocada
 *    desincroniza el decoder MQ desde el primer pixel (el error no se
 *    ve como "un pixel mal", se ve como TODO basura). Las tablas de
 *    abajo se tomaron de pdf.js (jbig2.js, Mozilla, MPL 2.0 -- una
 *    implementacion publica ampliamente usada) y se verificaron con un
 *    prototipo en Python contra la imagen REAL decodificada por
 *    PyMuPDF/MuPDF (tests/PRINCIPLES_OF_MINERAL_PROCESSINGFuerstan.pdf,
 *    pagina 4, template 0, AT por defecto): 0 diferencias en los
 *    7 779 968 pixeles de la pagina completa, ANTES de escribir esta
 *    version en C. GBTEMPLATE 1/2/3 comparten la misma logica de
 *    armado, pero NO se probaron contra un archivo real todavia (no
 *    hizo falta para el caso que motivo esto) -- si alguna vez fallan,
 *    empezar por ahi. */

typedef struct { signed char x, y; } jbig2_pt;

static const jbig2_pt TPL0[12] = {
    {-1,-2},{0,-2},{1,-2}, {-2,-1},{-1,-1},{0,-1},{1,-1},{2,-1}, {-4,0},{-3,0},{-2,0},{-1,0}
};
static const jbig2_pt TPL1[12] = {
    {-1,-2},{0,-2},{1,-2},{2,-2}, {-2,-1},{-1,-1},{0,-1},{1,-1},{2,-1}, {-3,0},{-2,0},{-1,0}
};
static const jbig2_pt TPL2[9] = {
    {-1,-2},{0,-2},{1,-2}, {-2,-1},{-1,-1},{0,-1},{1,-1}, {-2,0},{-1,0}
};
static const jbig2_pt TPL3[9] = {
    {-3,-1},{-2,-1},{-1,-1},{0,-1},{1,-1}, {-4,0},{-3,0},{-2,0},{-1,0}
};

/* Contexto "pseudo-pixel" fijo para el bit SLTP de prediccion tipica
 * (TPGDON) -- uno por plantilla, valor fijo de la norma (Annex 6.2.5.7).
 * NO implementado/verificado contra un archivo real todavia (el que
 * motivo este decoder usa TPGDON=0) -- se deja implementado siguiendo
 * el mismo criterio que el resto (pdf.js), pero marcado como pendiente
 * de verificacion real si algun archivo futuro lo ejercita. */
static const unsigned int JBIG2_REUSED_CONTEXT[4] = { 0x9B25, 0x0795, 0x00E5, 0x0195 };

static int jbig2_pt_cmp(const jbig2_pt *a, const jbig2_pt *b)
{
    if (a->y != b->y) return (int)a->y - (int)b->y;
    return (int)a->x - (int)b->x;
}

/* Arma la plantilla final (fijos de GBTEMPLATE + AT), ordenada por
 * (y,x) ascendente -- MISMO criterio que decodeBitmap de pdf.js, del
 * que depende el bit-order del contexto (el primero en el orden
 * ordenado es el bit MAS significativo). Devuelve la cantidad total
 * de puntos (12+4=16 para template 0, 13 para 1/2, 10 para 3). */
static int jbig2_build_template(int gbtemplate, const jbig2_pt *at, int n_at, jbig2_pt *out)
{
    const jbig2_pt *fixed;
    int n_fixed, n_total, i, j;

    switch (gbtemplate)
    {
        case 0: fixed = TPL0; n_fixed = 12; break;
        case 1: fixed = TPL1; n_fixed = 12; break;
        case 2: fixed = TPL2; n_fixed = 9;  break;
        default: fixed = TPL3; n_fixed = 9; break;
    }

    n_total = 0;
    for (i = 0; i < n_fixed; i++) out[n_total++] = fixed[i];
    for (i = 0; i < n_at; i++) out[n_total++] = at[i];

    /* insertion sort -- a lo sumo 16 elementos, no hace falta nada mas
     * elaborado. */
    for (i = 1; i < n_total; i++)
    {
        jbig2_pt key = out[i];
        j = i - 1;
        while (j >= 0 && jbig2_pt_cmp(&out[j], &key) > 0)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    return n_total;
}

/* Decodifica UNA region generica (Annex 6.2) hacia 'bitmap' (1 byte
 * por pixel, 0/1, ya alocado por el llamador con 'w*h' bytes -- se
 * llena entero, incluyendo ceros). 'mq'/'contexts' ya deben estar
 * inicializados por el llamador (contexts: 1<<16 entradas, todas en
 * {i=0,mps=0} -- estado inicial fijo de la norma). */
static void jbig2_decode_generic_region(mq_dec *mq, mq_ctx *contexts,
                                          unsigned char *bitmap, int w, int h,
                                          int gbtemplate, int tpgdon,
                                          const jbig2_pt *at, int n_at)
{
    jbig2_pt tpl[16];
    int n_tpl = jbig2_build_template(gbtemplate, at, n_at, tpl);
    unsigned int pseudo_ctx = JBIG2_REUSED_CONTEXT[gbtemplate];
    int ltp = 0;
    int x, y, k;

    for (y = 0; y < h; y++)
    {
        unsigned char *row = bitmap + (long)y * w;

        if (tpgdon)
        {
            int sltp = mq_decode(mq, &contexts[pseudo_ctx]);
            ltp ^= sltp;
            if (ltp)
            {
                /* fila IDENTICA a la anterior -- copiarla y no gastar
                 * ningun bit del stream en ella (asi ahorra la norma
                 * para lineas repetidas, comunes en zonas en blanco). */
                if (y > 0) memcpy(row, row - w, (size_t)w);
                else memset(row, 0, (size_t)w);
                continue;
            }
        }

        for (x = 0; x < w; x++)
        {
            unsigned int ctxval = 0;
            for (k = 0; k < n_tpl; k++)
            {
                int xx = x + tpl[k].x, yy = y + tpl[k].y;
                unsigned int bit = 0;
                if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    bit = bitmap[(long)yy * w + xx];
                ctxval = (ctxval << 1) | bit;
            }
            row[x] = (unsigned char)mq_decode(mq, &contexts[ctxval]);
        }
    }
}

/* ========================================================================
 * 3. Parser de segmentos embebidos (Annex D de T.88 -- organizacion
 *    "embedded" que usa PDF: sin encabezado de archivo JBIG2, solo una
 *    secuencia de segmentos uno atras del otro).
 * ======================================================================== */

/* Devuelve 1 y deja 'data_off'/'data_len' listos si pudo leer un
 * encabezado de segmento valido a partir de '*pos' (avanzandolo), 0 si
 * el stream termino o el encabezado es invalido/trunco -- tolerante,
 * nunca lee fuera de 'len'. */
static int jbig2_read_seg_header(const unsigned char *data, long len, long *pos,
                                   unsigned long *seg_number, int *seg_type,
                                   long *data_off, long *data_len)
{
    long p = *pos;
    unsigned char flags, rtscaf;
    int count, refsize, i;
    unsigned long dummy_refs[256];

    if (p + 5 > len) return 0;
    *seg_number = ((unsigned long)data[p] << 24) | ((unsigned long)data[p+1] << 16) |
                  ((unsigned long)data[p+2] << 8) | (unsigned long)data[p+3];
    p += 4;
    flags = data[p]; p += 1;
    *seg_type = flags & 0x3F;

    if (p + 1 > len) return 0;
    rtscaf = data[p];
    count = rtscaf >> 5;
    if (count == 7)
    {
        unsigned long cnt4;
        long retain_bytes;
        if (p + 4 > len) return 0;
        cnt4 = (((unsigned long)data[p] << 24) | ((unsigned long)data[p+1] << 16) |
                ((unsigned long)data[p+2] << 8) | (unsigned long)data[p+3]) & 0x1FFFFFFFUL;
        p += 4;
        retain_bytes = (long)((cnt4 + 8) / 8);
        p += retain_bytes;
        count = (cnt4 > 255) ? 255 : (int)cnt4; /* limite defensivo de este parser */
    }
    else
    {
        p += 1;
    }

    if (*seg_number <= 256) refsize = 1;
    else if (*seg_number <= 65536) refsize = 2;
    else refsize = 4;

    for (i = 0; i < count && i < 256; i++)
    {
        if (p + refsize > len) return 0;
        if (refsize == 1) { dummy_refs[i] = data[p]; p += 1; }
        else if (refsize == 2) { dummy_refs[i] = ((unsigned long)data[p]<<8)|data[p+1]; p += 2; }
        else { dummy_refs[i] = ((unsigned long)data[p]<<24)|((unsigned long)data[p+1]<<16)|((unsigned long)data[p+2]<<8)|data[p+3]; p += 4; }
    }
    if (count > 256) p += (long)(count - 256) * refsize; /* saltar el resto sin guardarlos (no hace falta para esta implementacion) */

    if (flags & 0x40)
    {
        if (p + 4 > len) return 0;
        p += 4; /* asociacion de pagina de 4 bytes -- no se usa (una sola pagina por imagen de PDF) */
    }
    else
    {
        if (p + 1 > len) return 0;
        p += 1;
    }

    if (p + 4 > len) return 0;
    *data_len = (long)(((unsigned long)data[p] << 24) | ((unsigned long)data[p+1] << 16) |
                        ((unsigned long)data[p+2] << 8) | (unsigned long)data[p+3]);
    p += 4;

    if (*data_len < 0 || p + *data_len > len) return 0; /* trunco o "longitud desconocida" (0xFFFFFFFF) -- no soportado */

    *data_off = p;
    *pos = p + *data_len;
    return 1;
}

/* ========================================================================
 * 4. pdf_filter_jbig2 -- recorre 'globals' (si hay) y despues 'src',
 *    segmento a segmento, y arma el bitmap de pagina.
 * ======================================================================== */

typedef struct
{
    pdf_arena *arena;
    unsigned char *page_bits; /* empaquetado 1bpp, MSB primero, filas a byte -- salida final */
    int page_w, page_h;
    long row_bytes;
    int have_page_info;
} jbig2_ctx;

static void jbig2_set_pixel(jbig2_ctx *jc, int x, int y, int val)
{
    long idx;
    unsigned char mask;
    if (x < 0 || x >= jc->page_w || y < 0 || y >= jc->page_h) return;
    idx = (long)y * jc->row_bytes + (x >> 3);
    mask = (unsigned char)(0x80 >> (x & 7));
    if (val) jc->page_bits[idx] |= mask;
    else jc->page_bits[idx] &= (unsigned char)~mask;
}

static int jbig2_get_pixel(const jbig2_ctx *jc, int x, int y)
{
    long idx;
    unsigned char mask;
    if (x < 0 || x >= jc->page_w || y < 0 || y >= jc->page_h) return 0;
    idx = (long)y * jc->row_bytes + (x >> 3);
    mask = (unsigned char)(0x80 >> (x & 7));
    return (jc->page_bits[idx] & mask) ? 1 : 0;
}

/* Procesa los segmentos de un buffer ('globals' o 'src'). Devuelve
 * PDF_OK, o PDF_ERR_UNSUPPORTED apenas aparece un segmento fuera de
 * alcance (diccionario de simbolos/texto/halftone/refinamiento/tabla/
 * extension) -- tolerante en el sentido de que NO intenta adivinar,
 * corta limpio (el llamador de mas arriba, pdf_image.c, ya degrada sin
 * crashear cuando un filtro de imagen falla). */
static int jbig2_process_segments(jbig2_ctx *jc, const unsigned char *data, long len)
{
    long pos = 0;

    while (pos < len)
    {
        unsigned long seg_number;
        int seg_type;
        long data_off, data_len;

        if (!jbig2_read_seg_header(data, len, &pos, &seg_number, &seg_type, &data_off, &data_len))
            break; /* stream agotado o encabezado trunco -- fin tolerante */

        if (seg_type == 48) /* page information */
        {
            if (data_len >= 8)
            {
                int w = (int)(((unsigned long)data[data_off]<<24)|((unsigned long)data[data_off+1]<<16)|
                               ((unsigned long)data[data_off+2]<<8)|(unsigned long)data[data_off+3]);
                int h = (int)(((unsigned long)data[data_off+4]<<24)|((unsigned long)data[data_off+5]<<16)|
                               ((unsigned long)data[data_off+6]<<8)|(unsigned long)data[data_off+7]);
                /* alto "desconocido" (0xFFFFFFFF, paginas en streaming
                 * sin alto declarado de antemano) -- no deberia pasar
                 * en un PDF (siempre trae /Height), se ignora y se
                 * confia en el que ya se reservo desde el dict de
                 * imagen. */
                if (w > 0 && w == jc->page_w && h > 0 && h <= jc->page_h)
                    jc->have_page_info = 1;
                else
                    jc->have_page_info = 1; /* discrepancia menor: seguir igual, confiar en el buffer ya reservado */
            }
        }
        else if (seg_type == 36 || seg_type == 38 || seg_type == 39) /* generic region (intermediate/immediate/immediate-lossless) */
        {
            int rw, rh, rx, ry, comb_op;
            unsigned char flags;
            int mmr, gbtemplate, tpgdon;
            jbig2_pt at[4];
            int n_at, i;
            long p;
            unsigned char *region_bmp;
            mq_dec mq;
            mq_ctx *contexts;

            if (data_len < 18) return PDF_ERR_UNSUPPORTED;
            p = data_off;
            rw = (int)(((unsigned long)data[p]<<24)|((unsigned long)data[p+1]<<16)|((unsigned long)data[p+2]<<8)|data[p+3]);
            rh = (int)(((unsigned long)data[p+4]<<24)|((unsigned long)data[p+5]<<16)|((unsigned long)data[p+6]<<8)|data[p+7]);
            rx = (int)(((unsigned long)data[p+8]<<24)|((unsigned long)data[p+9]<<16)|((unsigned long)data[p+10]<<8)|data[p+11]);
            ry = (int)(((unsigned long)data[p+12]<<24)|((unsigned long)data[p+13]<<16)|((unsigned long)data[p+14]<<8)|data[p+15]);
            comb_op = data[p+16] & 0x07;
            flags = data[p+17];
            mmr = flags & 1;
            gbtemplate = (flags >> 1) & 3;
            tpgdon = (flags >> 3) & 1;
            p += 18;

            if (mmr) return PDF_ERR_UNSUPPORTED; /* MMR (estilo CCITT) para region generica: no implementado */
            if (rw <= 0 || rh <= 0 || rw > 20000 || rh > 20000) return PDF_ERR_BADARG;

            n_at = (gbtemplate == 0) ? 4 : 1;
            if (p + n_at * 2 > data_off + data_len) return PDF_ERR_UNSUPPORTED;
            for (i = 0; i < n_at; i++)
            {
                at[i].x = (signed char)data[p]; p += 1;
                at[i].y = (signed char)data[p]; p += 1;
            }

            region_bmp = (unsigned char *)pdf_arena_alloc(jc->arena, (size_t)rw * rh);
            if (region_bmp == NULL) return PDF_ERR_NOMEM;
            memset(region_bmp, 0, (size_t)rw * rh);

            contexts = (mq_ctx *)pdf_arena_alloc(jc->arena, (1UL << 16) * sizeof(mq_ctx));
            if (contexts == NULL) return PDF_ERR_NOMEM;
            memset(contexts, 0, (1UL << 16) * sizeof(mq_ctx)); /* i=0,mps=0 -- estado inicial fijo de la norma */

            mq_init(&mq, data + p, (data_off + data_len) - p);
            jbig2_decode_generic_region(&mq, contexts, region_bmp, rw, rh, gbtemplate, tpgdon, at, n_at);

            /* componer en la pagina segun el operador externo (Annex
             * 7.4.8.1: 0=OR,1=AND,2=XOR,3=XNOR,4=REPLACE). El caso real
             * que motivo esto usa XOR contra una pagina en blanco
             * (0 XOR x == x), pero se implementan los 5 para no
             * romper con otros archivos reales. */
            {
                int xx, yy;
                for (yy = 0; yy < rh; yy++)
                {
                    for (xx = 0; xx < rw; xx++)
                    {
                        int src_bit = region_bmp[(long)yy * rw + xx];
                        int dst_bit = jbig2_get_pixel(jc, rx + xx, ry + yy);
                        int out_bit;
                        switch (comb_op)
                        {
                            case 0: out_bit = dst_bit | src_bit; break;
                            case 1: out_bit = dst_bit & src_bit; break;
                            case 2: out_bit = dst_bit ^ src_bit; break;
                            case 3: out_bit = 1 - (dst_bit ^ src_bit); break;
                            default: out_bit = src_bit; break; /* REPLACE */
                        }
                        jbig2_set_pixel(jc, rx + xx, ry + yy, out_bit);
                    }
                }
            }
        }
        else if (seg_type == 49 || seg_type == 50 || seg_type == 51)
        {
            /* end of page / end of stripe / end of file: nada que hacer */
        }
        else
        {
            /* diccionario de simbolos (0), regiones de texto (4/6/7),
             * diccionario de patrones (16), halftone (20/22/23),
             * refinamiento (40/42/43), tablas (53), extension (62):
             * fuera de alcance esta ronda, ver pdf_jbig2.h. */
            return PDF_ERR_UNSUPPORTED;
        }
    }

    return PDF_OK;
}

int pdf_filter_jbig2(pdf_arena *arena,
                      const unsigned char *src, long src_len,
                      const unsigned char *globals, long globals_len,
                      int width, int height, pdf_buf *out)
{
    jbig2_ctx jc;
    int rc;

    if (arena == NULL || src == NULL || out == NULL || width <= 0 || height <= 0)
        return PDF_ERR_BADARG;
    if (width > 20000 || height > 20000)
        return PDF_ERR_BADARG; /* limite defensivo, mismo criterio que pdf_image.c */

    jc.arena = arena;
    jc.page_w = width;
    jc.page_h = height;
    jc.row_bytes = ((long)width + 7) / 8;
    jc.have_page_info = 0;

    jc.page_bits = (unsigned char *)pdf_arena_alloc(arena, (size_t)jc.row_bytes * height);
    if (jc.page_bits == NULL) return PDF_ERR_NOMEM;
    /* pixel por defecto de pagina: 0 (blanco/fondo) -- el segmento de
     * informacion de pagina puede pedir 1 como default (bit de flags),
     * pero no se vio ningun archivo real que lo necesite todavia; se
     * deja documentado como limite de alcance en vez de adivinar. */
    memset(jc.page_bits, 0, (size_t)jc.row_bytes * height);

    if (globals != NULL && globals_len > 0)
    {
        rc = jbig2_process_segments(&jc, globals, globals_len);
        if (rc != PDF_OK) return rc;
    }

    rc = jbig2_process_segments(&jc, src, src_len);
    if (rc != PDF_OK) return rc;

    /* JBIG2 fija su propia convencion (bit=1 SIEMPRE significa
     * "primer plano/negro", norma T.88) -- pero pdf_image.c espera el
     * mismo convenio que ya usa para CCITT/crudo (ver comentario
     * grande junto a 'black_is_1' en pdf_filter_ccitt_g4, src/filters/
     * pdf_filter.c): bit=1 -> blanco/255, bit=0 -> negro/0 (el Decode
     * array default [0 1] de DeviceGray). Se invierte ACA, antes de
     * entregar, para que el desempaquetado generico de pdf_image.c
     * (bpc==1) interprete los bits sin ningun caso especial -- misma
     * idea que 'black_is_1' de CCITT, pero JBIG2 no tiene una bandera
     * equivalente en el PDF: su convencion es siempre fija. */
    {
        long i, total_bytes = jc.row_bytes * height;
        for (i = 0; i < total_bytes; i++)
            jc.page_bits[i] = (unsigned char)~jc.page_bits[i];
    }

    out->data = jc.page_bits;
    out->len = jc.row_bytes * height;
    return PDF_OK;
}
