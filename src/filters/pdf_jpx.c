/* pdf_jpx.c
 *
 * Ver pdf_jpx.h y DESIGN.md seccion 60 para el contexto completo.
 *
 * Implementacion minima de JPEG2000 (T.800) enfocada en el caso REAL
 * mas comun en PDFs (confirmado inspeccionando codestreams reales a
 * mano, no adivinado): contenedor JP2 con boxes, tiles, 9/7
 * irreversible O 5/3 reversible, MCT, UNA capa de calidad,
 * code-blocks sin estilos especiales.
 *
 * Estructura del archivo (de mas fundamental a mas alto nivel):
 *   1. Decodificador aritmetico MQ (T.800 Annex C) -- el motor de
 *      entropia que usan TANTO los arboles de tags (Tier-2, para los
 *      headers de paquete) COMO la codificacion por planos de bits de
 *      cada code-block (Tier-1).
 *   2. Arbol de tags (T.800 Annex B.10) -- estructura que codifica de
 *      forma compacta, por precinto, que code-blocks estan incluidos
 *      en cada paquete y cuantos planos de bits "cero" tiene cada uno.
 *   3. EBCOT Tier-1: decodificacion por planos de bits de un
 *      code-block (las 3 pasadas -- significance propagation,
 *      magnitude refinement, cleanup -- con sus modelos de contexto).
 *   4. Tier-2: parseo de paquetes (headers + datos) dentro de una
 *      parte de tile.
 *   5. Transformada wavelet inversa (5/3 reversible y 9/7
 *      irreversible, ambas por lifting, separable 2D multi-nivel).
 *   6. Driver principal: boxes JP2, marcadores J2K, armado de tiles,
 *      MCT inversa, salida RGB final.
 */

#include "pdf_jpx.h"
#include "pdf_error.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>   /* PDF_JPX_TIMING -- diagnostico TEMPORAL de rendimiento, ver DESIGN.md */

/* BUG REAL DE RENDIMIENTO ENCONTRADO Y ARREGLADO (Arturo: "sigue lento"
 * al abrir por primera vez una pagina con imagenes JPX pesadas de
 * varias capas -- ver DESIGN.md seccion 75 y el fix en cb_pass_cleanup
 * mas abajo, que solo, ya duplico la velocidad). getenv("PDF_JPX_DEBUG")
 * y getenv("PDF_JPX_ONLY_V0") se llamaban SIN CACHEAR en un par de
 * lugares realmente calientes: cb_decode_sign() (una vez por CADA
 * coeficiente que se vuelve significativo -- pueden ser millones en
 * una imagen real) y el armado final del buffer RGB (una vez POR
 * PIXEL de la imagen completa). Como estas banderas de entorno son de
 * DEBUG -- nunca cambian durante la corrida del proceso -- alcanza con
 * consultarlas UNA sola vez por nombre y cachear el resultado; todas
 * las llamadas siguientes devuelven el valor cacheado sin tocar el
 * entorno de nuevo. */
static int jpx_env_flag_cached(const char *name, int *cache)
{
    if (*cache < 0)
        *cache = (getenv(name) != NULL) ? 1 : 0;
    return *cache;
}

static int jpx_debug_on(void)
{
    static int cache = -1;
    return jpx_env_flag_cached("PDF_JPX_DEBUG", &cache);
}

static int jpx_only_v0_on(void)
{
    static int cache = -1;
    return jpx_env_flag_cached("PDF_JPX_ONLY_V0", &cache);
}

/* ====================================================================
 * 1. Decodificador aritmetico MQ (T.800 Annex C / identico al de
 *    JBIG2 Annex E -- mismo diseño de "estados" y mismas contantes).
 * ==================================================================== */

typedef struct
{
    unsigned short qe;
    unsigned char  nmps, nlps, sw;
} mq_state_row;

/* Tabla de estados Qe/NMPS/NLPS/SWITCH -- T.800 Tabla C.2, 47 filas.
 * Verificada contra el texto del estandar (es la misma tabla
 * universalmente citada, tambien usada por JBIG2). */
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

/* Contexto MQ: un par (indice de estado, bit MPS actual). */
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

/* ====================================================================
 * 2. Lector de bits crudos para headers de paquete (T.800 B.10) --
 *    BUG QUE SE EVITO A TIEMPO: los headers de paquete (donde viven
 *    los arboles de tags) NO pasan por el decodificador aritmetico MQ
 *    -- son bits CRUDOS empaquetados MSB-primero, con la MISMA regla
 *    de "bit-stuffing" que el resto del codestream (un byte 0xFF debe
 *    ir seguido de un byte cuyo bit mas significativo es 0, para
 *    evitar que aparezca por accidente un codigo de marcador 0xFFxx
 *    dentro de los datos) -- el decodificador MQ se usa unicamente
 *    para los DATOS de cada code-block (las pasadas de codificacion
 *    por plano de bits, mas abajo en la seccion 3). Confundir los dos
 *    (usar MQ para el header tambien) desincroniza la lectura desde
 *    el primer bit. */

typedef struct
{
    const unsigned char *data;
    long len;
    long bytepos;
    int bitpos;      /* proximo bit a leer, 7 (MSB) .. 0 (LSB) */
    int prev_was_ff;
} bitrd;

static void bitrd_init(bitrd *b, const unsigned char *data, long len)
{
    b->data = data; b->len = len; b->bytepos = 0; b->bitpos = 7; b->prev_was_ff = 0;
}

static int bitrd_bit(bitrd *b)
{
    unsigned char cur;
    int bit;

    if (b->bytepos >= b->len) return 0; /* stream agotado: tolerante, devuelve 0 */

    if (b->prev_was_ff && b->bitpos == 7)
        b->bitpos = 6; /* bit de relleno tras un 0xFF: no es un bit de dato real */

    cur = b->data[b->bytepos];
    bit = (cur >> b->bitpos) & 1;

    if (b->bitpos == 0)
    {
        b->prev_was_ff = (cur == 0xFF);
        b->bytepos++;
        b->bitpos = 7;
    }
    else
    {
        b->bitpos--;
    }
    return bit;
}

/* Lee 'n' bits crudos como un entero MSB-primero (usado para el
 * campo de longitud del contribucion de un code-block, y para los
 * 2 bits del indice dentro de un "run" en la pasada de limpieza --
 * ver seccion 3). */
static unsigned int bitrd_bits(bitrd *b, int n)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 1) | (unsigned int)bitrd_bit(b);
    return v;
}

/* Arbol de tags -- estructura que codifica de forma compacta, por
 * precinto, que code-blocks estan incluidos en cada paquete ("arbol
 * de inclusion") y cuantos planos de bits cero tiene cada uno antes
 * de su primer bit significativo ("arbol de planos-cero"). Sobre el
 * lector de bits crudos de arriba (NO arithmetic-coded, ver
 * comentario arriba). Implementado como arreglo explicito por nivel
 * en vez de la formulacion recursiva del estandar -- mas facil de
 * mantener vivo entre paquetes sucesivos (el estado -- 'low' -- debe
 * persistir entre invocaciones, tal como exige el algoritmo). */

typedef struct
{
    int nlevels;
    int *w_at_level;
    int *h_at_level;
    int **value;    /* value[level][y*w+x] -- valor ya CONFIRMADO (o "infinito" si no) */
    int **low;      /* low[level][y*w+x] -- cota inferior ya explorada */
} tag_tree;

static tag_tree *tag_tree_create(pdf_arena *arena, int w, int h)
{
    tag_tree *t;
    int lw, lh, nlevels, i;

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    t = (tag_tree *)pdf_arena_alloc(arena, sizeof(tag_tree));
    if (t == NULL) return NULL;

    nlevels = 1;
    lw = w; lh = h;
    while (lw > 1 || lh > 1)
    {
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
        nlevels++;
    }
    t->nlevels = nlevels;

    t->w_at_level = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)nlevels);
    t->h_at_level = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)nlevels);
    t->value = (int **)pdf_arena_alloc(arena, sizeof(int *) * (size_t)nlevels);
    t->low   = (int **)pdf_arena_alloc(arena, sizeof(int *) * (size_t)nlevels);
    if (t->w_at_level == NULL || t->h_at_level == NULL || t->value == NULL || t->low == NULL)
        return NULL;

    lw = w; lh = h;
    for (i = 0; i < nlevels; i++)
    {
        int k;
        t->w_at_level[i] = lw;
        t->h_at_level[i] = lh;
        t->value[i] = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)(lw * lh));
        t->low[i]   = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)(lw * lh));
        if (t->value[i] == NULL || t->low[i] == NULL) return NULL;
        for (k = 0; k < lw * lh; k++) { t->value[i][k] = 0x7FFFFFF; t->low[i][k] = 0; }
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
    }

    return t;
}

/* Decodifica progresivamente el valor de la hoja (x,y) hasta que se
 * sepa que es >= threshold (arbol de inclusion, threshold = num de
 * capa) o hasta conocer el valor EXACTO (arbol de planos-cero,
 * threshold = infinito practico). Devuelve la cota conocida tras
 * decodificar. */
static int tag_tree_decode(tag_tree *t, bitrd *br, int x, int y, int threshold)
{
    int lxs[32], lys[32];
    int level, known_low;

    lxs[0] = x; lys[0] = y;
    for (level = 1; level < t->nlevels; level++)
    {
        lxs[level] = lxs[level-1] / 2;
        lys[level] = lys[level-1] / 2;
    }

    known_low = 0;
    for (level = t->nlevels - 1; level >= 0; level--)
    {
        int w = t->w_at_level[level];
        int idx = lys[level] * w + lxs[level];

        if (t->low[level][idx] < known_low)
            t->low[level][idx] = known_low;

        while (t->low[level][idx] < threshold && t->low[level][idx] < t->value[level][idx])
        {
            int bit = bitrd_bit(br);
            if (bit == 0)
                t->low[level][idx]++;
            else
                t->value[level][idx] = t->low[level][idx];
        }
        known_low = t->low[level][idx];
        if (t->value[level][idx] < known_low)
            known_low = t->value[level][idx];
    }
    return known_low;
}

/* ====================================================================
 * 3. EBCOT Tier-1: decodificacion por planos de bits de un
 *    code-block (T.800 Annex D). Cada code-block, una vez que Tier-2
 *    (mas abajo) le extrajo sus bytes de datos, se decodifica de
 *    forma completamente independiente -- estado y contextos propios,
 *    su propio decodificador MQ arrancado de cero.
 *
 *    19 contextos por code-block (T.800 Tabla D.7):
 *      0-8   Zero Coding (ZC) -- significancia de un coeficiente aun
 *            no significativo, segun el patron de vecinos ya
 *            significativos (3 tablas de mapeo distintas segun la
 *            orientacion de la subbanda: LL/LH, HL, HH).
 *      9-13  Sign Coding (SC) -- signo de un coeficiente que ACABA de
 *            volverse significativo, segun el signo de sus vecinos
 *            horizontales/verticales ya significativos.
 *      14-16 Magnitude Refinement (MR) -- bit de refinamiento de un
 *            coeficiente YA significativo de una pasada anterior.
 *      17    Run-length (RL) -- atajo para "estos 4 coeficientes de
 *            una columna siguen todos insignificantes", usado en la
 *            pasada de limpieza.
 *      18    Uniform -- contexto de probabilidad fija (~0.5) usado
 *            para los 2 bits de indice dentro de un run.
 */

#define ZC_CTX_BASE   0
#define SC_CTX_BASE   9
#define MR_CTX_BASE  14
#define RL_CTX       17
#define UNIFORM_CTX  18
#define NUM_CTX      19

/* Tabla D.1: mapeo (h,v,d) -> contexto ZC (0-8), para subbandas
 * LL/LH (h = vecinos horizontales significativos [0-2], v =
 * verticales [0-2], d = diagonales [0-4]). */
static int zc_ctx_lh(int h, int v, int d)
{
    if (h == 2) return 8;
    if (h == 1) { if (v >= 1) return 7; if (d >= 1) return 6; return 5; }
    /* h == 0 */
    if (v == 2) return 4;
    if (v == 1) return 3;
    if (d >= 2) return 2;
    if (d == 1) return 1;
    return 0;
}

/* HL: igual que LH pero con h y v intercambiados (T.800 D.3.1). */
static int zc_ctx_hl(int h, int v, int d) { return zc_ctx_lh(v, h, d); }

/* HH: basado principalmente en d, con h+v como desempate (Tabla D.1,
 * tercera columna). */
static int zc_ctx_hh(int h, int v, int d)
{
    int hv = h + v;
    if (d >= 3) return 8;
    if (d == 2) { if (hv >= 1) return 7; return 6; }
    if (d == 1) { if (hv >= 2) return 5; if (hv == 1) return 4; return 3; }
    /* d == 0 */
    if (hv >= 2) return 2;
    if (hv == 1) return 1;
    return 0;
}

/* orientacion de subbanda, para elegir la tabla ZC correcta y (mas
 * abajo) el orden de sub-muestreo al armar el plano de coeficientes
 * completo a partir de sus code-blocks. */
typedef enum { BAND_LL = 0, BAND_HL = 1, BAND_LH = 2, BAND_HH = 3 } band_type;

/* Estado por-coeficiente de un code-block durante Tier-1. Arreglos
 * paralelos de (w+2)*(h+2) con 1 pixel de borde (siempre
 * insignificante) para simplificar el acceso a vecinos sin chequear
 * bordes en cada lectura. */
typedef struct
{
    int w, h;         /* tamanio real (sin el borde) */
    int stride;       /* w+2 */
    unsigned char *sig;      /* 1 = ya significativo */
    unsigned char *sign;     /* 0=positivo, 1=negativo (valido si sig) */
    unsigned char *refined;  /* 1 = ya paso por al menos una pasada MR */
    unsigned char *visited;  /* se resetea cada plano: ya se decidio su significancia ESTE plano (en SPP o CP) */
    unsigned int  *mag;      /* magnitud acumulada (bits de a uno, de MSB a LSB) */
    band_type band;
    mq_ctx ctx[NUM_CTX];

    /* BUG REAL ENCONTRADO Y ARREGLADO (Arturo probo un PDF real con
     * imagenes JPX de 5 capas de calidad -- "checkerboard" de bloques
     * de colores en vez del icono real): las capas de un code-block NO
     * son streams MQ independientes, son UN SOLO stream continuo
     * cortado por limites de PASADA -- decodificar cada capa por
     * separado (como hacia esta funcion antes, con mq_init() arrancando
     * de cero en cada llamada) interpreta los bytes de la capa 2+ como
     * si fueran el INICIO de un stream nuevo, produciendo basura. Fix:
     * en vez de decodificar apenas llega cada capa, se ACUMULAN los
     * rangos (offset,largo) dentro de tile_data de cada capa que le
     * aporto algo a este code-block (range_off/range_len, hasta
     * max_ranges = cod->num_layers -- un code-block aparece como mucho
     * una vez por capa) y se decodifica UNA SOLA VEZ al final, con
     * todos los bytes concatenados en orden y el total_passes YA
     * final -- ver el segundo bucle nuevo despues del recorrido de
     * paquetes en jpx_decode_tile_data(). Para el caso de 1 sola capa
     * (el unico soportado antes de este fix) esto es matematicamente
     * identico a lo de antes, solo reordenado -- por eso no hace falta
     * un camino separado para ese caso. */
    long *range_off;
    int  *range_len;
    int   n_ranges;
    int   max_ranges;
} cb_state;

static cb_state *cb_state_create(pdf_arena *arena, int w, int h, band_type band, int num_layers)
{
    cb_state *s;
    int stride, total, i;

    s = (cb_state *)pdf_arena_alloc(arena, sizeof(cb_state));
    if (s == NULL) return NULL;
    s->w = w; s->h = h; s->band = band;
    stride = w + 2;
    s->stride = stride;
    total = stride * (h + 2);

    s->sig     = (unsigned char *)pdf_arena_alloc(arena, (size_t)total);
    s->sign    = (unsigned char *)pdf_arena_alloc(arena, (size_t)total);
    s->refined = (unsigned char *)pdf_arena_alloc(arena, (size_t)total);
    s->visited = (unsigned char *)pdf_arena_alloc(arena, (size_t)total);
    s->mag     = (unsigned int  *)pdf_arena_alloc(arena, sizeof(unsigned int) * (size_t)total);
    if (s->sig == NULL || s->sign == NULL || s->refined == NULL ||
        s->visited == NULL || s->mag == NULL)
        return NULL;

    memset(s->sig, 0, (size_t)total);
    memset(s->sign, 0, (size_t)total);
    memset(s->refined, 0, (size_t)total);
    memset(s->visited, 0, (size_t)total);
    memset(s->mag, 0, sizeof(unsigned int) * (size_t)total);

    for (i = 0; i < NUM_CTX; i++) { s->ctx[i].i = 0; s->ctx[i].mps = 0; }

    if (num_layers < 1) num_layers = 1;
    s->n_ranges   = 0;
    s->max_ranges = num_layers;
    s->range_off  = (long *)pdf_arena_alloc(arena, sizeof(long) * (size_t)num_layers);
    s->range_len  = (int  *)pdf_arena_alloc(arena, sizeof(int)  * (size_t)num_layers);
    if (s->range_off == NULL || s->range_len == NULL)
        return NULL;
    /* BUG REAL ENCONTRADO Y CONFIRMADO (T.800 Tabla D.7 / verificado
     * ademas linea por linea contra el codigo fuente real de OpenJPEG
     * 2.5.4, funcion opj_mqc_reset_enc en mqc.c y su contraparte de
     * decodificacion en t1.c -- t1_decode_cblk): TRES contextos, no
     * uno, arrancan en un estado distinto de 0:
     *   - ZC_CTX_BASE+0 (el contexto de "sin vecinos significativos",
     *     el mas usado de lejos) arranca en estado 4, no 0.
     *   - RL_CTX (run-length) arranca en estado 3, no 0.
     *   - UNIFORM_CTX arranca en estado 46, no 0 (aunque el estado 46
     *     tiene el mismo Qe=0x5601 que el estado 0, el NMPS/NLPS de
     *     ahi en adelante difieren, asi que SI importa).
     * Encontrado construyendo casos de prueba minimos (imagenes
     * JPEG2000 sinteticas, 5/3 sin perdida, comparadas bit a bit
     * contra Pillow/OpenJPEG como referencia real) y comparando
     * despues directo contra las fuentes de OpenJPEG: con los 19
     * contextos en estado 0 el decodificador se desincronizaba
     * inmediatamente despues del primer coeficiente significativo real
     * de cada code-block (todo lo posterior salia como ruido); con
     * solo el fix de RL_CTX=3 el PRIMER coeficiente ya coincidia pero
     * el segundo (usando ZC_CTX_BASE+0, tambien mal inicializado)
     * seguia fallando. Con los tres fixes juntos, un code-block de
     * prueba con un unico pixel distinto de 0 (y otro totalmente
     * plano) decodifican exactos, sin ningun falso positivo. Este era
     * el bug detras del "ruido tipo tablero de ajedrez" en imagenes
     * JPX reales (Conveyor_Handbook.pdf): estos 3 contextos son
     * justamente los mas usados de cleanup (la pasada que corre
     * SIEMPRE, en todo bitplane), asi que un Qe inicial equivocado ahi
     * desincroniza el MQ-decoder casi de inmediato en CADA
     * code-block. */
    s->ctx[ZC_CTX_BASE + 0].i = 4;
    s->ctx[RL_CTX].i = 3;
    s->ctx[UNIFORM_CTX].i = 46;

    return s;
}

/* indice dentro de los arreglos, para la posicion (x,y) YA CON offset
 * de borde (asi que el rango real de datos es x,y en [1,w],[1,h] --
 * los llamadores externos usan coordenadas 0-based y suman 1). */
#define CB_IDX(s, x, y) (((y) + 1) * (s)->stride + ((x) + 1))

static int cb_sig(cb_state *s, int x, int y) { return s->sig[CB_IDX(s, x, y)]; }
static int cb_sign(cb_state *s, int x, int y) { return s->sign[CB_IDX(s, x, y)]; }

/* suma de vecinos significativos en cada categoria (D.3.1). */
static void cb_neighbor_counts(cb_state *s, int x, int y, int *h, int *v, int *d)
{
    *h = cb_sig(s, x-1, y) + cb_sig(s, x+1, y);
    *v = cb_sig(s, x, y-1) + cb_sig(s, x, y+1);
    *d = cb_sig(s, x-1, y-1) + cb_sig(s, x+1, y-1) + cb_sig(s, x-1, y+1) + cb_sig(s, x+1, y+1);
}

static int cb_zc_context(cb_state *s, int x, int y)
{
    int h, v, d;
    cb_neighbor_counts(s, x, y, &h, &v, &d);
    switch (s->band)
    {
        case BAND_HL: return zc_ctx_hl(h, v, d);
        case BAND_HH: return zc_ctx_hh(h, v, d);
        default:      return zc_ctx_lh(h, v, d); /* LL y LH usan la misma tabla */
    }
}

/* Tabla D.2: contribucion de signo de un vecino significativo (+1 si
 * signo positivo, -1 si negativo, 0 si no significativo), sumada por
 * par opuesto y saturada a {-1,0,1}. */
static int sign_contrib(cb_state *s, int x, int y)
{
    if (!cb_sig(s, x, y)) return 0;
    return cb_sign(s, x, y) ? -1 : 1;
}

static int cb_sign_context(cb_state *s, int x, int y, int *xorbit)
{
    int hc = sign_contrib(s, x-1, y) + sign_contrib(s, x+1, y);
    int vc = sign_contrib(s, x, y-1) + sign_contrib(s, x, y+1);
    int H = (hc > 0) - (hc < 0);
    int V = (vc > 0) - (vc < 0);
    static const int ctx_tab[3][3] = {
        /* V=-1        V=0         V=1     */
        {  13,          12,         11   }, /* H=-1 */
        {  10,           9,         10   }, /* H=0  */
        {  11,          12,         13   }, /* H=1  */
    };
    /* xorbit: 1 cuando el patron es el "espejado" del canonico (H<0,
     * o H==0 && V<0) -- el bit de signo decodificado se XORea con
     * esto antes de interpretarse (T.800 D.3.2, Tabla D.2). */
    *xorbit = (H < 0 || (H == 0 && V < 0)) ? 1 : 0;
    return ctx_tab[H + 1][V + 1];
}

/* Decodifica el signo de un coeficiente que acaba de volverse
 * significativo, lo guarda, y devuelve 0/1 (0=positivo). */
static int cb_decode_sign(cb_state *s, mq_dec *mq, int x, int y)
{
    int xorbit, ctxi, bit, sign;
    ctxi = cb_sign_context(s, x, y, &xorbit); /* ya es indice ABSOLUTO (9-13) dentro de s->ctx */
    bit = mq_decode(mq, &s->ctx[ctxi]);
    sign = bit ^ xorbit;
    if (jpx_debug_on())
        fprintf(stderr, "DEBUG signo (%d,%d): ctxi=%d xorbit=%d bit_crudo=%d -> sign_final=%d\n",
                x, y, ctxi, xorbit, bit, sign);
    s->sign[CB_IDX(s, x, y)] = (unsigned char)sign;
    return sign;
}

/* Contexto de refinamiento (D.3.3, Tabla D.3): si es la primera vez
 * que se refina este coeficiente, depende de si tiene algun vecino
 * significativo; si ya se refino antes, un unico contexto fijo. */
static int cb_mr_context(cb_state *s, int x, int y)
{
    int idx = CB_IDX(s, x, y);
    if (s->refined[idx])
        return MR_CTX_BASE + 2;
    {
        int h, v, d;
        cb_neighbor_counts(s, x, y, &h, &v, &d);
        return (h + v + d) > 0 ? MR_CTX_BASE + 1 : MR_CTX_BASE + 0;
    }
}

/* Una pasada completa de Significance Propagation (D.4). Recorre el
 * code-block en "franjas" de 4 filas (T.800 D.2: orden de escaneo por
 * columnas dentro de cada franja de 4, franjas de arriba hacia
 * abajo). Solo visita coeficientes AUN no significativos que tengan
 * al menos un vecino ya significativo. */
static void cb_pass_spp(cb_state *s, mq_dec *mq, int bitplane)
{
    int y0;
    for (y0 = 0; y0 < s->h; y0 += 4)
    {
        int rows = (s->h - y0 < 4) ? s->h - y0 : 4;
        int x;
        for (x = 0; x < s->w; x++)
        {
            int yy;
            for (yy = 0; yy < rows; yy++)
            {
                int y = y0 + yy;
                int idx = CB_IDX(s, x, y);
                int h, v, d;

                if (s->sig[idx]) continue; /* ya significativo de antes: no toca a SPP */
                cb_neighbor_counts(s, x, y, &h, &v, &d);
                if (h + v + d == 0) continue; /* sin vecinos significativos: lo maneja CP */

                s->visited[idx] = 1;
                if (mq_decode(mq, &s->ctx[ZC_CTX_BASE + cb_zc_context(s, x, y)]))
                {
                    s->sig[idx] = 1;
                    s->mag[idx] = 1u << bitplane;
                    cb_decode_sign(s, mq, x, y);
                }
            }
        }
    }
}

/* Magnitude Refinement (D.5): un bit de refinamiento por cada
 * coeficiente que YA era significativo ANTES de esta pasada (no los
 * que se acaban de activar en la SPP de este mismo plano -- por eso
 * el chequeo de 'visited' esta invertido respecto a SPP: acepta los
 * que NO fueron tocados en SPP este plano pero SI son significativos
 * de planos anteriores). */
static void cb_pass_mrp(cb_state *s, mq_dec *mq, int bitplane)
{
    int y0;
    for (y0 = 0; y0 < s->h; y0 += 4)
    {
        int rows = (s->h - y0 < 4) ? s->h - y0 : 4;
        int x;
        for (x = 0; x < s->w; x++)
        {
            int yy;
            for (yy = 0; yy < rows; yy++)
            {
                int y = y0 + yy;
                int idx = CB_IDX(s, x, y);

                if (!s->sig[idx]) continue;
                if (s->visited[idx]) continue; /* se volvio significativo RECIEN en la SPP de este plano */

                if (mq_decode(mq, &s->ctx[cb_mr_context(s, x, y)]))
                    s->mag[idx] |= 1u << bitplane;
                s->refined[idx] = 1;
            }
        }
    }
}

/* Limpieza (D.6): cubre todo lo que SPP y MRP no tocaron este plano
 * (coeficientes sin vecino significativo y aun no significativos).
 * Incluye el atajo de "run-length" para franjas de 4 completas que
 * arrancan totalmente "vírgenes" (ver comentario grande en el
 * cuerpo). */
static void cb_pass_cleanup(cb_state *s, mq_dec *mq, int bitplane)
{
    int y0;
    /* BUG REAL DE RENDIMIENTO ENCONTRADO Y ARREGLADO (Arturo: "sigue
     * lento" al abrir la primera vez -- perfilado con PDF_JPX_TIMING
     * mostro que Tier-1 (esta funcion + cb_pass_spp/mrp) es ~92% del
     * tiempo total de decodificar una imagen JPX real): getenv() se
     * llamaba adentro del loop MAS interno (una vez por cada franja de
     * 4 filas x columna, osea (alto/4)*ancho veces POR BITPLANO POR
     * CODE-BLOCK -- para una imagen con miles de code-blocks y varios
     * bitplanos cada uno, esto suma millones de llamadas a getenv(),
     * que hace un escaneo lineal de variables de entorno cada vez, por
     * una bandera de DEBUG que en el uso normal jamas esta seteada.
     * Sacado del loop -- se consulta UNA sola vez por llamada a esta
     * funcion (una vez por bitplano por code-block, no una vez por
     * coeficiente) sin cambiar el comportamiento en absoluto. */
    int no_rl = (getenv("PDF_JPX_NO_RL") != NULL);
    for (y0 = 0; y0 < s->h; y0 += 4)
    {
        int rows = (s->h - y0 < 4) ? s->h - y0 : 4;
        int x;
        for (x = 0; x < s->w; x++)
        {
            int yy = 0;

            /* Atajo de run-length (D.6): solo aplica a una franja
             * COMPLETA de 4 (rows==4) donde NINGUNO de los 4 fue
             * tocado por SPP/MRP este plano NI ya era significativo
             * NI tiene vecino significativo (osea, los 4 hubieran
             * pasado por la rama "general" de aca abajo sin disparar
             * nada) -- en ese caso el codificador uso el contexto RL
             * para decir de un saque "ninguno de los 4 se activa" (un
             * solo bit=0) o "el primero en activarse es el indice
             * tal" (bit=1 + 2 bits de indice, contexto UNIFORM). */
            if (rows == 4 && !no_rl)
            {
                int all_virgin = 1, k;
                for (k = 0; k < 4; k++)
                {
                    int idx = CB_IDX(s, x, y0 + k);
                    if (s->sig[idx] || s->visited[idx]) { all_virgin = 0; break; }
                    {
                        int h, v, d;
                        cb_neighbor_counts(s, x, y0 + k, &h, &v, &d);
                        if (h + v + d != 0) { all_virgin = 0; break; }
                    }
                }
                if (all_virgin)
                {
                    if (!mq_decode(mq, &s->ctx[RL_CTX]))
                    {
                        /* ninguno de los 4 se activa este plano */
                        continue;
                    }
                    {
                        int idxbit0 = mq_decode(mq, &s->ctx[UNIFORM_CTX]);
                        int idxbit1 = mq_decode(mq, &s->ctx[UNIFORM_CTX]);
                        int first = (idxbit0 << 1) | idxbit1;
                        /* los 'first' primeros de la franja quedan
                         * confirmados insignificantes (nada que hacer);
                         * el de indice 'first' SI se activa (se salta
                         * su bit de significancia -- ya se sabe que es
                         * 1 -- pero SI hay que decodificar su signo);
                         * el resto de la franja sigue con decodificacion
                         * normal (yy arranca en first+1). */
                        int y = y0 + first;
                        int idx = CB_IDX(s, x, y);
                        s->sig[idx] = 1;
                        s->mag[idx] = 1u << bitplane;
                        cb_decode_sign(s, mq, x, y);
                        yy = first + 1;
                    }
                }
            }

            for (; yy < rows; yy++)
            {
                int y = y0 + yy;
                int idx = CB_IDX(s, x, y);

                if (s->sig[idx] || s->visited[idx]) continue;

                if (mq_decode(mq, &s->ctx[ZC_CTX_BASE + cb_zc_context(s, x, y)]))
                {
                    s->sig[idx] = 1;
                    s->mag[idx] = 1u << bitplane;
                    cb_decode_sign(s, mq, x, y);
                }
            }
        }
        /* 'visited' se resetea DESPUES de terminar el plano completo
         * (las 3 pasadas), no aca -- ver el bucle que llama a estas
         * 3 funciones mas abajo. */
    }
}

/* Decodifica un code-block completo: 'num_passes' pasadas en total
 * (repartidas en planos de bits: el plano MAS significativo arranca
 * SIEMPRE con cleanup unicamente -- no hay nada que propagar ni
 * refinar todavia -- los planos siguientes hacen SPP, MRP, cleanup en
 * ese orden). 'data'/'data_len' son los bytes de ESTE code-block
 * (extraidos por Tier-2), arrancando un decodificador MQ nuevo desde
 * cero (T.800: cada code-block es un segmento MQ independiente). */
static void cb_decode(cb_state *s, const unsigned char *data, long data_len,
                       int num_passes, int msb_bitplane)
{
    mq_dec mq;
    int bitplane = msb_bitplane;
    int pass = 0;
    int first_plane = 1;

    mq_init(&mq, data, data_len);

    while (pass < num_passes)
    {
        if (first_plane)
        {
            cb_pass_cleanup(s, &mq, bitplane);
            pass++;
            first_plane = 0;
        }
        else
        {
            if (pass < num_passes) { cb_pass_spp(s, &mq, bitplane); pass++; }
            if (pass < num_passes) { cb_pass_mrp(s, &mq, bitplane); pass++; }
            if (pass < num_passes) { cb_pass_cleanup(s, &mq, bitplane); pass++; }
        }
        memset(s->visited, 0, (size_t)s->stride * (size_t)(s->h + 2));
        bitplane--;
    }
}

/* ====================================================================
 * 5. Transformada wavelet inversa (T.800 Annex F) -- 5/3 reversible
 *    (entera, sin perdida) y 9/7 irreversible (con perdida, la que
 *    usa el caso real de Conveyor_Handbook.pdf), ambas por "lifting",
 *    separables (1D aplicada primero por columna, despues por fila --
 *    ver comentario grande mas abajo de por que ese orden y no el
 *    otro) y multi-nivel (se reconstruye de la resolucion mas
 *    gruesa/chica hacia la mas fina/grande, en 'nivel' pasadas).
 *
 *    Extension de borde: "simetrica de punto entero" (T.800 F.3.3) --
 *    el indice -1 espeja al indice 1 (NO al 0), el indice n espeja al
 *    n-2, etc. -- necesaria porque los filtros de lifting miran 1-2
 *    posiciones mas alla del borde real de cada subbanda.
 */

static double idwt_mirror(const double *arr, int n, int i)
{
    while (i < 0 || i >= n)
    {
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
    }
    return arr[i];
}

/* Sintesis 1D 5/3 reversible (T.800 F.3.3, formulas inversas). 'low'
 * (n_low muestras) y 'high' (n_high muestras) entran SEPARADOS; el
 * resultado entrelazado (n_low+n_high muestras) sale en 'out'
 * (out[2n]=par/low, out[2n+1]=impar/high, formula estandar). */
static void idwt_53_1d(const double *low, int n_low, const double *high, int n_high, double *out)
{
    int n = n_low + n_high;
    int i;
    if (n == 0) return;
    if (n == 1) { out[0] = low[0]; return; }

    for (i = 0; i < n; i++)
        out[i] = (i % 2 == 0) ? low[i/2] : high[i/2];

    /* deshacer 'update': par -= floor((impar[-1]+impar[+1]+2)/4) */
    for (i = 0; i < n; i += 2)
    {
        double h_prev = idwt_mirror(out, n, i - 1);
        double h_next = idwt_mirror(out, n, i + 1);
        out[i] -= floor((h_prev + h_next + 2.0) / 4.0);
    }
    /* deshacer 'predict': impar += floor((par[-1... ya actualizado]+par[+1])/2) -- usa los pares YA actualizados arriba */
    for (i = 1; i < n; i += 2)
    {
        double l_prev = idwt_mirror(out, n, i - 1);
        double l_next = idwt_mirror(out, n, i + 1);
        out[i] += floor((l_prev + l_next) / 2.0);
    }
}

/* Sintesis 1D 9/7 irreversible (T.800 F.4, lifting de 4 pasos +
 * escalado, invertidos en orden opuesto -- ver comentario grande en
 * DESIGN.md seccion 60 con la derivacion completa de signos). */
static void idwt_97_1d(const double *low, int n_low, const double *high, int n_high, double *out)
{
    static const double ALPHA = -1.586134342059924;
    static const double BETA  = -0.052980118572961;
    static const double GAMMA =  0.882911075530934;
    static const double DELTA =  0.443506852043971;
    static const double KREC  =  1.0 / 1.230174104914001; /* 1/K, para el par (low) */
    static const double K     =  1.230174104914001;       /* K, para el impar (high) */
    int n = n_low + n_high;
    int i;
    if (n == 0) return;
    if (n == 1) { out[0] = low[0] * KREC; return; }

    for (i = 0; i < n; i++)
        out[i] = (i % 2 == 0) ? low[i/2] : high[i/2];

    /* deshacer escalado -- BUG REAL ENCONTRADO Y CONFIRMADO (contra
     * el codigo fuente real de OpenJPEG 2.5.4, dwt.c,
     * opj_v8dwt_decode: 'wavelet+a' -- el lado LOW/par -- se escala
     * con K, y 'wavelet+b' -- el lado HIGH/impar -- con 1/K). La
     * version anterior tenia esto AL REVES (low*=1/K, high*=K),
     * atenuando brutalmente la amplitud reconstruida en cada nivel de
     * sintesis (confirmado con un gradiente sintetico: la forma/
     * direccion salian correctas pero el rango quedaba comprimido a
     * ~3% del real). Con Tier-1 ya corregido (ver cb_state_create),
     * este swap deja la reconstruccion bit-exacta contra Pillow/
     * OpenJPEG en los casos de prueba sinteticos. */
    for (i = 0; i < n; i += 2) out[i] *= K;
    for (i = 1; i < n; i += 2) out[i] *= KREC;

    /* deshacer update2 (delta) */
    for (i = 0; i < n; i += 2)
        out[i] -= DELTA * (idwt_mirror(out, n, i - 1) + idwt_mirror(out, n, i + 1));
    /* deshacer predict2 (gamma) */
    for (i = 1; i < n; i += 2)
        out[i] -= GAMMA * (idwt_mirror(out, n, i - 1) + idwt_mirror(out, n, i + 1));
    /* deshacer update1 (beta) */
    for (i = 0; i < n; i += 2)
        out[i] -= BETA * (idwt_mirror(out, n, i - 1) + idwt_mirror(out, n, i + 1));
    /* deshacer predict1 (alpha) */
    for (i = 1; i < n; i += 2)
        out[i] -= ALPHA * (idwt_mirror(out, n, i - 1) + idwt_mirror(out, n, i + 1));
}

typedef void (*idwt_1d_fn)(const double *low, int n_low, const double *high, int n_high, double *out);

/* Un nivel de sintesis 2D: combina 4 subbandas (LL,HL,LH,HH) en una
 * imagen de resolucion mas fina. Ver el comentario grande arriba de
 * esta seccion para la derivacion del orden (columnas primero,
 * despues filas) y de que LL/LH comparten ANCHO (banda horizontal
 * "Low") mientras que LL/HL comparten ALTO (banda vertical "Low"). */
static double *idwt_synth_level(pdf_arena *arena, idwt_1d_fn fn1d,
                                 const double *ll, const double *hl,
                                 const double *lh, const double *hh,
                                 int llw, int llh, int hlw, int lhh,
                                 int *out_w, int *out_h)
{
    int ow = llw + hlw;
    int oh = llh + lhh;
    double *left, *right, *out;
    double *col_low, *col_high, *col_out;
    int x, y;

    *out_w = ow; *out_h = oh;
    if (ow <= 0 || oh <= 0) return NULL;

    left  = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)llw * (size_t)oh);
    right = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)hlw * (size_t)oh);
    out   = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)ow  * (size_t)oh);
    if (left == NULL || right == NULL || out == NULL) return NULL;

    col_low  = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)(llh > lhh ? llh : lhh));
    col_high = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)(llh > lhh ? llh : lhh));
    col_out  = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)oh);
    if (col_low == NULL || col_high == NULL || col_out == NULL) return NULL;

    /* columnas: LL(low)+LH(high) -> 'left' (ancho llw); HL(low)+HH(high) -> 'right' (ancho hlw) */
    for (x = 0; x < llw; x++)
    {
        for (y = 0; y < llh; y++) col_low[y]  = ll[y * llw + x];
        for (y = 0; y < lhh; y++) col_high[y] = lh[y * llw + x];
        fn1d(col_low, llh, col_high, lhh, col_out);
        for (y = 0; y < oh; y++) left[y * llw + x] = col_out[y];
    }
    for (x = 0; x < hlw; x++)
    {
        for (y = 0; y < llh; y++) col_low[y]  = hl[y * hlw + x];
        for (y = 0; y < lhh; y++) col_high[y] = hh[y * hlw + x];
        fn1d(col_low, llh, col_high, lhh, col_out);
        for (y = 0; y < oh; y++) right[y * hlw + x] = col_out[y];
    }

    /* filas: left(low, ancho llw) + right(high, ancho hlw) -> salida (ancho ow) */
    {
        double *row_out = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)ow);
        if (row_out == NULL) return NULL;
        for (y = 0; y < oh; y++)
        {
            fn1d(left + (size_t)y * llw, llw, right + (size_t)y * hlw, hlw, row_out);
            memcpy(out + (size_t)y * ow, row_out, sizeof(double) * (size_t)ow);
        }
    }

    return out;
}

/* PROBADO Y DESCARTADO (ver DESIGN.md, ronda "JPX real"): se
 * implemento temporalmente una variante de idwt_synth_level que hace
 * la pasada de FILAS antes que la de COLUMNAS (el orden opuesto al de
 * arriba), para probar empiricamente si el orden supuesto era el
 * equivocado -- resultado IDENTICO (mismo patron de "tablero de
 * ajedrez"), como es de esperar matematicamente para una transformada
 * separable real (el orden fila/columna no puede ser la causa de un
 * resultado distinto si el resto de la matematica es consistente) --
 * eliminada, no era el bug. */

/* ====================================================================
 * 6. Driver principal: boxes JP2, marcadores J2K, geometria de
 *    subbandas/precintos/code-blocks, Tier-2 (paquetes), y el
 *    ensamblado final (dequantizacion, sintesis wavelet multi-nivel
 *    por componente, transformada de color inversa, union de tiles).
 * ==================================================================== */

#define JPX_MAX_COMP   4
#define JPX_MAX_RES    33   /* num_decomp+1, generoso (NL real jamas pasa de ~10) */

typedef struct
{
    long Xsiz, Ysiz, XOsiz, YOsiz, XTsiz, YTsiz, XTOsiz, YTOsiz;
    int ncomp;
    int prec[JPX_MAX_COMP];
    int is_signed[JPX_MAX_COMP];
    int xr[JPX_MAX_COMP], yr[JPX_MAX_COMP];
} siz_params;

typedef struct
{
    int prog_order;
    int num_layers;
    int mct;
    int num_decomp;      /* NL */
    int cb_w, cb_h;      /* tamanio nominal de code-block, ya en pixeles (2^(exp+2)) */
    int transform;       /* 0 = 9/7 irreversible, 1 = 5/3 reversible */
} cod_params;

typedef struct
{
    int qstyle;                     /* 0=ninguna, 1=derivada, 2=expounded */
    int guard_bits;
    int exponent[3 * JPX_MAX_RES + 1];
    int mantissa[3 * JPX_MAX_RES + 1];
} qcd_params;

/* --- boxes JP2 -------------------------------------------------------- */

/* Busca el box 'jp2c' (codestream contiguo) recorriendo la estructura
 * de boxes ISO-BMFF que envuelve JPEG2000 en su variante ".jp2". Si
 * 'src' ya arranca directo con el codestream crudo (SOC = FF 4F, sin
 * ningun box), lo devuelve tal cual -- ambos casos son validos para
 * /JPXDecode segun el PDF (algunos generadores incrustan el archivo
 * .jp2 completo, otros solo el codestream). */
static int jpx_find_codestream(const unsigned char *src, long src_len,
                                const unsigned char **out, long *out_len)
{
    long pos = 0;

    if (src_len >= 2 && src[0] == 0xFF && src[1] == 0x4F)
    {
        *out = src; *out_len = src_len;
        return PDF_OK;
    }

    while (pos + 8 <= src_len)
    {
        long length = ((long)src[pos] << 24) | ((long)src[pos+1] << 16) |
                      ((long)src[pos+2] << 8) | (long)src[pos+3];
        long hdr = 8;
        long type_off = pos + 4;

        if (length == 1)
        {
            if (pos + 16 > src_len) break;
            length = 0;
            {
                int k;
                for (k = 0; k < 8; k++) length = (length << 8) | src[pos+8+k];
            }
            hdr = 16;
        }
        else if (length == 0)
        {
            length = src_len - pos;
        }
        if (length < hdr || pos + length > src_len) break;

        if (src[type_off] == 'j' && src[type_off+1] == 'p' &&
            src[type_off+2] == '2' && src[type_off+3] == 'c')
        {
            *out = src + pos + hdr;
            *out_len = length - hdr;
            return PDF_OK;
        }
        pos += length;
    }
    return PDF_ERR_BADARG; /* ni codestream crudo ni box jp2c encontrado */
}

/* --- geometria de resoluciones/subbandas ------------------------------- */

typedef struct { int x0, y0, x1, y1; } jpx_rect;
static int rect_w(jpx_rect r) { return r.x1 > r.x0 ? r.x1 - r.x0 : 0; }
static int rect_h(jpx_rect r) { return r.y1 > r.y0 ? r.y1 - r.y0 : 0; }

typedef struct
{
    int nres; /* NL+1 */
    jpx_rect ll[JPX_MAX_RES];              /* solo ll[0] es una subbanda real */
    jpx_rect hl[JPX_MAX_RES], lh[JPX_MAX_RES], hh[JPX_MAX_RES]; /* validos 1..NL */
} jpx_res_geom;

/* Parte el rango tile-componente [x0,x1)x[y0,y1) en sus NL niveles de
 * resolucion (T.800 B.5) -- ver comentario grande arriba de la
 * seccion 6 (DESIGN.md tiene la derivacion completa). Implementado de
 * forma RECURSIVA (particion binaria por paridad) en vez de la
 * formula directa con exponente -- mas facil de verificar que no
 * tenga un off-by-one. */
static void jpx_compute_geom(jpx_rect tcomp, int nl, jpx_res_geom *g)
{
    jpx_rect cur = tcomp;
    int r;
    if (nl >= JPX_MAX_RES) nl = JPX_MAX_RES - 1;
    g->nres = nl + 1;
    for (r = nl; r >= 1; r--)
    {
        int lx0 = (cur.x0 + 1) / 2, lx1 = (cur.x1 + 1) / 2;
        int hx0 = cur.x0 / 2, hx1 = cur.x1 / 2;
        int ly0 = (cur.y0 + 1) / 2, ly1 = (cur.y1 + 1) / 2;
        int hy0 = cur.y0 / 2, hy1 = cur.y1 / 2;

        g->hl[r].x0 = hx0; g->hl[r].x1 = hx1; g->hl[r].y0 = ly0; g->hl[r].y1 = ly1;
        g->lh[r].x0 = lx0; g->lh[r].x1 = lx1; g->lh[r].y0 = hy0; g->lh[r].y1 = hy1;
        g->hh[r].x0 = hx0; g->hh[r].x1 = hx1; g->hh[r].y0 = hy0; g->hh[r].y1 = hy1;

        cur.x0 = lx0; cur.x1 = lx1; cur.y0 = ly0; cur.y1 = ly1;
    }
    g->ll[0] = cur;
}

/* --- subbanda: grilla de code-blocks + arboles de tags ------------------ */

typedef struct
{
    jpx_rect ext;
    int ncbx, ncby;
    cb_state **cb;
    tag_tree *incl;
    tag_tree *zbp;
    int *lblock;
    int *included_before;
    int *total_passes;
    int *zero_planes;      /* -1 hasta que se decodifique en la primera inclusion */
    int mb;                 /* bitplanes maximos posibles para esta subbanda (guard_bits+exponent-1) */
    band_type band;
} jpx_subband;

static jpx_subband *jpx_subband_create(pdf_arena *arena, jpx_rect ext, int cbw, int cbh,
                                        band_type band, int mb)
{
    jpx_subband *sb;
    int ncbx, ncby, n, i;

    sb = (jpx_subband *)pdf_arena_alloc(arena, sizeof(jpx_subband));
    if (sb == NULL) return NULL;
    sb->ext = ext;
    sb->band = band;
    sb->mb = mb;

    ncbx = (rect_w(ext) + cbw - 1) / cbw;
    ncby = (rect_h(ext) + cbh - 1) / cbh;
    if (ncbx < 1) ncbx = 1;
    if (ncby < 1) ncby = 1;
    sb->ncbx = ncbx; sb->ncby = ncby;
    n = ncbx * ncby;

    sb->cb = (cb_state **)pdf_arena_alloc(arena, sizeof(cb_state *) * (size_t)n);
    sb->lblock = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)n);
    sb->included_before = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)n);
    sb->total_passes = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)n);
    sb->zero_planes = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)n);
    if (sb->cb == NULL || sb->lblock == NULL || sb->included_before == NULL ||
        sb->total_passes == NULL || sb->zero_planes == NULL)
        return NULL;

    for (i = 0; i < n; i++)
    {
        sb->cb[i] = NULL;
        sb->lblock[i] = 3; /* T.800 B.10.7: valor inicial estandar */
        sb->included_before[i] = 0;
        sb->total_passes[i] = 0;
        sb->zero_planes[i] = -1;
    }

    sb->incl = tag_tree_create(arena, ncbx, ncby);
    sb->zbp  = tag_tree_create(arena, ncbx, ncby);
    if (sb->incl == NULL || sb->zbp == NULL) return NULL;

    return sb;
}

/* NOTA: en el diseño actual, cada code-block recibe TODOS sus bytes
 * de una sola vez desde jpx_decode_tile (ver mas abajo) y se
 * decodifica de inmediato via cb_decode -- no hace falta un buffer de
 * acumulacion entre capas separado (limitacion: con mas de 1 capa por
 * code-block, cada capa nueva se decodificaria desde cero en vez de
 * incrementalmente; ver DESIGN.md seccion 60). */

/* Decodifica "numero de nuevas pasadas incluidas" (T.800 Tabla B.4). */
static int jpx_read_num_passes(bitrd *br)
{
    if (!bitrd_bit(br)) return 1;
    if (!bitrd_bit(br)) return 2;
    {
        unsigned int v = bitrd_bits(br, 2);
        if (v != 3) return 3 + (int)v;
    }
    {
        unsigned int v = bitrd_bits(br, 5);
        if (v != 31) return 6 + (int)v;
    }
    return 37 + (int)bitrd_bits(br, 7);
}

/* Header de UN paquete (T.800 B.10): recorre las subbandas presentes
 * en esta resolucion (1 si r==0 (solo LL), 3 si r>=1 (HL,LH,HH)),
 * code-block por code-block, decidiendo inclusion/planos-cero/numero
 * de pasadas/longitud -- y ANOTA (sin leer todavia) cuantos bytes de
 * 'body' le corresponden a cada code-block incluido, en el mismo
 * orden en que se recorrieron (para que el llamador pueda extraer
 * esos bytes justo despues, del cuerpo del paquete). Devuelve 0 si el
 * paquete estaba vacio (bit inicial en 0). 'layer' es el indice de
 * capa 0-based actual. */
static int jpx_packet_header(bitrd *br, jpx_subband **bands, int nbands, int layer,
                              int *out_cb_band, int *out_cb_idx, int *out_cb_len, int max_out)
{
    int nonempty = bitrd_bit(br);
    int nout = 0;
    int bi;

    if (!nonempty) return 0;

    for (bi = 0; bi < nbands; bi++)
    {
        jpx_subband *sb = bands[bi];
        int cy, cx;
        for (cy = 0; cy < sb->ncby; cy++)
        {
            for (cx = 0; cx < sb->ncbx; cx++)
            {
                int idx = cy * sb->ncbx + cx;
                int included;

                if (!sb->included_before[idx])
                {
                    int v = tag_tree_decode(sb->incl, br, cx, cy, layer + 1);
                    included = (v <= layer);
                }
                else
                {
                    included = bitrd_bit(br);
                }

                if (!included) continue;

                if (!sb->included_before[idx])
                {
                    sb->zero_planes[idx] = tag_tree_decode(sb->zbp, br, cx, cy, 999);
                    sb->included_before[idx] = 1;
                }

                {
                    int num_new = jpx_read_num_passes(br);
                    int incr = 0;
                    int bits_needed, len;
                    while (bitrd_bit(br)) incr++;
                    sb->lblock[idx] += incr;
                    /* T.800 B.10.7: bits para el campo de longitud */
                    bits_needed = sb->lblock[idx];
                    {
                        int p = num_new, e = 0;
                        while (p > 1) { p >>= 1; e++; }
                        bits_needed += e;
                    }
                    len = (int)bitrd_bits(br, bits_needed);

                    if (nout < max_out)
                    {
                        out_cb_band[nout] = bi;
                        out_cb_idx[nout] = idx;
                        out_cb_len[nout] = len;
                        nout++;
                    }
                    sb->total_passes[idx] += num_new;
                }
            }
        }
    }
    return nout;
}

static long jpx_u16(const unsigned char *p) { return ((long)p[0] << 8) | p[1]; }
static long jpx_u32(const unsigned char *p)
{ return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | p[3]; }

static void jpx_parse_siz(const unsigned char *seg, siz_params *siz)
{
    int i;
    siz->XOsiz  = jpx_u32(seg + 10);
    siz->YOsiz  = jpx_u32(seg + 14);
    siz->XTsiz  = jpx_u32(seg + 18);
    siz->YTsiz  = jpx_u32(seg + 22);
    siz->XTOsiz = jpx_u32(seg + 26);
    siz->YTOsiz = jpx_u32(seg + 30);
    siz->Xsiz   = jpx_u32(seg + 2);
    siz->Ysiz   = jpx_u32(seg + 6);
    siz->ncomp  = (int)jpx_u16(seg + 34);
    if (siz->ncomp > JPX_MAX_COMP) siz->ncomp = JPX_MAX_COMP; /* mas de 4: fuera de alcance, se trunca defensivamente */
    for (i = 0; i < siz->ncomp; i++)
    {
        unsigned char ssiz = seg[36 + i*3];
        siz->is_signed[i] = (ssiz & 0x80) ? 1 : 0;
        siz->prec[i] = (ssiz & 0x7F) + 1;
        siz->xr[i] = seg[36 + i*3 + 1];
        siz->yr[i] = seg[36 + i*3 + 2];
    }
}

static void jpx_parse_cod(const unsigned char *seg, cod_params *cod)
{
    unsigned char scod = seg[0];
    cod->prog_order = seg[1];
    cod->num_layers = (int)jpx_u16(seg + 2);
    cod->mct = seg[4];
    cod->num_decomp = seg[5];
    cod->cb_w = 1 << (seg[6] + 2);
    cod->cb_h = 1 << (seg[7] + 2);
    cod->transform = seg[9];
    (void)scod; /* bit 0 (precintos explicitos) y bit 6 (SOP/EPH) no se usan -- ver limitaciones en DESIGN.md */
}

static void jpx_parse_qcd(const unsigned char *seg, long seg_len, qcd_params *qcd)
{
    unsigned char sqcd = seg[0];
    int n, i;
    qcd->qstyle = sqcd & 0x1F;
    qcd->guard_bits = sqcd >> 5;
    n = (int)(seg_len - 1);
    if (qcd->qstyle == 0) /* sin cuantizacion: solo exponente, 1 byte c/u */
    {
        for (i = 0; i < n && i < 3*JPX_MAX_RES+1; i++)
        {
            qcd->exponent[i] = seg[1+i] >> 3;
            qcd->mantissa[i] = 0;
        }
    }
    else /* derivada (1) o expounded (2): exponente(5b)+mantisa(11b) por cada 2 bytes */
    {
        int cnt = n / 2;
        for (i = 0; i < cnt && i < 3*JPX_MAX_RES+1; i++)
        {
            int v = (int)jpx_u16(seg + 1 + i*2);
            qcd->exponent[i] = v >> 11;
            qcd->mantissa[i] = v & 0x7FF;
        }
    }
}

static void bitrd_align(bitrd *b)
{
    if (b->bitpos != 7)
    {
        b->prev_was_ff = (b->bytepos < b->len && b->data[b->bytepos] == 0xFF);
        b->bytepos++;
        b->bitpos = 7;
    }
}

/* indice del exponente/mantisa QCD para una subbanda dada (ver
 * comentario grande en DESIGN.md seccion 60 sobre el orden: LL
 * primero, despues por cada resolucion creciente HL,LH,HH). */
static int qcd_index(band_type band, int res)
{
    if (band == BAND_LL) return 0;
    return 1 + 3 * (res - 1) + (band == BAND_HL ? 0 : band == BAND_LH ? 1 : 2);
}

/* Decodifica UN tile-componente completo: geometria de resoluciones,
 * subbandas + arboles de tags, recorre los paquetes de ESTE
 * componente EN EL ORDEN QUE LE TOQUEN dentro de la progresion
 * general (el llamador ya itero resolucion/capa/precinto segun el
 * orden global -- aca solo se procesa la parte de un paquete que
 * corresponde a este (resolucion,capa) para este componente
 * especifico), decodifica cada code-block via Tier-1, dequantiza y
 * sintetiza con IDWT multi-nivel. Devuelve un buffer de doubles
 * width*height (tamanio del tile-componente completo). */

/* Debido a que el iterador de paquetes recorre RESOLUCION y CAPA por
 * fuera de un componente individual (la progresion RLCP/LRCP mezcla
 * resolucion/capa/componente en distinto orden), la estructura de
 * subbandas de CADA componente se crea una sola vez por tile (antes
 * de iterar paquetes) y se va llenando paquete a paquete a medida que
 * el iterador global los visita -- ver jpx_decode_tile mas abajo, que
 * arma el arreglo de componentes con sus resoluciones/subbandas ANTES
 * de recorrer paquetes, y esta funcion (jpx_component_synth) solo se
 * llama AL FINAL, una vez que todos los paquetes ya se procesaron. */
typedef struct
{
    jpx_res_geom geom;
    jpx_subband *ll0;                       /* subbanda LL de la resolucion 0 */
    jpx_subband *hl[JPX_MAX_RES], *lh[JPX_MAX_RES], *hh[JPX_MAX_RES]; /* 1..NL */
    jpx_rect tcomp;                          /* extent del tile-componente completo */
} jpx_tile_comp;

/* Vuelca los coeficientes YA decodificados (Tier-1) de una subbanda
 * hacia un plano denso width*height de doubles, dequantizando cada
 * coeficiente (T.800 Annex E: coef_real = coef_entero * 2^(exponente
 * efectivo) * step_mantisa, colapsado aca a un unico factor por
 * subbanda ya que la mantisa/exponente de QCD son fijos para toda la
 * subbanda). Para 5/3 reversible (sin perdida) 'step'=1 y NO se
 * agrega el punto medio del intervalo (+0.5) -- BUG REAL ENCONTRADO
 * (ver DESIGN.md seccion 60.2): el +0.5 solo tiene sentido cuando
 * hubo cuantizacion CON perdida (9/7 irreversible, para minimizar el
 * error esperado de redondeo) -- para 5/3 reversible los
 * coeficientes decodificados YA SON el valor entero exacto, sumarles
 * 0.5 los corrompe (confirmado con un caso de prueba minimo generado
 * con PIL/OpenJPEG: coeficientes que deberian salir enteros exactos
 * salian con ".50" pegado). */
static double *jpx_subband_to_plane(pdf_arena *arena, jpx_subband *sb, int cbw, int cbh,
                                     double step, int reversible)
{
    int w = rect_w(sb->ext), h = rect_h(sb->ext);
    double *plane;
    int cy, cx;

    plane = (double *)pdf_arena_alloc(arena, sizeof(double) * (size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1));
    if (plane == NULL) return NULL;
    memset(plane, 0, sizeof(double) * (size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1));
    if (w <= 0 || h <= 0) return plane;

    for (cy = 0; cy < sb->ncby; cy++)
    {
        for (cx = 0; cx < sb->ncbx; cx++)
        {
            cb_state *cbst = sb->cb[cy * sb->ncbx + cx];
            int bx0 = cx * cbw, by0 = cy * cbh;
            int bw, bh, x, y;
            if (cbst == NULL) continue;
            bw = cbst->w; bh = cbst->h;
            for (y = 0; y < bh; y++)
            {
                for (x = 0; x < bw; x++)
                {
                    int gx = bx0 + x, gy = by0 + y;
                    unsigned int mag;
                    double val;
                    if (gx >= w || gy >= h) continue;
                    mag = cbst->mag[CB_IDX(cbst, x, y)];
                    if (mag == 0) continue;
                    /* T.800 D.2: el valor reconstruido usa el punto
                     * medio del intervalo de cuantizacion (mag +
                     * 0.5*ULP) para minimizar el error esperado --
                     * SOLO aplica al caso con perdida (9/7). */
                    val = reversible ? (double)mag * step : ((double)mag + 0.5) * step;
                    if (cb_sign(cbst, x, y)) val = -val;
                    plane[gy * w + gx] = val;
                }
            }
        }
    }
    return plane;
}

/* Reconstruye la imagen espacial (double, width*height) de UN
 * componente de UN tile, ya con todas las subbandas decodificadas
 * (Tier-1) y listas en 'tc'. */
/* T.800 Eq E-2: "rango dinamico nominal" Rb de una subbanda, usado
 * SOLO para el tamanio de paso de cuantizacion (Delta_b, Eq E-3) --
 * NO es lo mismo que 'mb' (guard_bits+exponente-1, el numero de
 * PLANOS DE BITS que usa Tier-1 para saber donde arrancar a
 * decodificar, ver cb_decode). Rb = precision de la imagen + una
 * "ganancia" fija segun la orientacion de la subbanda (0 bits para
 * LL, 1 para HL/LH, 2 para HH -- T.800 Tabla E.1, refleja que un
 * coeficiente HH de magnitud 1 representa mas energia real que uno
 * LL de la misma magnitud, por como se acumula el filtro wavelet).
 *
 * BUG REAL ENCONTRADO (confirmado contra Conveyor_Handbook.pdf,
 * imagenes JPX reales -- ver DESIGN.md, ronda "JPX real"): la version
 * anterior usaba 'mb' en el lugar de Rb en la formula del paso de
 * cuantizacion (pow(2, mb-exponente)) -- como 'mb' YA incluye ese
 * mismo exponente (mb = guard_bits+exponente-1), el exponente se
 * CANCELABA matematicamente (mb-exponente = guard_bits-1, una
 * constante identica para TODAS las subbandas/resoluciones, sin
 * importar el exponente real que declaraba QCD para cada una) -- el
 * resultado: los coeficientes de detalle (HL/LH/HH) en TODAS las
 * resoluciones se dequantizaban con practicamente el mismo paso chico
 * que la banda LL, sobre-amplificando brutalmente el detalle de alta
 * frecuencia en cada nivel de sintesis wavelet -- exactamente el
 * patron de "tablero de ajedrez" cada vez peor que se observaba
 * visualmente (ver PDF_JPX_DUMP_LL) al agregar cada resolucion. */
static double jpx_nominal_range(band_type band, int precision)
{
    int gain;
    switch (band)
    {
        case BAND_HL: case BAND_LH: gain = 1; break;
        case BAND_HH:               gain = 2; break;
        default:                    gain = 0; break; /* LL */
    }
    return (double)(precision + gain);
}

static double *jpx_component_synth(pdf_arena *arena, jpx_tile_comp *tc,
                                    qcd_params *qcd, int cbw, int cbh,
                                    int transform_53, int precision, int *out_w, int *out_h)
{
    double *cur;
    int cur_w, cur_h, r;
    double step;

    /* LL de resolucion 0 */
    if (transform_53)
        step = 1.0; /* reversible: coeficientes ya enteros exactos, sin escalar */
    else
    {
        int e = qcd->exponent[qcd_index(BAND_LL, 0)];
        int m = qcd->mantissa[qcd_index(BAND_LL, 0)];
        step = pow(2.0, jpx_nominal_range(BAND_LL, precision) - e) * (1.0 + (double)m / 2048.0);
    }
    cur = jpx_subband_to_plane(arena, tc->ll0, cbw, cbh, step, transform_53);
    cur_w = rect_w(tc->ll0->ext);
    cur_h = rect_h(tc->ll0->ext);

    /* PDF_JPX_DUMP_LL=<ruta base> (variable de entorno de diagnostico,
     * ver PDF_JPX_DEBUG mas abajo para el mismo criterio): vuelca la
     * banda LL0 (y, mas abajo en el bucle de sintesis, el resultado
     * parcial despues de cada nivel de reconstruccion) como PGM
     * (escala de grises, normalizado min-max por imagen) a
     * "<ruta base>.<contador>.pgm" -- diagnostico agregado durante la
     * investigacion de por que la salida final sale como ruido
     * (ver DESIGN.md, ronda "JPX real"): permite ver EN QUE ETAPA la
     * imagen deja de tener estructura reconocible. Confirmado con
     * Conveyor_Handbook.pdf: la banda LL0 sola (una imagen borrosa
     * ~8x2) ya tiene algo de estructura (no es ruido puro), pero el
     * patron se vuelve un "tablero de ajedrez" cada vez mas marcado a
     * partir del primer nivel de sintesis wavelet (al incorporar
     * HL/LH/HH) -- descartado que sea una inversion de signo global o
     * un intercambio HL<->LH (ver PDF_JPX_FLIP_SIGN/PDF_JPX_SWAP_HL_LH
     * en el historial de esta investigacion, ninguno de los dos
     * arreglo el patron) -- causa raiz AUN NO IDENTIFICADA. */
    if (getenv("PDF_JPX_DUMP_LL"))
    {
        static int dump_ll_counter = 0;
        char fname[256];
        FILE *pf;
        sprintf(fname, "%s.%d.pgm", getenv("PDF_JPX_DUMP_LL"), dump_ll_counter++);
        pf = fopen(fname, "wb");
        if (pf != NULL)
        {
            int yy, xx;
            double mn = 1e300, mx = -1e300;
            for (yy = 0; yy < cur_w * cur_h; yy++)
            {
                if (cur[yy] < mn) mn = cur[yy];
                if (cur[yy] > mx) mx = cur[yy];
            }
            fprintf(pf, "P5\n%d %d\n255\n", cur_w, cur_h);
            for (yy = 0; yy < cur_h; yy++)
                for (xx = 0; xx < cur_w; xx++)
                {
                    double v = cur[yy * cur_w + xx];
                    unsigned char b = (mx > mn) ? (unsigned char)(255.0 * (v - mn) / (mx - mn) + 0.5) : 128;
                    fputc(b, pf);
                }
            fclose(pf);
            fprintf(stderr, "DUMP_LL: w=%d h=%d min=%.3f max=%.3f -> %s\n", cur_w, cur_h, mn, mx, fname);
        }
    }

    if (jpx_debug_on())
    {
        cb_state *cbst = tc->ll0->cb[0];
        fprintf(stderr, "DEBUG LL0: ext=(%d,%d)-(%d,%d) w=%d h=%d step=%.6f ncbx=%d ncby=%d\n",
                tc->ll0->ext.x0, tc->ll0->ext.y0, tc->ll0->ext.x1, tc->ll0->ext.y1,
                cur_w, cur_h, step, tc->ll0->ncbx, tc->ll0->ncby);
        if (cbst != NULL)
        {
            int xx;
            fprintf(stderr, "DEBUG LL0 cb[0]: w=%d h=%d, primeras magnitudes (fila 0):\n  ", cbst->w, cbst->h);
            for (xx = 0; xx < cbst->w && xx < 12; xx++)
                fprintf(stderr, "%u(%c) ", cbst->mag[CB_IDX(cbst,xx,0)], cb_sign(cbst,xx,0)?'-':'+');
            fprintf(stderr, "\n  fila 1: ");
            for (xx = 0; xx < cbst->w && xx < 12; xx++)
                fprintf(stderr, "%u(%c) ", cbst->mag[CB_IDX(cbst,xx,1)], cb_sign(cbst,xx,1)?'-':'+');
            fprintf(stderr, "\n");
        }
        else
        {
            fprintf(stderr, "DEBUG LL0 cb[0] es NULL (nunca se creo -- no llegaron datos de paquete)\n");
        }
        fprintf(stderr, "DEBUG plano LL despues de dequantizar, primeros valores: ");
        {
            int i;
            for (i = 0; i < cur_w && i < 12; i++) fprintf(stderr, "%.2f ", cur[i]);
        }
        fprintf(stderr, "\n");
    }

    for (r = 1; r < tc->geom.nres; r++)
    {
        double *hl_p, *lh_p, *hh_p, *next;
        int hlw, lhw, lhh, nw, nh;
        idwt_1d_fn fn = transform_53 ? idwt_53_1d : idwt_97_1d;

        if (transform_53) step = 1.0;
        else
        {
            int eh = qcd->exponent[qcd_index(BAND_HL, r)];
            int mh = qcd->mantissa[qcd_index(BAND_HL, r)];
            step = pow(2.0, jpx_nominal_range(BAND_HL, precision) - eh) * (1.0 + (double)mh / 2048.0);
        }
        hl_p = jpx_subband_to_plane(arena, tc->hl[r], cbw, cbh, step, transform_53);
        hlw = rect_w(tc->hl[r]->ext); /* hl height == ll height (comparten banda vertical "Low"), no hace falta calcularla aparte */

        if (!transform_53)
        {
            int el = qcd->exponent[qcd_index(BAND_LH, r)];
            int ml = qcd->mantissa[qcd_index(BAND_LH, r)];
            step = pow(2.0, jpx_nominal_range(BAND_LH, precision) - el) * (1.0 + (double)ml / 2048.0);
        }
        lh_p = jpx_subband_to_plane(arena, tc->lh[r], cbw, cbh, step, transform_53);
        lhw = rect_w(tc->lh[r]->ext); lhh = rect_h(tc->lh[r]->ext);

        if (!transform_53)
        {
            int eh2 = qcd->exponent[qcd_index(BAND_HH, r)];
            int mh2 = qcd->mantissa[qcd_index(BAND_HH, r)];
            step = pow(2.0, jpx_nominal_range(BAND_HH, precision) - eh2) * (1.0 + (double)mh2 / 2048.0);
        }
        hh_p = jpx_subband_to_plane(arena, tc->hh[r], cbw, cbh, step, transform_53);

        next = idwt_synth_level(arena, fn, cur, hl_p, lh_p, hh_p,
                                 cur_w, cur_h, hlw, lhh, &nw, &nh);
        cur = next; cur_w = nw; cur_h = nh;
        (void)lhw;

        if (getenv("PDF_JPX_DUMP_LL"))
        {
            static int dump_lvl_counter = 0;
            char fname[256];
            FILE *pf;
            sprintf(fname, "%s.lvl%d.%d.pgm", getenv("PDF_JPX_DUMP_LL"), r, dump_lvl_counter++);
            pf = fopen(fname, "wb");
            if (pf != NULL)
            {
                int yy, xx;
                double mn = 1e300, mx = -1e300;
                for (yy = 0; yy < cur_w * cur_h; yy++)
                {
                    if (cur[yy] < mn) mn = cur[yy];
                    if (cur[yy] > mx) mx = cur[yy];
                }
                fprintf(pf, "P5\n%d %d\n255\n", cur_w, cur_h);
                for (yy = 0; yy < cur_h; yy++)
                    for (xx = 0; xx < cur_w; xx++)
                    {
                        double v = cur[yy * cur_w + xx];
                        unsigned char b = (mx > mn) ? (unsigned char)(255.0 * (v - mn) / (mx - mn) + 0.5) : 128;
                        fputc(b, pf);
                    }
                fclose(pf);
                fprintf(stderr, "DUMP_LVL%d: w=%d h=%d min=%.3f max=%.3f -> %s\n", r, cur_w, cur_h, mn, mx, fname);
            }
        }
    }

    *out_w = cur_w; *out_h = cur_h;
    return cur;
}

/* --- decodificacion de un tile completo --------------------------------- */

/* Decodifica los code-blocks de UNA subbanda que ya juntaron todos sus
 * rangos de bytes (ver comentario grande en cb_state) -- se llama
 * DESPUES de terminar el recorrido completo de paquetes (todas las
 * capas), nunca durante. Copia los rangos (potencialmente salteados
 * dentro de tile_data, uno por capa que le aporto algo a ese
 * code-block) a un buffer chico contiguo en la arena y recien ahi
 * llama cb_decode() -- UNA sola vez por code-block, con el
 * total_passes YA final. */
static int jpx_decode_pending_subband(pdf_arena *arena, jpx_subband *sb,
                                       const unsigned char *tile_data)
{
    int idx, n;

    if (sb == NULL) return PDF_OK;

    n = sb->ncbx * sb->ncby;
    for (idx = 0; idx < n; idx++)
    {
        cb_state *cbs = sb->cb[idx];
        unsigned char *buf;
        int total_len, r, off;
        int msb_bp;

        if (cbs == NULL || cbs->n_ranges == 0) continue;

        total_len = 0;
        for (r = 0; r < cbs->n_ranges; r++) total_len += cbs->range_len[r];
        if (total_len <= 0) continue;

        buf = (unsigned char *)pdf_arena_alloc(arena, (size_t)total_len);
        if (buf == NULL) return PDF_ERR_NOMEM;

        off = 0;
        for (r = 0; r < cbs->n_ranges; r++)
        {
            memcpy(buf + off, tile_data + cbs->range_off[r], (size_t)cbs->range_len[r]);
            off += cbs->range_len[r];
        }

        msb_bp = sb->mb - 1 - sb->zero_planes[idx];
        if (jpx_debug_on())
            fprintf(stderr, "DEBUG cb_decode(final) idx=%d n_ranges=%d total_len=%d total_passes=%d msb_bp=%d\n",
                    idx, cbs->n_ranges, total_len, sb->total_passes[idx], msb_bp);
        cb_decode(cbs, buf, total_len, sb->total_passes[idx], msb_bp);
    }
    return PDF_OK;
}

/* Procesa UN paquete (una combinacion (layer,res,comp) puntual) --
 * extraido tal cual de jpx_decode_tile (sin cambios de logica) para
 * poder llamarlo con el orden de bucles que corresponda segun
 * cod->prog_order (LRCP vs RLCP, ver comentario grande en el
 * llamador). Avanza 'br' in-place. */
static void jpx_process_one_packet(pdf_arena *arena, bitrd *br, jpx_tile_comp *tc,
                                    const cod_params *cod, long tile_len,
                                    int layer, int res, int comp)
{
    jpx_subband *bands[3];
    int nbands;
    int cbband[256], cbidx[256], cblen[256];
    int n, k;

    if (res == 0) { bands[0] = tc[comp].ll0; nbands = 1; }
    else { bands[0] = tc[comp].hl[res]; bands[1] = tc[comp].lh[res]; bands[2] = tc[comp].hh[res]; nbands = 3; }

    n = jpx_packet_header(br, bands, nbands, layer, cbband, cbidx, cblen, 256);
    bitrd_align(br);
    if (jpx_debug_on())
        fprintf(stderr, "DEBUG paquete layer=%d res=%d comp=%d nbands=%d n_incluidos=%d bytepos=%ld\n",
                layer, res, comp, nbands, n, br->bytepos);

    for (k = 0; k < n; k++)
    {
        jpx_subband *sb = bands[cbband[k]];
        int idx = cbidx[k];
        long avail = tile_len - br->bytepos;
        long take = cblen[k];
        if (take > avail) take = avail; /* defensivo: datos truncados */
        if (jpx_debug_on())
            fprintf(stderr, "  cb band=%d idx=%d len=%d zero_planes=%d total_passes=%d mb=%d\n",
                    cbband[k], idx, cblen[k], sb->zero_planes[idx], sb->total_passes[idx], sb->mb);
        if (take > 0)
        {
            /* los bytes del cuerpo se agregan directo al
             * code-block (creandolo si es la primera vez) */
            if (sb->cb[idx] == NULL)
            {
                int bx = (idx % sb->ncbx) * cod->cb_w;
                int by = (idx / sb->ncbx) * cod->cb_h;
                int bw = rect_w(sb->ext) - bx;
                int bh = rect_h(sb->ext) - by;
                if (bw > cod->cb_w) bw = cod->cb_w;
                if (bh > cod->cb_h) bh = cod->cb_h;
                if (bw > 0 && bh > 0)
                    sb->cb[idx] = cb_state_create(arena, bw, bh, sb->band, cod->num_layers);
            }
            /* NO se decodifica aca -- ver comentario grande en cb_state
             * (mas arriba): las capas de un code-block son UN SOLO
             * stream MQ continuo, asi que solo se puede decodificar
             * bien cuando ya se juntaron TODOS los bytes de TODAS las
             * capas que le tocaron. Por ahora solo se registra donde
             * estan estos bytes dentro de tile_data -- el decode real
             * pasa en jpx_decode_pending_subband(), una vez terminado
             * TODO el recorrido de paquetes. */
            if (sb->cb[idx] != NULL && sb->cb[idx]->n_ranges < sb->cb[idx]->max_ranges)
            {
                cb_state *cbs = sb->cb[idx];
                cbs->range_off[cbs->n_ranges] = br->bytepos;
                cbs->range_len[cbs->n_ranges] = (int)take;
                cbs->n_ranges++;
            }
        }
        br->bytepos += take;
        br->bitpos = 7;
        br->prev_was_ff = 0;
    }
}

/* Decodifica todos los paquetes de un tile (los datos entre SOD y el
 * proximo SOT/EOC) siguiendo el orden de progresion indicado, y
 * dispara Tier-1 + sintesis para cada componente. 'comp_planes' (ya
 * alocado por el llamador, ncomp punteros) recibe el resultado de
 * cada componente (double*, tile_w x tile_h EN COORDENADAS DE TILE,
 * el llamador ya sabe el ancho/alto porque son iguales a tcomp para
 * xr=yr=1 -- unico caso soportado, ver limitaciones). */
static int jpx_decode_tile(pdf_arena *arena, const unsigned char *tile_data, long tile_len,
                            jpx_rect tile_rect, const siz_params *siz, const cod_params *cod,
                            const qcd_params *qcd, double **comp_planes, int *comp_w, int *comp_h)
{
    jpx_tile_comp tc[JPX_MAX_COMP];
    int c, r;
    bitrd br;

    if (cod->prog_order != 0 && cod->prog_order != 1)
        return PDF_ERR_UNSUPPORTED; /* solo LRCP y RLCP -- ver limitaciones DESIGN.md seccion 60 */

    /* --- armar geometria + subbandas de cada componente, ANTES de
     * recorrer paquetes (los paquetes solo APORTAN DATOS a code-blocks
     * ya existentes, no cambian la geometria). --------------------- */
    for (c = 0; c < siz->ncomp; c++)
    {
        tc[c].tcomp = tile_rect; /* xr=yr=1 siempre en el caso soportado */
        jpx_compute_geom(tc[c].tcomp, cod->num_decomp, &tc[c].geom);

        {
            int mb0 = qcd->guard_bits + qcd->exponent[qcd_index(BAND_LL, 0)] - 1;
            tc[c].ll0 = jpx_subband_create(arena, tc[c].geom.ll[0], cod->cb_w, cod->cb_h, BAND_LL, mb0);
            if (tc[c].ll0 == NULL) return PDF_ERR_NOMEM;
        }
        for (r = 1; r < tc[c].geom.nres; r++)
        {
            int mbh = qcd->guard_bits + qcd->exponent[qcd_index(BAND_HL, r)] - 1;
            int mbl = qcd->guard_bits + qcd->exponent[qcd_index(BAND_LH, r)] - 1;
            int mbhh = qcd->guard_bits + qcd->exponent[qcd_index(BAND_HH, r)] - 1;
            tc[c].hl[r] = jpx_subband_create(arena, tc[c].geom.hl[r], cod->cb_w, cod->cb_h, BAND_HL, mbh);
            tc[c].lh[r] = jpx_subband_create(arena, tc[c].geom.lh[r], cod->cb_w, cod->cb_h, BAND_LH, mbl);
            tc[c].hh[r] = jpx_subband_create(arena, tc[c].geom.hh[r], cod->cb_w, cod->cb_h, BAND_HH, mbhh);
            if (tc[c].hl[r] == NULL || tc[c].lh[r] == NULL || tc[c].hh[r] == NULL)
                return PDF_ERR_NOMEM;
        }
    }

    /* --- recorrer paquetes segun el orden de progresion -------------
     * BUG REAL ENCONTRADO Y ARREGLADO (mismo PDF de 5 capas de arriba):
     * el recorrido estaba SIEMPRE codificado como layer-afuera,
     * resolucion-en-medio, componente-adentro (orden LRCP) sin importar
     * lo que dijera cod->prog_order -- con 1 sola capa da lo mismo
     * (intercambiar dos bucles cuando uno mide 1 no cambia nada, por
     * eso el caso de 1 capa SIEMPRE funciono bien), pero con RLCP
     * (prog_order==1, el caso real de este archivo) el orden real de
     * los paquetes en el archivo es resolucion-afuera, capa-en-medio,
     * componente-adentro -- leerlos como si fueran LRCP le asigna los
     * bytes de cada paquete a la combinacion (layer,res,comp)
     * EQUIVOCADA. jpx_process_one_packet() -- el cuerpo de "procesar
     * UN paquete", sin cambios de logica, solo movido a funcion aparte
     * -- ahora se llama con el orden de bucles que corresponda. */
    bitrd_init(&br, tile_data, tile_len);

    {
        clock_t t_packets0, t_packets1, t_tier1_1;
        t_packets0 = clock();

    if (cod->prog_order == 0) /* LRCP */
    {
        int layer, res, comp;
        for (layer = 0; layer < cod->num_layers; layer++)
            for (res = 0; res < tc[0].geom.nres; res++)
                for (comp = 0; comp < siz->ncomp; comp++)
                    jpx_process_one_packet(arena, &br, tc, cod, tile_len, layer, res, comp);
    }
    else /* RLCP (prog_order == 1, el unico otro caso aceptado arriba) */
    {
        int layer, res, comp;
        for (res = 0; res < tc[0].geom.nres; res++)
            for (layer = 0; layer < cod->num_layers; layer++)
                for (comp = 0; comp < siz->ncomp; comp++)
                    jpx_process_one_packet(arena, &br, tc, cod, tile_len, layer, res, comp);
    }
    t_packets1 = clock();

    /* --- decodificar (Tier-1) los code-blocks que quedaron pendientes,
     * ahora que ya se junto TODO lo de TODAS las capas -- ver comentario
     * grande en cb_state/jpx_decode_pending_subband. Tiene que pasar
     * ANTES de la sintesis de abajo (que lee sb->cb[idx] ya decodificado
     * via jpx_component_synth). */
    for (c = 0; c < siz->ncomp; c++)
    {
        int r2;
        int rc = jpx_decode_pending_subband(arena, tc[c].ll0, tile_data);
        if (rc != PDF_OK) return rc;
        for (r2 = 1; r2 < tc[c].geom.nres; r2++)
        {
            rc = jpx_decode_pending_subband(arena, tc[c].hl[r2], tile_data);
            if (rc != PDF_OK) return rc;
            rc = jpx_decode_pending_subband(arena, tc[c].lh[r2], tile_data);
            if (rc != PDF_OK) return rc;
            rc = jpx_decode_pending_subband(arena, tc[c].hh[r2], tile_data);
            if (rc != PDF_OK) return rc;
        }
    }
    t_tier1_1 = clock();

    if (getenv("PDF_JPX_TIMING"))
        fprintf(stderr, "TIMING tile: paquetes=%.3fs tier1=%.3fs\n",
                (double)(t_packets1 - t_packets0) / CLOCKS_PER_SEC,
                (double)(t_tier1_1 - t_packets1) / CLOCKS_PER_SEC);
    }

    /* --- sintesis final por componente ------------------------------ */
    {
        clock_t t0 = clock();
        for (c = 0; c < siz->ncomp; c++)
        {
            comp_planes[c] = jpx_component_synth(arena, &tc[c], (qcd_params *)qcd,
                                                  cod->cb_w, cod->cb_h,
                                                  cod->transform == 1, siz->prec[c], &comp_w[c], &comp_h[c]);
            if (comp_planes[c] == NULL) return PDF_ERR_NOMEM;
        }
        if (getenv("PDF_JPX_TIMING"))
            fprintf(stderr, "TIMING tile: sintesis=%.3fs\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    }

    return PDF_OK;
}

/* --- driver de mas alto nivel: JP2/J2K completo -------------------------- */

int pdf_filter_jpx(pdf_arena *arena, const unsigned char *src, long src_len, pdf_jpx_image *out)
{
    const unsigned char *cs;
    long cs_len, pos;
    siz_params siz;
    cod_params cod;
    qcd_params qcd;
    int have_siz = 0, have_cod = 0, have_qcd = 0;
    double **all_comp_planes; /* [tile][comp] aplanado luego a la imagen final */
    int ntx, nty;

    out->width = out->height = 0;
    out->rgb = NULL;

    if (jpx_find_codestream(src, src_len, &cs, &cs_len) != PDF_OK)
        return PDF_ERR_BADARG;

    if (cs_len < 4 || cs[0] != 0xFF || cs[1] != 0x4F) /* SOC */
        return PDF_ERR_BADARG;
    pos = 2;

    /* --- header principal: SIZ, COD, QCD (se ignoran COC/QCC -- se
     * asume que el COD/QCD principal aplica a todos los componentes,
     * el caso comun -- y tambien POC/CRG/COM). Se detiene en el
     * primer SOT. ------------------------------------------------- */
    while (pos + 4 <= cs_len)
    {
        int marker;
        long seg_len;
        if (cs[pos] != 0xFF) { pos++; continue; }
        marker = cs[pos+1];
        if (marker == 0x90) break; /* SOT: fin del header principal */
        if (marker == 0xD9) return PDF_ERR_BADARG; /* EOC sin tiles */
        pos += 2;
        if (pos + 2 > cs_len) return PDF_ERR_BADARG;
        seg_len = jpx_u16(cs + pos);
        if (seg_len < 2 || pos + seg_len > cs_len) return PDF_ERR_BADARG;

        if (marker == 0x51) { jpx_parse_siz(cs + pos + 2, &siz); have_siz = 1; }
        else if (marker == 0x52) { jpx_parse_cod(cs + pos + 2, &cod); have_cod = 1; }
        else if (marker == 0x5C) { jpx_parse_qcd(cs + pos + 2, seg_len - 2, &qcd); have_qcd = 1; }
        else if (marker == 0x52 + 1) { /* COC: no soportado, ver limitaciones */ }

        if (jpx_debug_on())
            fprintf(stderr, "DEBUG marker FF%02X len=%ld pos=%ld\n", marker, seg_len, pos);
        pos += seg_len;
    }
    if (jpx_debug_on())
        fprintf(stderr, "DEBUG post-loop: have_siz=%d have_cod=%d have_qcd=%d pos=%ld\n", have_siz, have_cod, have_qcd, pos);
    if (!have_siz || !have_cod || !have_qcd)
        return PDF_ERR_UNSUPPORTED;
    if (siz.ncomp < 1 || siz.ncomp > 4)
        return PDF_ERR_UNSUPPORTED;
    {
        int c;
        for (c = 0; c < siz.ncomp; c++)
            if (siz.xr[c] != 1 || siz.yr[c] != 1)
                return PDF_ERR_UNSUPPORTED; /* submuestreo de croma: fuera de alcance */
    }
    if (siz.Xsiz <= 0 || siz.Ysiz <= 0 || siz.Xsiz > 20000 || siz.Ysiz > 20000)
        return PDF_ERR_BADARG;
    if (jpx_debug_on())
        fprintf(stderr, "DEBUG SIZ: X=%ld Y=%ld ncomp=%d COD: prog=%d layers=%d mct=%d decomp=%d cbw=%d cbh=%d transform=%d QCD: style=%d guard=%d\n",
                siz.Xsiz, siz.Ysiz, siz.ncomp, cod.prog_order, cod.num_layers, cod.mct, cod.num_decomp,
                cod.cb_w, cod.cb_h, cod.transform, qcd.qstyle, qcd.guard_bits);

    ntx = (int)((siz.Xsiz - siz.XTOsiz + siz.XTsiz - 1) / siz.XTsiz);
    nty = (int)((siz.Ysiz - siz.YTOsiz + siz.YTsiz - 1) / siz.YTsiz);
    if (ntx < 1 || nty < 1 || (long)ntx * nty > 4096)
        return PDF_ERR_UNSUPPORTED; /* limite defensivo */

    out->rgb = (unsigned char *)pdf_arena_alloc(arena, (size_t)siz.Xsiz * (size_t)siz.Ysiz * 3);
    if (out->rgb == NULL) return PDF_ERR_NOMEM;
    memset(out->rgb, 0, (size_t)siz.Xsiz * (size_t)siz.Ysiz * 3);
    out->width = (int)siz.Xsiz;
    out->height = (int)siz.Ysiz;

    all_comp_planes = (double **)pdf_arena_alloc(arena, sizeof(double *) * (size_t)siz.ncomp);
    if (all_comp_planes == NULL) return PDF_ERR_NOMEM;

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 60): un tile puede
     * estar partido en VARIOS "tile-parts" (SOT con distinto TPsot,
     * mismo Isot) -- confirmado con datos reales
     * (Conveyor_Handbook.pdf): la imagen de 2 tiles en realidad tenia
     * 12 tile-parts, 6 por cada tile, alternados (Isot=0,1,0,1,...).
     * La primera version de este driver trataba CADA tile-part como
     * si fuera un tile completo independiente, re-arrancando Tier-2
     * desde cero con datos truncados -- el resultado decodificaba
     * "algo" (no crasheaba) pero con basura casi en todos lados,
     * salvo quizas el primerisimo pedacito que por casualidad cabia
     * en el primer tile-part. La solucion real: los tile-parts de un
     * mismo Isot deben CONCATENARSE (sus datos de body, en el orden
     * en que aparecen -- el estandar exige que aparezcan en orden de
     * TPsot creciente) en un solo buffer contiguo ANTES de correr
     * Tier-2, ya que los paquetes de un tile pueden quedar repartidos
     * entre tile-parts sucesivos sin ningun alineamiento especial.
     *
     * Implementado en dos pasadas sobre el codestream: la primera
     * solo mide cuanto le corresponde a cada Isot (sumando el largo
     * de cada tile-part que le pertenece); la segunda reserva UN
     * buffer por tile del tamanio exacto y copia cada tile-part a su
     * lugar. Recien con eso armado se decodifica cada tile completo
     * (una sola vez). */
    {
        int ntiles = ntx * nty;
        long *tile_total_len;
        long *tile_written;
        unsigned char **tile_buf;
        long scan_pos;
        int ti;

        if (ntiles < 1) ntiles = 1;
        tile_total_len = (long *)pdf_arena_alloc(arena, sizeof(long) * (size_t)ntiles);
        tile_written    = (long *)pdf_arena_alloc(arena, sizeof(long) * (size_t)ntiles);
        tile_buf         = (unsigned char **)pdf_arena_alloc(arena, sizeof(unsigned char *) * (size_t)ntiles);
        if (tile_total_len == NULL || tile_written == NULL || tile_buf == NULL)
            return PDF_ERR_NOMEM;
        for (ti = 0; ti < ntiles; ti++) { tile_total_len[ti] = 0; tile_written[ti] = 0; tile_buf[ti] = NULL; }

        /* --- pasada 1: medir --------------------------------------- */
        scan_pos = pos;
        while (scan_pos + 4 <= cs_len)
        {
            long sot_marker_pos, seg_len, sot_len, tidx, tpart_len, tpart_start, body_len;
            int marker;

            if (cs[scan_pos] != 0xFF) { scan_pos++; continue; }
            marker = cs[scan_pos+1];
            if (marker == 0xD9) break;
            if (marker != 0x90)
            {
                scan_pos += 2;
                if (scan_pos + 2 > cs_len) break;
                seg_len = jpx_u16(cs + scan_pos);
                scan_pos += seg_len;
                continue;
            }
            sot_marker_pos = scan_pos;
            scan_pos += 2;
            seg_len = jpx_u16(cs + scan_pos);
            sot_len = seg_len;
            tidx = jpx_u16(cs + scan_pos + 2);
            tpart_len = jpx_u32(cs + scan_pos + 4);
            scan_pos += sot_len;
            while (scan_pos + 2 <= cs_len && !(cs[scan_pos] == 0xFF && cs[scan_pos+1] == 0x93))
            {
                if (cs[scan_pos] != 0xFF) { scan_pos++; continue; }
                if (cs[scan_pos+1] == 0x90 || cs[scan_pos+1] == 0xD9) break;
                { long sl = jpx_u16(cs + scan_pos + 2); scan_pos += 2 + sl; }
            }
            if (scan_pos + 2 > cs_len || cs[scan_pos] != 0xFF || cs[scan_pos+1] != 0x93) break;
            scan_pos += 2;
            tpart_start = scan_pos;
            body_len = (tpart_len > 0) ? (tpart_len - (tpart_start - sot_marker_pos)) : (cs_len - tpart_start);
            if (body_len < 0) body_len = 0;
            if (tpart_start + body_len > cs_len) body_len = cs_len - tpart_start;
            if (tidx >= 0 && tidx < ntiles) tile_total_len[tidx] += body_len;
            scan_pos = tpart_start + body_len;
        }

        for (ti = 0; ti < ntiles; ti++)
        {
            if (tile_total_len[ti] <= 0) continue;
            tile_buf[ti] = (unsigned char *)pdf_arena_alloc(arena, (size_t)tile_total_len[ti]);
            if (tile_buf[ti] == NULL) return PDF_ERR_NOMEM;
        }

        /* --- pasada 2: concatenar ------------------------------------ */
        scan_pos = pos;
        while (scan_pos + 4 <= cs_len)
        {
            long sot_marker_pos, seg_len, sot_len, tidx, tpart_len, tpart_start, body_len;
            int marker;

            if (cs[scan_pos] != 0xFF) { scan_pos++; continue; }
            marker = cs[scan_pos+1];
            if (marker == 0xD9) break;
            if (marker != 0x90)
            {
                scan_pos += 2;
                if (scan_pos + 2 > cs_len) break;
                seg_len = jpx_u16(cs + scan_pos);
                scan_pos += seg_len;
                continue;
            }
            sot_marker_pos = scan_pos;
            scan_pos += 2;
            seg_len = jpx_u16(cs + scan_pos);
            sot_len = seg_len;
            tidx = jpx_u16(cs + scan_pos + 2);
            tpart_len = jpx_u32(cs + scan_pos + 4);
            scan_pos += sot_len;
            while (scan_pos + 2 <= cs_len && !(cs[scan_pos] == 0xFF && cs[scan_pos+1] == 0x93))
            {
                if (cs[scan_pos] != 0xFF) { scan_pos++; continue; }
                if (cs[scan_pos+1] == 0x90 || cs[scan_pos+1] == 0xD9) break;
                { long sl = jpx_u16(cs + scan_pos + 2); scan_pos += 2 + sl; }
            }
            if (scan_pos + 2 > cs_len || cs[scan_pos] != 0xFF || cs[scan_pos+1] != 0x93) break;
            scan_pos += 2;
            tpart_start = scan_pos;
            body_len = (tpart_len > 0) ? (tpart_len - (tpart_start - sot_marker_pos)) : (cs_len - tpart_start);
            if (body_len < 0) body_len = 0;
            if (tpart_start + body_len > cs_len) body_len = cs_len - tpart_start;
            if (tidx >= 0 && tidx < ntiles && tile_buf[tidx] != NULL)
            {
                memcpy(tile_buf[tidx] + tile_written[tidx], cs + tpart_start, (size_t)body_len);
                tile_written[tidx] += body_len;
            }
            scan_pos = tpart_start + body_len;
        }

        /* --- decodificar cada tile UNA vez, con su buffer ya completo --- */
        for (ti = 0; ti < ntiles; ti++)
        {
            int tile_x, tile_y;
            jpx_rect trect;
            int cws[JPX_MAX_COMP], chs[JPX_MAX_COMP];
            int rc, c;

            if (tile_buf[ti] == NULL) continue;

            tile_x = ti % ntx;
            tile_y = ti / ntx;
            {
                long rx0 = siz.XTOsiz + (long)tile_x * siz.XTsiz;
                long ry0 = siz.YTOsiz + (long)tile_y * siz.YTsiz;
                long rx1 = siz.XTOsiz + (long)(tile_x+1) * siz.XTsiz;
                long ry1 = siz.YTOsiz + (long)(tile_y+1) * siz.YTsiz;
                if (rx0 < siz.XOsiz) rx0 = siz.XOsiz;
                if (ry0 < siz.YOsiz) ry0 = siz.YOsiz;
                if (rx1 > siz.Xsiz) rx1 = siz.Xsiz;
                if (ry1 > siz.Ysiz) ry1 = siz.Ysiz;
                trect.x0 = (int)rx0; trect.y0 = (int)ry0; trect.x1 = (int)rx1; trect.y1 = (int)ry1;
            }

            rc = jpx_decode_tile(arena, tile_buf[ti], tile_written[ti], trect, &siz, &cod, &qcd,
                                  all_comp_planes, cws, chs);
            if (rc != PDF_OK) return rc;

            {
                int tw = rect_w(trect), th = rect_h(trect);
                int px, py;
                int shift0 = 1 << (siz.prec[0] - 1);
                int maxval0 = (1 << siz.prec[0]) - 1;
                double scale = 255.0 / maxval0;

                for (py = 0; py < th; py++)
                {
                    for (px = 0; px < tw; px++)
                    {
                        double v0 = all_comp_planes[0][py * cws[0] + px];
                        double r, g, b;

                        if (jpx_only_v0_on())
                        {
                            r = g = b = v0;
                        }
                        else if (siz.ncomp >= 3 && cod.mct)
                        {
                            double v1 = all_comp_planes[1][py * cws[1] + px];
                            double v2 = all_comp_planes[2][py * cws[2] + px];
                            if (cod.transform == 1)
                            {
                                double gg = v0 - floor((v1 + v2) / 4.0);
                                r = v2 + gg;
                                b = v1 + gg;
                                g = gg;
                            }
                            else
                            {
                                r = v0 + 1.402 * v2;
                                g = v0 - 0.344136 * v1 - 0.714136 * v2;
                                b = v0 + 1.772 * v1;
                            }
                        }
                        else if (siz.ncomp >= 3)
                        {
                            r = v0;
                            g = all_comp_planes[1][py * cws[1] + px];
                            b = all_comp_planes[2][py * cws[2] + px];
                        }
                        else
                        {
                            r = g = b = v0;
                        }

                        r += shift0; g += shift0; b += shift0;
                        if (r < 0) r = 0;
                        if (r > maxval0) r = maxval0;
                        if (g < 0) g = 0;
                        if (g > maxval0) g = maxval0;
                        if (b < 0) b = 0;
                        if (b > maxval0) b = maxval0;

                        {
                            int gx = trect.x0 + px, gy = trect.y0 + py;
                            long oidx = ((long)gy * siz.Xsiz + gx) * 3;
                            out->rgb[oidx+0] = (unsigned char)(r * scale + 0.5);
                            out->rgb[oidx+1] = (unsigned char)(g * scale + 0.5);
                            out->rgb[oidx+2] = (unsigned char)(b * scale + 0.5);
                        }
                    }
                }
            }
            for (c = 0; c < siz.ncomp; c++) all_comp_planes[c] = NULL;
        }
    }

    return PDF_OK;
}
