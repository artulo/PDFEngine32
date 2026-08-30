/* pdf_filter.c
 *
 * Ver pdf_filter.h. El inflate (RFC 1951) esta escrito desde cero para
 * este motor (sin depender de zlib), siguiendo el algoritmo estandar de
 * Huffman canonico descripto en la RFC -- misma familia de algoritmo que
 * usa zlib/miniz/puff.c, pero implementacion propia.
 */

#include "pdf_filter.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Buffer de salida que crece dentro de la arena (mismo patron que los
 * arrays de pdf_object.c: duplica y copia; el bloque viejo queda como
 * desperdicio hasta el proximo reset de la arena, aceptable porque la
 * arena de decode se resetea entre streams). */

typedef struct
{
    pdf_arena     *arena;
    unsigned char *data;
    long           len;
    long           cap;
} pdf_dynbuf;

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que los bugs ya documentados en pdf_object.c/pdf_xref.c): la
 * miscompilacion de bcc32 7.70 con escrituras directas consecutivas a
 * campos adyacentes de un struct/union tambien aparece aca -- confirmado
 * end-to-end contra tests/Conveyor_Handbook.pdf, donde el 'pdf_buf' de
 * salida de pdf_filter_flate() (out->data/out->len, ver mas abajo)
 * quedaba con 'len' corrompido, produciendo lecturas fuera de rango mas
 * adelante en pdf_xref_load_stream_section. Mismo fix en TODO este
 * archivo: cada campo se escribe via memcpy() desde una variable local
 * en vez de asignacion directa. */
static int dynbuf_init(pdf_dynbuf *db, pdf_arena *arena, long initial_cap)
{
    unsigned char *data;
    long zero_len;

    if (initial_cap < 256) initial_cap = 256;
    memcpy(&db->arena, &arena, sizeof(arena));
    data = (unsigned char *)pdf_arena_alloc(arena, (size_t)initial_cap);
    if (data == NULL) return PDF_ERR_NOMEM;
    zero_len = 0;
    memcpy(&db->data, &data, sizeof(data));
    memcpy(&db->len, &zero_len, sizeof(zero_len));
    memcpy(&db->cap, &initial_cap, sizeof(initial_cap));
    return PDF_OK;
}

static int dynbuf_ensure(pdf_dynbuf *db, long more)
{
    long new_cap;
    unsigned char *new_data;

    if (db->len + more <= db->cap)
        return PDF_OK;

    new_cap = db->cap * 2;
    while (new_cap < db->len + more)
        new_cap *= 2;

    new_data = (unsigned char *)pdf_arena_alloc(db->arena, (size_t)new_cap);
    if (new_data == NULL)
        return PDF_ERR_NOMEM; /* presupuesto agotado: la decodificacion se corta aca */

    memcpy(new_data, db->data, (size_t)db->len);
    memcpy(&db->data, &new_data, sizeof(new_data));
    memcpy(&db->cap, &new_cap, sizeof(new_cap));
    return PDF_OK;
}

static int dynbuf_put_byte(pdf_dynbuf *db, unsigned char b)
{
    if (dynbuf_ensure(db, 1) != PDF_OK)
        return PDF_ERR_NOMEM;
    db->data[db->len++] = b;
    return PDF_OK;
}

/* ------------------------------------------------------------------ */
/* ASCII85Decode */

int pdf_filter_ascii85(pdf_arena *arena, const unsigned char *src, long src_len,
                        pdf_buf *out)
{
    pdf_dynbuf db;
    long i;
    unsigned long tuple;
    int count;

    if (dynbuf_init(&db, arena, src_len) != PDF_OK)
        return PDF_ERR_NOMEM;

    tuple = 0;
    count = 0;

    for (i = 0; i < src_len; i++)
    {
        unsigned char c = src[i];

        if (c == '~') /* terminador "~>" */
            break;
        if (c == 'z' && count == 0)
        {
            if (dynbuf_ensure(&db, 4) != PDF_OK) return PDF_ERR_NOMEM;
            db.data[db.len++] = 0; db.data[db.len++] = 0;
            db.data[db.len++] = 0; db.data[db.len++] = 0;
            continue;
        }
        if (c < '!' || c > 'u')
            continue; /* espacio u otro caracter fuera de rango: se ignora */

        tuple = tuple * 85 + (unsigned long)(c - '!');
        count++;

        if (count == 5)
        {
            if (dynbuf_ensure(&db, 4) != PDF_OK) return PDF_ERR_NOMEM;
            db.data[db.len++] = (unsigned char)(tuple >> 24);
            db.data[db.len++] = (unsigned char)(tuple >> 16);
            db.data[db.len++] = (unsigned char)(tuple >> 8);
            db.data[db.len++] = (unsigned char)(tuple);
            tuple = 0;
            count = 0;
        }
    }

    if (count > 0)
    {
        /* grupo final incompleto: rellenar con 'u' (84) segun el estandar */
        int k;
        int n = count;
        for (k = count; k < 5; k++)
            tuple = tuple * 85 + 84;
        if (dynbuf_ensure(&db, 4) != PDF_OK) return PDF_ERR_NOMEM;
        {
            unsigned char bytes[4];
            bytes[0] = (unsigned char)(tuple >> 24);
            bytes[1] = (unsigned char)(tuple >> 16);
            bytes[2] = (unsigned char)(tuple >> 8);
            bytes[3] = (unsigned char)(tuple);
            for (k = 0; k < n - 1; k++)
                db.data[db.len++] = bytes[k];
        }
    }

    memcpy(&out->data, &db.data, sizeof(db.data));
    memcpy(&out->len, &db.len, sizeof(db.len));
    return PDF_OK;
}

/* ------------------------------------------------------------------ */
/* Inflate (RFC 1951) sobre wrapper zlib (RFC 1950) para FlateDecode.
 * Implementacion propia de Huffman canonico + LZ77, sin dependencias. */

typedef struct
{
    const unsigned char *src;
    long                  len;
    long                  pos;
    unsigned long          bitbuf;
    int                    bitcnt;
} pdf_bits;

static int bits_getbit(pdf_bits *b)
{
    int bit;
    if (b->bitcnt == 0)
    {
        unsigned long newbuf;
        int newcnt;
        if (b->pos >= b->len)
            return -1;
        newbuf = (unsigned long)b->src[b->pos++];
        newcnt = 8;
        /* BUG REAL ENCONTRADO -- ver comentario extenso junto a
         * dynbuf_init mas arriba: dos escrituras directas consecutivas
         * a campos adyacentes ('bitbuf'/'bitcnt') de pdf_bits, la
         * estructura mas "caliente" de todo el inflate (se llama una
         * vez POR BIT decodificado) -- confirmado como el punto de
         * corrupcion real detras del crash contra el xref stream de
         * tests/Conveyor_Handbook.pdf (los fixes anteriores en
         * dynbuf_init/pdf_filter_flate no alcanzaban porque el bug
         * estaba aca, mas adentro). */
        memcpy(&b->bitbuf, &newbuf, sizeof(newbuf));
        memcpy(&b->bitcnt, &newcnt, sizeof(newcnt));
    }
    bit = (int)(b->bitbuf & 1);
    b->bitbuf >>= 1;
    b->bitcnt--;
    return bit;
}

static long bits_getbits(pdf_bits *b, int n)
{
    long val = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        int bit = bits_getbit(b);
        if (bit < 0) return -1;
        val |= ((long)bit) << i;
    }
    return val;
}

static void bits_align_byte(pdf_bits *b)
{
    unsigned long zero_l = 0;
    int zero_i = 0;
    memcpy(&b->bitbuf, &zero_l, sizeof(zero_l));
    memcpy(&b->bitcnt, &zero_i, sizeof(zero_i));
}

/* Tabla de Huffman canonico: cuenta de codigos por longitud + simbolos
 * ordenados. Suficiente para decodificar sin construir un arbol real. */
#define PDF_MAXBITS 15
#define PDF_MAXCODES 288

typedef struct
{
    int count[PDF_MAXBITS + 1];
    int symbol[PDF_MAXCODES];
} pdf_huff;

static int huff_build(pdf_huff *h, const unsigned char *lengths, int n)
{
    int i;
    int offs[PDF_MAXBITS + 1];

    for (i = 0; i <= PDF_MAXBITS; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;

    offs[1] = 0;
    for (i = 1; i < PDF_MAXBITS; i++)
        offs[i + 1] = offs[i] + h->count[i];

    for (i = 0; i < n; i++)
    {
        if (lengths[i] != 0)
            h->symbol[offs[lengths[i]]++] = i;
    }
    return 0;
}

static int huff_decode(pdf_bits *b, const pdf_huff *h)
{
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len <= PDF_MAXBITS; len++)
    {
        int bit = bits_getbit(b);
        if (bit < 0) return -1;
        code |= bit;
        {
            int count = h->count[len];
            if (code - first < count)
                return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    return -1; /* codigo invalido */
}

static const short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const char LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const char DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inflate_block_data(pdf_bits *b, pdf_dynbuf *out,
                               const pdf_huff *lh, const pdf_huff *dh)
{
    for (;;)
    {
        int sym = huff_decode(b, lh);
        if (sym < 0) return PDF_ERR_BADARG;

        if (sym < 256)
        {
            if (dynbuf_put_byte(out, (unsigned char)sym) != PDF_OK)
                return PDF_ERR_NOMEM;
        }
        else if (sym == 256)
        {
            return PDF_OK; /* fin de bloque */
        }
        else
        {
            int len_idx = sym - 257;
            long length, dist_val;
            int dsym;

            if (len_idx >= 29) return PDF_ERR_BADARG;
            length = LEN_BASE[len_idx];
            if (LEN_EXTRA[len_idx] > 0)
            {
                long extra = bits_getbits(b, LEN_EXTRA[len_idx]);
                if (extra < 0) return PDF_ERR_BADARG;
                length += extra;
            }

            dsym = huff_decode(b, dh);
            if (dsym < 0 || dsym >= 30) return PDF_ERR_BADARG;
            dist_val = DIST_BASE[dsym];
            if (DIST_EXTRA[dsym] > 0)
            {
                long extra = bits_getbits(b, DIST_EXTRA[dsym]);
                if (extra < 0) return PDF_ERR_BADARG;
                dist_val += extra;
            }

            if (dist_val > out->len) return PDF_ERR_BADARG; /* backref invalida */

            {
                long src_pos = out->len - dist_val;
                long k;
                for (k = 0; k < length; k++)
                {
                    if (dynbuf_put_byte(out, out->data[src_pos + k]) != PDF_OK)
                        return PDF_ERR_NOMEM;
                }
            }
        }
    }
}

static void build_fixed_tables(pdf_huff *lh, pdf_huff *dh)
{
    unsigned char lengths[288];
    unsigned char dlengths[30];
    int i;

    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (i = 144; i < 256; i++) lengths[i] = 9;
    for (i = 256; i < 280; i++) lengths[i] = 7;
    for (i = 280; i < 288; i++) lengths[i] = 8;
    huff_build(lh, lengths, 288);

    for (i = 0; i < 30; i++) dlengths[i] = 5;
    huff_build(dh, dlengths, 30);
}

static const short CLEN_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int inflate_dynamic_block(pdf_bits *b, pdf_dynbuf *out)
{
    long hlit, hdist, hclen;
    unsigned char clen_lengths[19];
    pdf_huff clen_huff;
    unsigned char lengths[288 + 30];
    long total, n;
    pdf_huff lh, dh;
    int i;

    hlit  = bits_getbits(b, 5); if (hlit  < 0) return PDF_ERR_BADARG; hlit  += 257;
    hdist = bits_getbits(b, 5); if (hdist < 0) return PDF_ERR_BADARG; hdist += 1;
    hclen = bits_getbits(b, 4); if (hclen < 0) return PDF_ERR_BADARG; hclen += 4;

    for (i = 0; i < 19; i++) clen_lengths[i] = 0;
    for (i = 0; i < hclen; i++)
    {
        long v = bits_getbits(b, 3);
        if (v < 0) return PDF_ERR_BADARG;
        clen_lengths[CLEN_ORDER[i]] = (unsigned char)v;
    }
    huff_build(&clen_huff, clen_lengths, 19);

    total = hlit + hdist;
    n = 0;
    while (n < total)
    {
        int sym = huff_decode(b, &clen_huff);
        if (sym < 0) return PDF_ERR_BADARG;

        if (sym < 16)
        {
            lengths[n++] = (unsigned char)sym;
        }
        else if (sym == 16)
        {
            long rep; int prev;
            if (n == 0) return PDF_ERR_BADARG;
            prev = lengths[n - 1];
            rep = bits_getbits(b, 2); if (rep < 0) return PDF_ERR_BADARG;
            rep += 3;
            while (rep-- > 0 && n < total) lengths[n++] = (unsigned char)prev;
        }
        else if (sym == 17)
        {
            long rep = bits_getbits(b, 3); if (rep < 0) return PDF_ERR_BADARG;
            rep += 3;
            while (rep-- > 0 && n < total) lengths[n++] = 0;
        }
        else /* 18 */
        {
            long rep = bits_getbits(b, 7); if (rep < 0) return PDF_ERR_BADARG;
            rep += 11;
            while (rep-- > 0 && n < total) lengths[n++] = 0;
        }
    }

    huff_build(&lh, lengths, (int)hlit);
    huff_build(&dh, lengths + hlit, (int)hdist);

    return inflate_block_data(b, out, &lh, &dh);
}

static int inflate_stored_block(pdf_bits *b, pdf_dynbuf *out)
{
    unsigned int len, nlen;

    bits_align_byte(b);
    if (b->pos + 4 > b->len) return PDF_ERR_BADARG;

    len  = (unsigned int)b->src[b->pos]     | ((unsigned int)b->src[b->pos+1] << 8);
    nlen = (unsigned int)b->src[b->pos + 2] | ((unsigned int)b->src[b->pos+3] << 8);
    b->pos += 4;

    if ((len ^ 0xFFFFu) != nlen) return PDF_ERR_BADARG;
    if (b->pos + (long)len > b->len) return PDF_ERR_BADARG;

    if (dynbuf_ensure(out, (long)len) != PDF_OK) return PDF_ERR_NOMEM;
    memcpy(out->data + out->len, b->src + b->pos, len);
    out->len += (long)len;
    b->pos += (long)len;

    return PDF_OK;
}

static int inflate_raw(const unsigned char *src, long src_len, pdf_dynbuf *out)
{
    pdf_bits b;
    int final_block;
    long zero_l = 0;
    unsigned long zero_ul = 0;
    int zero_i = 0;

    memcpy(&b.src, &src, sizeof(src));
    memcpy(&b.len, &src_len, sizeof(src_len));
    memcpy(&b.pos, &zero_l, sizeof(zero_l));
    memcpy(&b.bitbuf, &zero_ul, sizeof(zero_ul));
    memcpy(&b.bitcnt, &zero_i, sizeof(zero_i));

    do
    {
        int bfinal, btype;
        int rc;

        bfinal = bits_getbit(&b);
        if (bfinal < 0) return PDF_ERR_BADARG;
        final_block = bfinal;

        btype = (int)bits_getbits(&b, 2);
        if (btype < 0) return PDF_ERR_BADARG;

        if (btype == 0)
        {
            rc = inflate_stored_block(&b, out);
        }
        else if (btype == 1)
        {
            pdf_huff lh, dh;
            build_fixed_tables(&lh, &dh);
            rc = inflate_block_data(&b, out, &lh, &dh);
        }
        else if (btype == 2)
        {
            rc = inflate_dynamic_block(&b, out);
        }
        else
        {
            return PDF_ERR_BADARG; /* btype==3 es reservado/invalido */
        }

        if (rc != PDF_OK) return rc;

    } while (!final_block);

    return PDF_OK;
}

int pdf_filter_flate(pdf_arena *arena, const unsigned char *src, long src_len,
                      long expected_size, pdf_buf *out)
{
    pdf_dynbuf db;
    long start;
    int rc;

    if (src_len < 2)
        return PDF_ERR_BADARG;

    /* wrapper zlib (RFC 1950): 2 bytes de cabecera (CMF/FLG). Si no
     * matchea el patron esperado, se asume deflate crudo (algunos PDFs
     * mal formados omiten el wrapper). */
    start = 0;
    if ((src[0] & 0x0F) == 8 && (((unsigned int)(src[0] << 8) | (unsigned int)src[1]) % 31) == 0)
        start = 2;

    if (dynbuf_init(&db, arena, expected_size > 0 ? expected_size : 4096) != PDF_OK)
        return PDF_ERR_NOMEM;

    rc = inflate_raw(src + start, src_len - start, &db);
    if (rc != PDF_OK)
        return rc;

    /* Nota: el Adler32 final (4 bytes tras los datos deflate) no se
     * valida en este esqueleto -- TODO si hace falta deteccion de
     * streams corruptos mas estricta. */

    memcpy(&out->data, &db.data, sizeof(db.data));
    memcpy(&out->len, &db.len, sizeof(db.len));
    return PDF_OK;
}

/* ==================================================================== */
/* DCTDecode: decodificador JPEG baseline propio                        */
/* ==================================================================== */

typedef struct
{
    int counts[17]; /* cantidad de codigos de cada longitud, 1..16 (indice 0 sin usar) */
    int symbols[256];
} pdf_jpeg_huff;

static void jpeg_huff_build(pdf_jpeg_huff *h, const unsigned char *bits16,
                             const unsigned char *values, int total)
{
    int i;
    h->counts[0] = 0;
    for (i = 1; i <= 16; i++)
        h->counts[i] = bits16[i - 1];
    for (i = 0; i < total && i < 256; i++)
        h->symbols[i] = values[i];
}

typedef struct
{
    const unsigned char *data;
    long len, pos;
    unsigned int bitbuf;
    int bitcnt;
} jpeg_bits;

/* Lee el proximo byte de datos entropy-coded, deshaciendo el byte
 * stuffing (FF 00 -> FF) y deteniendose (sin consumir) si encuentra un
 * marcador real (para que el llamador de mas alto nivel lo procese --
 * tipicamente un RST de reinicio de intervalo, o el fin del scan). */
static int jpeg_next_byte(jpeg_bits *b)
{
    unsigned char c;
    if (b->pos >= b->len) return -1;
    c = b->data[b->pos];
    if (c == 0xFF)
    {
        if (b->pos + 1 < b->len && b->data[b->pos + 1] == 0x00)
        {
            b->pos += 2;
            return 0xFF;
        }
        return -1; /* marcador real: no consumir */
    }
    b->pos++;
    return c;
}

static int jpeg_getbit(jpeg_bits *b)
{
    if (b->bitcnt == 0)
    {
        int c = jpeg_next_byte(b);
        if (c < 0) return -1;
        b->bitbuf = (unsigned int)c;
        b->bitcnt = 8;
    }
    b->bitcnt--;
    return (int)((b->bitbuf >> b->bitcnt) & 1);
}

/* BUG REAL DE RENDIMIENTO (Arturo: "comparando con Acrobat/MuPDF el
 * nuestro es extremadamente lento", medido contra 3240-3241-2.pdf --
 * un escaneo de pagina completa, decenas de miles de bloques JPEG por
 * imagen): 'jpeg_getbits'/'jpeg_huff_decode' son los dos puntos MAS
 * calientes del decoder (se llaman por CADA bit de CADA simbolo DC/AC
 * de CADA bloque de la imagen) y antes llamaban a 'jpeg_getbit()' --
 * una funcion aparte -- una vez POR BIT. bcc32 7.70 no la inlinea de
 * por si (a diferencia de un compilador moderno). Se pega el CUERPO
 * de 'jpeg_getbit()' directo adentro de estos dos loops -- MISMA
 * logica exacta, ni una linea de comportamiento distinta, solo sin el
 * costo de la llamada a funcion en el camino mas transitado. La
 * version con funcion sigue existiendo y se usa igual en el camino
 * progresivo (SOF2, mucho menos comun) mas abajo. */
static int jpeg_getbits(jpeg_bits *b, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++)
    {
        int bit;
        if (b->bitcnt == 0)
        {
            int c = jpeg_next_byte(b);
            if (c < 0) return -1;
            b->bitbuf = (unsigned int)c;
            b->bitcnt = 8;
        }
        b->bitcnt--;
        bit = (int)((b->bitbuf >> b->bitcnt) & 1);
        v = (v << 1) | bit;
    }
    return v;
}

static int jpeg_huff_decode(jpeg_bits *b, const pdf_jpeg_huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 16; len++)
    {
        int bit, count;
        if (b->bitcnt == 0)
        {
            int c = jpeg_next_byte(b);
            if (c < 0) return -1;
            b->bitbuf = (unsigned int)c;
            b->bitcnt = 8;
        }
        b->bitcnt--;
        bit = (int)((b->bitbuf >> b->bitcnt) & 1);
        code = (code << 1) | bit;
        count = h->counts[len];
        if (code - first < count)
            return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
    }
    return -1;
}

static int jpeg_extend(int v, int t)
{
    if (t == 0) return 0;
    if (v < (1 << (t - 1)))
        return v - (1 << t) + 1;
    return v;
}

static const int PDF_JPEG_ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static int jpeg_decode_block(jpeg_bits *b, const pdf_jpeg_huff *dc_h,
                              const pdf_jpeg_huff *ac_h, const int *qtable,
                              int *dc_pred, int *coeffs)
{
    int t, diff, k;
    int i;

    for (i = 0; i < 64; i++) coeffs[i] = 0;

    t = jpeg_huff_decode(b, dc_h);
    if (t < 0 || t > 11) return PDF_ERR_BADARG;
    if (t == 0)
    {
        diff = 0;
    }
    else
    {
        int v = jpeg_getbits(b, t);
        if (v < 0) return PDF_ERR_BADARG;
        diff = jpeg_extend(v, t);
    }
    *dc_pred += diff;
    coeffs[0] = *dc_pred * qtable[0];

    k = 1;
    while (k < 64)
    {
        int rs = jpeg_huff_decode(b, ac_h);
        int run, size;
        if (rs < 0) return PDF_ERR_BADARG;
        run  = rs >> 4;
        size = rs & 0xF;
        if (size == 0)
        {
            if (run == 15) { k += 16; continue; } /* ZRL: 16 ceros */
            break; /* EOB */
        }
        k += run;
        if (k >= 64) break;
        {
            int v = jpeg_getbits(b, size);
            if (v < 0) return PDF_ERR_BADARG;
            coeffs[PDF_JPEG_ZIGZAG[k]] = jpeg_extend(v, size) * qtable[k];
        }
        k++;
    }

    return PDF_OK;
}

/* IDCT 2D separable (dos pasadas de IDCT 1D), con tabla de cosenos
 * precalculada -- mucho mas rapido que la formula O(N^4) directa. */
static double PDF_JPEG_COS[8][8];
static int pdf_jpeg_cos_ready = 0;

/* IDCT reducida de 4 puntos (ver DESIGN.md seccion 87 y el comentario
 * grande de 'jpeg_idct_block' mas abajo) -- tabla ANALOGA a
 * PDF_JPEG_COS pero para N=4 en vez de N=8: misma formula general de
 * la IDCT-III (out[x] = 0.5 * suma_u C(u)*S(u)*cos((2x+1)*u*pi/(2N))),
 * con N=4 en el denominador del coseno en vez de N=8. NO es la tabla
 * de 8 puntos truncada a un 4x4 -- son angulos distintos porque
 * representan una transformada de 4 puntos independiente, no una
 * submuestra de la de 8. Verificado numericamente (harness Python con
 * numpy, coeficientes con la forma tipica de un bloque JPEG real ya
 * cuantizado -- DC grande, AC decayendo con la frecuencia) contra
 * decodificar a resolucion completa y promediar 2x2 despues: error
 * absoluto medio ~1.4 de 255, maximo ~4.2 en 5000 bloques de prueba --
 * la diferencia esperada de una tecnica de escalado real (no es un
 * atajo bit-exacto como el de la seccion 86, es una aproximacion de
 * MENOR calidad a proposito, exactamente como hace cualquier decoder
 * JPEG real con "scaled decoding" -- libjpeg la llama
 * scale_num/scale_denom). */
static double PDF_JPEG_COS4[4][4];

static void jpeg_init_cos_table(void)
{
    int x, u;
    if (pdf_jpeg_cos_ready) return;
    for (x = 0; x < 8; x++)
        for (u = 0; u < 8; u++)
        {
            /* BUG REAL DE RENDIMIENTO (misma medicion que
             * jpeg_compose_rgb mas abajo, ver ese comentario): el
             * factor C(u) (1/sqrt(2) solo para u==0) se aplicaba con
             * un branch DENTRO del loop mas caliente del decoder
             * (128 multiplicaciones por bloque, para cada uno de los
             * ~90000+ bloques de una foto escaneada tipica) en vez de
             * hornearse UNA vez en esta tabla, que ya se arma una sola
             * vez para todo el proceso. */
            double cu = (u == 0) ? (1.0 / 1.4142135623730951) : 1.0;
            PDF_JPEG_COS[x][u] = cu * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
        }
    for (x = 0; x < 4; x++)
        for (u = 0; u < 4; u++)
        {
            double cu = (u == 0) ? (1.0 / 1.4142135623730951) : 1.0;
            PDF_JPEG_COS4[x][u] = cu * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 8.0);
        }
    pdf_jpeg_cos_ready = 1;
}

/* BUG REAL DE RENDIMIENTO (Arturo: "mejoro pero sigue lento" --
 * continuacion de la seccion 85, medido contra 3240-3241-2.pdf, donde
 * se aislo que ~750ms de los ~960ms de render por pagina son el
 * propio Huffman+IDCT del decoder JPEG): la cuantizacion de JPEG
 * concentra la energia en las frecuencias BAJAS y produce, en la
 * inmensa mayoria de los bloques reales, muchos de los 64
 * coeficientes en CERO -- ese es el mecanismo mismo por el que
 * DCT+cuantizacion comprime. 'jpeg_idct_1d' sin embargo multiplicaba
 * y sumaba los 8 terminos SIEMPRE, incluyendo los que son cero (que
 * no aportan nada al resultado: sumar exactamente 0.0 a un double
 * jamas cambia su valor, no es una aproximacion). Reordenar el loop
 * (recorrer 'u' afuera, saltando los que son 0.0, y acumular sobre
 * 'x' adentro) da EXACTAMENTE el mismo resultado bit a bit -- misma
 * cuenta de sumas, mismo orden de acumulacion, solo se evita la
 * multiplicacion+suma cuando el termino no puede cambiar nada. */
static void jpeg_idct_1d(const double *in, double *out)
{
    int x, u;
    for (x = 0; x < 8; x++) out[x] = 0.0;
    for (u = 0; u < 8; u++)
    {
        double v = in[u];
        if (v == 0.0) continue;
        for (x = 0; x < 8; x++)
            out[x] += v * PDF_JPEG_COS[x][u];
    }
    for (x = 0; x < 8; x++) out[x] *= 0.5;
}

/* Version de 4 puntos de jpeg_idct_1d de arriba -- mismo truco de
 * saltar terminos en cero, misma forma, tabla PDF_JPEG_COS4 en vez de
 * PDF_JPEG_COS. Usada por el camino de reduccion (ver comentario
 * grande de jpeg_idct_block mas abajo). */
static void jpeg_idct_1d_4(const double *in, double *out)
{
    int x, u;
    for (x = 0; x < 4; x++) out[x] = 0.0;
    for (u = 0; u < 4; u++)
    {
        double v = in[u];
        if (v == 0.0) continue;
        for (x = 0; x < 4; x++)
            out[x] += v * PDF_JPEG_COS4[x][u];
    }
    for (x = 0; x < 4; x++) out[x] *= 0.5;
}

/* BUG REAL DE RENDIMIENTO (Arturo: "esta como antes, mejoro muy poco"
 * -- continuacion de la seccion 86/87, medido con un harness temporal
 * que aislo 'jpeg_idct_block' del resto de pdf_filter_dct: en
 * 3240-3241-2.pdf a la escala REAL de pantalla (0.558, "ajustar a
 * ventana"), este bloque solo es el 71% (437ms de 611ms) del tiempo
 * total de renderizar la pagina, y ese numero NO baja aunque la
 * imagen se vaya a mostrar mas chica -- porque siempre se decodifica
 * a resolucion NATIVA completa, sin importar a que escala se va a
 * dibujar despues (pdf_image_draw recien resamplea DESPUES de que
 * esto termino). 'reduction'==2 (ver pdf_filter.h) salta la IDCT de
 * 8 puntos completa y usa una de 4 puntos sobre SOLO los 16
 * coeficientes de baja frecuencia (filas 0-3, columnas 0-3 en orden
 * NATURAL -- las filas/columnas 4-7, alta frecuencia en cualquiera
 * de los dos ejes, se descartan sin decodificarlas mas, exactamente
 * lo que cualquier decoder JPEG real hace para un "scaled decode").
 * El Huffman-decode del bitstream NO cambia (ver pdf_filter.h) -- el
 * ahorro es 100% en esta funcion: 1/4 de las multiplicaciones por
 * pasada Y 1/4 de los pixeles de salida escritos. */
static void jpeg_idct_block(const int *coeffs, unsigned char *out, int reduction)
{
    double tmp1[64], tmp2[64];
    int x, y, i;

    if (reduction == 2)
    {
        for (y = 0; y < 4; y++)
        {
            double row_in[4], row_out[4];
            int u;
            for (u = 0; u < 4; u++) row_in[u] = (double)coeffs[y * 8 + u];
            jpeg_idct_1d_4(row_in, row_out);
            for (x = 0; x < 4; x++) tmp1[y * 4 + x] = row_out[x];
        }
        for (x = 0; x < 4; x++)
        {
            double col_in[4], col_out[4];
            int v;
            for (v = 0; v < 4; v++) col_in[v] = tmp1[v * 4 + x];
            jpeg_idct_1d_4(col_in, col_out);
            for (y = 0; y < 4; y++)
            {
                double s = col_out[y] + 128.0;
                if (s < 0.0) s = 0.0;
                if (s > 255.0) s = 255.0;
                out[y * 4 + x] = (unsigned char)(s + 0.5);
            }
        }
        return;
    }

    /* NOTA -- se probo y se DESCARTO un atajo que saltaba la IDCT 2D
     * entera para bloques "solo DC" (todos los 63 AC en cero,
     * reemplazando el resultado por una formula cerrada): resulto NO
     * ser bit-exacto. Verificado con un harness que probo los 4096
     * valores posibles de DC contra la IDCT completa -- incluso
     * repitiendo a mano la MISMA secuencia de multiplicaciones que
     * haria la pasada fila+columna real (mismo orden, mismas
     * constantes), el redondeo de precision extendida x87 de bcc32
     * (80 bits en registro vs 64 bits en memoria, segun como el
     * compilador decida asignar registros en cada contexto) hizo que
     * la formula "equivalente" divergiera del resultado real en
     * hasta 25 de 4096 valores probados, con la magnitud del error
     * cambiando segun como estuviera escrita la expresion -- exactamente
     * el tipo de bug de punto flotante no-reproducible que ya costo
     * mucho esfuerzo perseguir en este motor (ver seccion 81/83 de
     * DESIGN.md). Se descarto: el riesgo de reintroducir ese tipo de
     * bug no vale la ganancia. El atajo de abajo (saltar terminos en
     * CERO adentro de jpeg_idct_1d) sigue vigente y SI es seguro --
     * ahi no se reemplaza ninguna operacion por una formula
     * equivalente, solo se omiten sumas de +0.0 que son no-ops
     * exactos en IEEE754 sin importar la precision intermedia. */

    for (y = 0; y < 8; y++)
    {
        double row_in[8], row_out[8];
        int u;
        for (u = 0; u < 8; u++) row_in[u] = (double)coeffs[y * 8 + u];
        jpeg_idct_1d(row_in, row_out);
        for (x = 0; x < 8; x++) tmp1[y * 8 + x] = row_out[x];
    }
    for (x = 0; x < 8; x++)
    {
        double col_in[8], col_out[8];
        int v;
        for (v = 0; v < 8; v++) col_in[v] = tmp1[v * 8 + x];
        jpeg_idct_1d(col_in, col_out);
        for (y = 0; y < 8; y++) tmp2[y * 8 + x] = col_out[y];
    }
    for (i = 0; i < 64; i++)
    {
        double s = tmp2[i] + 128.0;
        if (s < 0.0) s = 0.0;
        if (s > 255.0) s = 255.0;
        out[i] = (unsigned char)(s + 0.5);
    }
}

typedef struct { int id, h, v, tq; } pdf_jpeg_comp;

static unsigned char jpeg_clamp255(double v)
{
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return (unsigned char)(v + 0.5);
}

/* ====================================================================
 * JPEG progresivo (SOF2) -- decodificacion por multiples "scans"
 *
 * A diferencia de baseline (un solo scan, cada bloque se decodifica y
 * se pasa por IDCT de una sola pasada), un JPEG progresivo describe
 * la imagen en VARIOS scans sucesivos, cada uno refinando un
 * subconjunto de los coeficientes DCT de cada bloque (seleccion
 * espectral: que indices de coeficiente toca este scan -- Ss..Se; y
 * aproximacion sucesiva: que bit de precision agrega -- Ah/Al). Los
 * coeficientes de TODOS los bloques de TODOS los componentes hay que
 * tenerlos guardados en memoria (sin dequantizar ni pasar por IDCT)
 * hasta que se haya visto el ultimo scan (EOI) -- recien ahi se hace
 * la reconstruccion final (dequantizar + IDCT + RGB), igual que en
 * baseline pero una sola vez al final en vez de scan-por-scan.
 *
 * Implementado siguiendo el algoritmo de referencia del propio
 * estandar (ITU-T T.81, Annex G) -- el caso de refinamiento AC
 * (G.1.2.3) es la parte mas intrincada (hay que revisar, para cada
 * coeficiente ya no-cero en el rango, si corresponde agregarle un
 * bit de correccion, mientras se cuentan los coeficientes CERO
 * salteados para saber donde cae un coeficiente nuevo -- todo
 * entrelazado con el manejo de "EOB run", que le permite a un solo
 * simbolo Huffman decir "los proximos N bloques no tienen mas
 * coeficientes no-cero en este rango", evitando repetir el simbolo
 * EOB bloque por bloque). */

/* Contexto que persiste a traves de TODOS los scans de un JPEG
 * progresivo (a diferencia de dc_pred/eobrun, que son por-scan). */
typedef struct
{
    short *coef[4];       /* [comp][block_row*blocks_per_row(comp) + block_col][0..63], orden NATURAL */
    int    blocks_per_row[4]; /* comp_w[comp]/8 -- stride de la grilla de bloques (con relleno a MCU) */
    int    blocks_per_col[4]; /* comp_h[comp]/8 */
} pdf_jpeg_prog_state;

/* DC, primer scan (Ah==0): igual que el DC de baseline (Huffman +
 * "extend"), pero el resultado se guarda desplazado por Al y SIN
 * dequantizar -- se dequantiza recien en la reconstruccion final. */
static int jpeg_decode_dc_first(jpeg_bits *b, const pdf_jpeg_huff *dc_h,
                                 int *dc_pred, short *blk, int al)
{
    int t, diff;
    t = jpeg_huff_decode(b, dc_h);
    if (t < 0 || t > 11) return PDF_ERR_BADARG; /* misma cota que el DC de baseline */
    if (t == 0) diff = 0;
    else
    {
        int v = jpeg_getbits(b, t);
        if (v < 0) return PDF_ERR_BADARG;
        diff = jpeg_extend(v, t);
    }
    *dc_pred += diff;
    blk[0] = (short)(*dc_pred << al);
    return PDF_OK;
}

/* DC, scan de refinamiento (Ah!=0): un solo bit crudo por bloque, sin
 * Huffman -- agrega ese bit de precision al coeficiente DC ya
 * conocido. */
static int jpeg_decode_dc_refine(jpeg_bits *b, short *blk, int al)
{
    int bit = jpeg_getbit(b);
    if (bit < 0) return PDF_ERR_BADARG;
    if (bit) blk[0] = (short)(blk[0] | (1 << al));
    return PDF_OK;
}

/* AC, primer scan (Ah==0) para un rango espectral Ss..Se (spec
 * G.1.2.2): igual de espiritu al AC de baseline, pero limitado al
 * rango [Ss,Se], los valores nuevos se guardan desplazados por Al, y
 * un simbolo EOBn (size==0, run==r<15) declara que los proximos
 * (2^r + bits_extra) bloques (incluido este) no tienen mas
 * coeficientes no-cero en este rango -- 'eobrun' cuenta cuantos
 * bloques MAS (despues de este) hay que saltear sin leer nada. */
static int jpeg_decode_ac_first(jpeg_bits *b, const pdf_jpeg_huff *ac_h,
                                 short *blk, int ss, int se, int al,
                                 int *eobrun)
{
    int k;
    if (*eobrun > 0) { (*eobrun)--; return PDF_OK; }
    k = ss;
    while (k <= se)
    {
        int rs = jpeg_huff_decode(b, ac_h);
        int run, size;
        if (rs < 0) return PDF_ERR_BADARG;
        run = rs >> 4; size = rs & 0xF;
        if (size == 0)
        {
            if (run < 15)
            {
                int extra = (run > 0) ? jpeg_getbits(b, run) : 0;
                if (extra < 0) return PDF_ERR_BADARG;
                *eobrun = (1 << run) - 1 + extra;
                break; /* EOB: nada mas que decodificar en este bloque */
            }
            k += 16; /* ZRL: 16 coeficientes cero */
            continue;
        }
        k += run;
        if (k > se) break;
        {
            int v = jpeg_getbits(b, size);
            if (v < 0) return PDF_ERR_BADARG;
            blk[PDF_JPEG_ZIGZAG[k]] = (short)(jpeg_extend(v, size) << al);
        }
        k++;
    }
    return PDF_OK;
}

/* AC, scan de refinamiento (Ah!=0) para Ss..Se (spec G.1.2.3) -- la
 * parte mas intrincada de todo el decodificador progresivo. p1 es el
 * valor del bit que se esta agregando (1<<Al); m1 es su version
 * "negativa" (el patron de bits para restar lo mismo si el
 * coeficiente es negativo -- en complemento a dos, sumar (-p1) logra
 * el efecto de "agregar el bit de magnitud del lado negativo"). */
static int jpeg_decode_ac_refine(jpeg_bits *b, const pdf_jpeg_huff *ac_h,
                                  short *blk, int ss, int se, int al,
                                  int *eobrun)
{
    int k = ss;
    int p1 = 1 << al;
    int m1 = -(1 << al); /* == (-1)<<al en valor, pero sin desplazar un negativo (UB en C89) */

    if (*eobrun == 0)
    {
        while (k <= se)
        {
            int rs = jpeg_huff_decode(b, ac_h);
            int run, size, new_val = 0;
            if (rs < 0) return PDF_ERR_BADARG;
            run = rs >> 4; size = rs & 0xF;

            if (size == 0)
            {
                if (run < 15)
                {
                    int extra = (run > 0) ? jpeg_getbits(b, run) : 0;
                    if (extra < 0) return PDF_ERR_BADARG;
                    *eobrun = (1 << run) + extra;
                    break; /* termina el bucle de "nuevos simbolos": lo que sigue lo maneja el EOB run de mas abajo */
                }
                /* run==15: ZRL -- saltear 16 coeficientes CERO (los
                 * no-cero que se crucen en el camino se refinan igual,
                 * no cuentan para el conteo de 16). run_remaining=15
                 * en vez de 16 porque, si el primer coeficiente
                 * cruzado ya es cero, ese YA cuenta como el primero de
                 * los 16 -- ver bucle de abajo, misma convencion que
                 * para un simbolo con new_val (run_remaining=run). */
            }
            else
            {
                /* size siempre es 1 en un scan de refinamiento AC
                 * (spec): el bit que sigue da el signo del nuevo
                 * coeficiente. */
                int sbit = jpeg_getbit(b);
                if (sbit < 0) return PDF_ERR_BADARG;
                new_val = sbit ? p1 : m1;
            }

            /* avanzar, refinando los coeficientes YA no-cero con un
             * bit de correccion cada uno, hasta encontrar 'run'
             * coeficientes CERO (esos se saltean sin tocar) -- el
             * ('run'+1)-esimo lugar CERO encontrado es donde se
             * coloca 'new_val' (si size!=0; si era ZRL, se sigue de
             * largo sin colocar nada). */
            for (;;)
            {
                short *coef;
                if (k > se) return PDF_ERR_BADARG; /* datos corruptos: nos quedamos sin rango */
                coef = &blk[PDF_JPEG_ZIGZAG[k]];
                if (*coef != 0)
                {
                    int cbit = jpeg_getbit(b);
                    if (cbit < 0) return PDF_ERR_BADARG;
                    if (cbit && (*coef & p1) == 0)
                        *coef = (short)(*coef + ((*coef >= 0) ? p1 : m1));
                }
                else
                {
                    if (run == 0) break;
                    run--;
                }
                k++;
            }

            if (size != 0)
                blk[PDF_JPEG_ZIGZAG[k]] = (short)new_val;

            k++;
        }
    }

    if (*eobrun > 0)
    {
        /* EOB run activo: no hay simbolos nuevos, pero los
         * coeficientes ya no-cero en lo que queda del rango SI se
         * siguen refinando (un bit de correccion cada uno). */
        while (k <= se)
        {
            short *coef = &blk[PDF_JPEG_ZIGZAG[k]];
            if (*coef != 0)
            {
                int cbit = jpeg_getbit(b);
                if (cbit < 0) return PDF_ERR_BADARG;
                if (cbit && (*coef & p1) == 0)
                    *coef = (short)(*coef + ((*coef >= 0) ? p1 : m1));
            }
            k++;
        }
        (*eobrun)--;
    }
    return PDF_OK;
}

/* Composicion final a RGB24, compartida entre el camino baseline (un
 * solo scan) y el progresivo (despues de acumular todos los scans) --
 * exactamente el mismo codigo que antes vivia inline dentro de
 * pdf_filter_dct, solo movido a funcion para no duplicarlo. */
static unsigned char jpeg_clamp255_i(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

static int jpeg_compose_rgb(pdf_arena *arena, pdf_jpeg_image *out,
                             unsigned char **planes, const int *comp_w,
                             const pdf_jpeg_comp *comps, int ncomp,
                             int width, int height, int maxh, int maxv,
                             int adobe_transform)
{
    int x, y;

    out->width = width;
    out->height = height;
    out->rgb = (unsigned char *)pdf_arena_alloc(arena, (size_t)width * height * 3);
    if (out->rgb == NULL) return PDF_ERR_NOMEM;

    /* BUG REAL DE RENDIMIENTO ENCONTRADO (Arturo: "mejorar la velocidad
     * ... demasiados graficos", medido contra 3240-3241-2.pdf -- una
     * pagina escaneada de 2496x1775, YCbCr de 3 componentes): esta
     * composicion tardaba ~813ms de los ~1469ms totales de decodificar
     * UNA sola imagen -- MAS que el loop de Huffman+IDCT completo. Dos
     * problemas en el mismo bucle por-pixel (4.4 millones de
     * iteraciones aca):
     *   1. 'cbx'/'crx' (columna de croma, dependen SOLO de x) y
     *      'cby'/'cry' (fila de croma, dependen SOLO de y) se
     *      recalculaban con una DIVISION ENTERA cada uno, en CADA
     *      PIXEL -- 4 divisiones/pixel que en realidad solo necesitan
     *      recalcularse una vez por columna (cbx/crx, antes del loop
     *      de filas) o una vez por fila (cby/cry, antes del loop de
     *      columnas).
     *   2. La conversion YCbCr->RGB hacia 3 multiplicaciones de PUNTO
     *      FLOTANTE (1.402/-0.344136/-0.714136/1.772) por pixel --
     *      con Cb/Cr acotados a 0-255, el resultado de cada
     *      coeficiente*componente tiene solo 256 valores posibles:
     *      una tabla de 256 enteros por coeficiente (armada UNA vez
     *      para toda la imagen) reemplaza la multiplicacion de punto
     *      flotante por una lectura de array. */
    if (ncomp == 1)
    {
        for (y = 0; y < height; y++)
        {
            const unsigned char *srow = planes[0] + (size_t)y * comp_w[0];
            unsigned char *drow = out->rgb + (size_t)y * width * 3;
            for (x = 0; x < width; x++)
            {
                unsigned char v = srow[x];
                drow[x*3+0] = v; drow[x*3+1] = v; drow[x*3+2] = v;
            }
        }
        return PDF_OK;
    }

    if (ncomp == 3)
    {
        /* Tablas de conversion YCbCr->RGB (ver comentario grande arriba)
         * -- una sola vez para toda la imagen, no por pixel. */
        int cr_to_r[256], cb_to_g[256], cr_to_g[256], cb_to_b[256];
        int *cbx_of_x, *crx_of_x;

        for (x = 0; x < 256; x++)
        {
            cr_to_r[x] = (int)(1.402    * (x - 128) + (x >= 128 ? 0.5 : -0.5));
            cb_to_g[x] = (int)(-0.344136 * (x - 128) + (x >= 128 ? 0.5 : -0.5));
            cr_to_g[x] = (int)(-0.714136 * (x - 128) + (x >= 128 ? 0.5 : -0.5));
            cb_to_b[x] = (int)(1.772    * (x - 128) + (x >= 128 ? 0.5 : -0.5));
        }

        cbx_of_x = (int *)pdf_arena_alloc(arena, (size_t)width * sizeof(int));
        crx_of_x = (int *)pdf_arena_alloc(arena, (size_t)width * sizeof(int));
        if (cbx_of_x == NULL || crx_of_x == NULL) return PDF_ERR_NOMEM;
        for (x = 0; x < width; x++)
        {
            cbx_of_x[x] = x * comps[1].h / maxh;
            crx_of_x[x] = x * comps[2].h / maxh;
        }

        for (y = 0; y < height; y++)
        {
            const unsigned char *yrow = planes[0] + (size_t)y * comp_w[0];
            const unsigned char *cbrow = planes[1] + (size_t)(y * comps[1].v / maxv) * comp_w[1];
            const unsigned char *crrow = planes[2] + (size_t)(y * comps[2].v / maxv) * comp_w[2];
            unsigned char *drow = out->rgb + (size_t)y * width * 3;

            for (x = 0; x < width; x++)
            {
                int yv = yrow[x];
                int cb = cbrow[cbx_of_x[x]];
                int cr = crrow[crx_of_x[x]];
                drow[x*3+0] = jpeg_clamp255_i(yv + cr_to_r[cr]);
                drow[x*3+1] = jpeg_clamp255_i(yv + cb_to_g[cb] + cr_to_g[cr]);
                drow[x*3+2] = jpeg_clamp255_i(yv + cb_to_b[cb]);
            }
        }
        return PDF_OK;
    }

    /* ncomp == 4 (CMYK/YCCK) -- caso raro, se deja sin la optimizacion
     * de tablas de arriba (no lo ejercita ningun archivo real visto
     * todavia; si hiciera falta, mismo criterio: precomputar indices
     * de croma por fila/columna y tablas de 256 en vez de por-pixel). */
    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            unsigned char R, G, B;
            int c0 = planes[0][y * comp_w[0] + x];
            int c1v = planes[1][(y * comps[1].v / maxv) * comp_w[1] + (x * comps[1].h / maxh)];
            int c2v = planes[2][(y * comps[2].v / maxv) * comp_w[2] + (x * comps[2].h / maxh)];
            int c3 = planes[3][(y * comps[3].v / maxv) * comp_w[3] + (x * comps[3].h / maxh)];
            double c, m, ye, k;

            if (adobe_transform == 2)
            {
                int rr = (int)jpeg_clamp255(c0 + 1.402 * (c2v - 128));
                int gg = (int)jpeg_clamp255(c0 - 0.344136 * (c1v - 128) - 0.714136 * (c2v - 128));
                int bb = (int)jpeg_clamp255(c0 + 1.772 * (c1v - 128));
                c = 255 - rr; m = 255 - gg; ye = 255 - bb; k = c3;
            }
            else
            {
                c = c0; m = c1v; ye = c2v; k = c3;
            }

            /* CMYK -> RGB, formula "print" simple (ver DESIGN.md
             * sobre por que NO se invierten los canales aunque
             * este el marcador Adobe -- verificado empiricamente
             * que invertir da peor resultado, no mejor). */
            {
                double cc = c / 255.0, mm = m / 255.0, yy2 = ye / 255.0, kk = k / 255.0;
                double rr = 1.0 - ((cc + kk > 1.0) ? 1.0 : cc + kk);
                double gg = 1.0 - ((mm + kk > 1.0) ? 1.0 : mm + kk);
                double bb = 1.0 - ((yy2 + kk > 1.0) ? 1.0 : yy2 + kk);
                R = (unsigned char)(rr * 255.0 + 0.5);
                G = (unsigned char)(gg * 255.0 + 0.5);
                B = (unsigned char)(bb * 255.0 + 0.5);
            }
            out->rgb[(y * width + x) * 3 + 0] = R;
            out->rgb[(y * width + x) * 3 + 1] = G;
            out->rgb[(y * width + x) * 3 + 2] = B;
        }
    }
    return PDF_OK;
}

/* ==================================================================== */

int pdf_filter_dct(pdf_arena *arena, const unsigned char *src, long src_len,
                    pdf_jpeg_image *out, int reduction)
{
    long pos;
    int qtables[4][64];
    pdf_jpeg_huff dc_huff[4], ac_huff[4];
    pdf_jpeg_comp comps[4];
    int ncomp = 0, width = 0, height = 0;
    int restart_interval = 0;
    int maxh = 1, maxv = 1;
    int mcus_x = 0, mcus_y = 0;
    int comp_w[4], comp_h[4];
    unsigned char *planes[4];
    int have_sof = 0;
    int adobe_transform = 0;
    int i;
    /* progresivo (SOF2, ver DESIGN.md seccion 58): coeficientes DCT
     * acumulados a traves de multiples scans, reservados apenas se
     * conocen las dimensiones (en el SOF) y llenados de a poco en
     * cada SOS -- la reconstruccion final se hace una sola vez al
     * llegar a EOI. En baseline, 'coef[i]' se queda en NULL siempre
     * (no se usa, el camino baseline reconstruye scan-a-scan como
     * antes). */
    int progressive = 0;
    short *coef[4];
    int coef_bpr[4], coef_bpc[4];

    for (i = 0; i < 4; i++) coef[i] = NULL;

    if (reduction != 2) reduction = 1; /* unico valor no-trivial soportado hasta ahora, ver pdf_filter.h */

    jpeg_init_cos_table();
    memset(qtables, 0, sizeof(qtables));
    memset(dc_huff, 0, sizeof(dc_huff));
    memset(ac_huff, 0, sizeof(ac_huff));
    out->width = out->height = 0;
    out->rgb = NULL;

    if (src_len < 4 || src[0] != 0xFF || src[1] != 0xD8)
        return PDF_ERR_BADARG;
    pos = 2;

    for (;;)
    {
        int marker;
        long seg_len;

        if (pos + 2 > src_len) return PDF_ERR_BADARG;
        if (src[pos] != 0xFF) { pos++; continue; }
        marker = src[pos + 1];
        pos += 2;

        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
            continue;
        if (marker == 0xD9)
            break; /* EOI */

        if (pos + 2 > src_len) return PDF_ERR_BADARG;
        seg_len = (long)((src[pos] << 8) | src[pos + 1]);
        if (seg_len < 2 || pos + seg_len > src_len) return PDF_ERR_BADARG;

        if (marker == 0xEE) /* APP14: marcador "Adobe" -- el byte
                              * 'transform' (ultimo de los 12 del payload)
                              * indica si se aplico una transformacion
                              * tipo YCbCr a los 4 canales (2=YCCK) o si
                              * son CMYK directos (0), para JPEGs de 4
                              * componentes. NOTA: en un intento anterior
                              * tambien se usaba la presencia de este
                              * marcador para invertir los 4 canales (la
                              * "convencion historica de Adobe" para CMYK
                              * invertido de Photoshop) -- se deshizo
                              * despues de verificar empiricamente que
                              * para PDFs reales (La_Maldicion_del_Mutun.pdf)
                              * esa inversion daba resultados PEORES, no
                              * mejores (ver comentario mas abajo, cerca
                              * de donde se usa adobe_transform). */
        {
            long p = pos + 2;
            if (seg_len >= 14 && p + 11 < src_len &&
                src[p] == 'A' && src[p+1] == 'd' && src[p+2] == 'o' &&
                src[p+3] == 'b' && src[p+4] == 'e')
            {
                adobe_transform = src[p + 11];
            }
        }
        else if (marker == 0xDB) /* DQT */
        {
            long end = pos + seg_len;
            long p = pos + 2;
            while (p < end)
            {
                int pq_tq = src[p++];
                int pq = pq_tq >> 4, tq = pq_tq & 0xF;
                int k;
                if (tq > 3) return PDF_ERR_UNSUPPORTED;
                for (k = 0; k < 64; k++)
                {
                    if (pq == 0) { qtables[tq][k] = src[p++]; }
                    else { qtables[tq][k] = (src[p] << 8) | src[p + 1]; p += 2; }
                }
            }
        }
        else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) /* SOF0/SOF1/SOF2 */
        {
            long p = pos + 2;
            p++; /* precision, se asume 8 */
            height = (src[p] << 8) | src[p + 1]; p += 2;
            width  = (src[p] << 8) | src[p + 1]; p += 2;
            ncomp = src[p++];
            if (ncomp < 1 || ncomp > 4) return PDF_ERR_UNSUPPORTED;
            for (i = 0; i < ncomp; i++)
            {
                comps[i].id = src[p++];
                comps[i].h  = src[p] >> 4;
                comps[i].v  = src[p] & 0xF;
                p++;
                comps[i].tq = src[p++];
                if (comps[i].h > maxh) maxh = comps[i].h;
                if (comps[i].v > maxv) maxv = comps[i].v;
            }
            have_sof = 1;
            progressive = (marker == 0xC2);

            /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion
             * 58): antes, SOF2 devolvia PDF_ERR_UNSUPPORTED aca mismo
             * -- "progresivo: no soportado" -- confirmado con un PDF
             * real (boiler_light_up_procedure.pdf) donde justamente
             * la imagen mas importante de la portada (el titulo
             * estilizado) es un JPEG progresivo, y salia en blanco.
             * Progresivo describe la imagen en VARIOS scans que
             * refinan los coeficientes DCT de a poco (ver comentario
             * grande mas arriba de jpeg_decode_ac_refine) -- hay que
             * juntar TODOS los scans antes de poder hacer la
             * reconstruccion final (dequantizar + IDCT), asi que
             * apenas se conocen las dimensiones (aca, recien parseado
             * el SOF) se reserva el buffer de coeficientes de cada
             * componente -- se llena de a poco en cada SOS que venga
             * despues, y la reconstruccion final se hace una sola vez,
             * al llegar a EOI (ver mas abajo). Para SOF0/SOF1
             * (baseline) este buffer NO se usa -- el camino baseline
             * sigue exactamente igual que antes, decodificando y
             * reconstruyendo en el unico SOS que tiene. */
            if (progressive)
            {
                mcus_x = (width  + 8 * maxh - 1) / (8 * maxh);
                mcus_y = (height + 8 * maxv - 1) / (8 * maxv);
                for (i = 0; i < ncomp; i++)
                {
                    long nblocks;
                    comp_w[i] = mcus_x * comps[i].h * 8;
                    comp_h[i] = mcus_y * comps[i].v * 8;
                    coef_bpr[i] = comp_w[i] / 8;
                    coef_bpc[i] = comp_h[i] / 8;
                    nblocks = (long)coef_bpr[i] * (long)coef_bpc[i];
                    coef[i] = (short *)pdf_arena_alloc(arena, (size_t)nblocks * 64 * sizeof(short));
                    if (coef[i] == NULL) return PDF_ERR_NOMEM;
                    memset(coef[i], 0, (size_t)nblocks * 64 * sizeof(short));
                }
            }
        }
        else if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8)
        {
            return PDF_ERR_UNSUPPORTED; /* aritmetico/jerarquico/etc: no soportado */
        }
        else if (marker == 0xC4) /* DHT */
        {
            long end = pos + seg_len;
            long p = pos + 2;
            while (p < end)
            {
                int tc_th = src[p++];
                int tc = tc_th >> 4, th = tc_th & 0xF;
                unsigned char counts16[16];
                unsigned char values[256];
                int total = 0, k;
                if (th > 3) return PDF_ERR_UNSUPPORTED;
                for (k = 0; k < 16; k++) { counts16[k] = src[p++]; total += counts16[k]; }
                if (total > 256) return PDF_ERR_BADARG;
                for (k = 0; k < total; k++) values[k] = src[p++];
                jpeg_huff_build(tc == 0 ? &dc_huff[th] : &ac_huff[th], counts16, values, total);
            }
        }
        else if (marker == 0xDD) /* DRI */
        {
            restart_interval = (src[pos + 2] << 8) | src[pos + 3];
        }
        else if (marker == 0xDA) /* SOS: arranca el scan entropy-coded */
        {
            long p = pos + 2;
            int ns;
            int scan_comp[4], scan_dc[4], scan_ac[4];
            jpeg_bits bits;
            int dc_pred[4];
            int mcu_x, mcu_y, restart_count;

            if (!have_sof) return PDF_ERR_BADARG;

            ns = src[p++];
            if (ns < 1 || ns > 4 || (!progressive && ns != ncomp))
                return PDF_ERR_UNSUPPORTED; /* baseline: solo scan unico interleaved con TODOS los componentes */

            for (i = 0; i < ns; i++)
            {
                int cs = src[p++];
                int td_ta = src[p++];
                int ci;
                for (ci = 0; ci < ncomp; ci++) if (comps[ci].id == cs) break;
                if (ci == ncomp) return PDF_ERR_BADARG;
                scan_comp[i] = ci;
                scan_dc[i] = td_ta >> 4;
                scan_ac[i] = td_ta & 0xF;
            }

            if (!progressive)
            {
                /* 'bs' = tamanio de bloque de salida por eje: 8 sin
                 * reduccion (de siempre), 4 con reduction==2 (ver
                 * jpeg_idct_block y pdf_filter.h). Los planos y las
                 * posiciones px0/py0 se arman directo a este tamanio
                 * reducido -- jpeg_compose_rgb no necesita saber nada
                 * de 'reduction', solo recibe width/height/comp_w ya
                 * reducidos de forma consistente. */
                int bs = 8 / reduction;

                p += 3; /* Ss, Se, AhAl -- no aplica a baseline */

                mcus_x = (width  + 8 * maxh - 1) / (8 * maxh);
                mcus_y = (height + 8 * maxv - 1) / (8 * maxv);

                for (i = 0; i < ncomp; i++)
                {
                    comp_w[i] = mcus_x * comps[i].h * bs;
                    comp_h[i] = mcus_y * comps[i].v * bs;
                    planes[i] = (unsigned char *)pdf_arena_alloc(arena, (size_t)comp_w[i] * comp_h[i]);
                    if (planes[i] == NULL) return PDF_ERR_NOMEM;
                    dc_pred[i] = 0;
                }

                bits.data = src; bits.len = src_len; bits.pos = p; bits.bitbuf = 0; bits.bitcnt = 0;
                restart_count = 0;

                for (mcu_y = 0; mcu_y < mcus_y; mcu_y++)
                {
                    for (mcu_x = 0; mcu_x < mcus_x; mcu_x++)
                    {
                        int ci;
                        for (ci = 0; ci < ns; ci++)
                        {
                            int comp_i = scan_comp[ci];
                            int bx, by;
                            for (by = 0; by < comps[comp_i].v; by++)
                            {
                                for (bx = 0; bx < comps[comp_i].h; bx++)
                                {
                                    int coeffs[64];
                                    unsigned char block_px[64];
                                    int rc = jpeg_decode_block(&bits, &dc_huff[scan_dc[ci]],
                                                                &ac_huff[scan_ac[ci]],
                                                                qtables[comps[comp_i].tq],
                                                                &dc_pred[comp_i], coeffs);
                                    int px0, py0, yy, xx;
                                    if (rc != PDF_OK)
                                    {
                                        mcu_y = mcus_y; mcu_x = mcus_x; ci = ns; /* cortar todos los loops */
                                        break;
                                    }
                                    jpeg_idct_block(coeffs, block_px, reduction);
                                    px0 = (mcu_x * comps[comp_i].h + bx) * bs;
                                    py0 = (mcu_y * comps[comp_i].v + by) * bs;
                                    for (yy = 0; yy < bs; yy++)
                                        for (xx = 0; xx < bs; xx++)
                                            planes[comp_i][(py0 + yy) * comp_w[comp_i] + (px0 + xx)] =
                                                block_px[yy * bs + xx];
                                }
                            }
                        }

                        restart_count++;
                        if (restart_interval > 0 && restart_count == restart_interval &&
                            !(mcu_x == mcus_x - 1 && mcu_y == mcus_y - 1))
                        {
                            restart_count = 0;
                            bits.bitbuf = 0; bits.bitcnt = 0;
                            if (bits.pos + 1 < bits.len && bits.data[bits.pos] == 0xFF &&
                                bits.data[bits.pos + 1] >= 0xD0 && bits.data[bits.pos + 1] <= 0xD7)
                                bits.pos += 2;
                            for (i = 0; i < ncomp; i++) dc_pred[i] = 0;
                        }
                    }
                }

                /* --- componer a RGB24 final ------------------------------- */
                {
                    int rc = jpeg_compose_rgb(arena, out, planes, comp_w, comps, ncomp,
                                               (width + reduction - 1) / reduction,
                                               (height + reduction - 1) / reduction,
                                               maxh, maxv, adobe_transform);
                    if (rc != PDF_OK) return rc;
                }

                return PDF_OK; /* un solo scan baseline: terminamos aca */
            }
            else
            {
                /* --- scan progresivo (ver DESIGN.md seccion 58) ------------
                 * A diferencia de baseline, esto NO reconstruye nada -- solo
                 * decodifica ESTE scan hacia 'coef[]' (ya reservado cuando
                 * se vio el SOF) y sigue: el bucle de marcadores de mas
                 * arriba va a seguir buscando mas DHT/DQT/SOS hasta EOI,
                 * momento en el que se hace la reconstruccion final una
                 * sola vez (ver el 'if (marker == 0xD9) break;' de arriba,
                 * y el bloque de reconstruccion despues de este while). */
                int ss, se, ah, al;
                int eobrun = 0;

                ss = src[p++];
                se = src[p++];
                { int ahal = src[p++]; ah = ahal >> 4; al = ahal & 0xF; }
                if (ss < 0 || se > 63 || ss > se) return PDF_ERR_BADARG;

                for (i = 0; i < ns; i++) dc_pred[i] = 0;

                bits.data = src; bits.len = src_len; bits.pos = p; bits.bitbuf = 0; bits.bitcnt = 0;
                restart_count = 0;

                if (ns > 1)
                {
                    /* scan interleaved -- por spec, siempre es DC (Ss==0,
                     * Se==0); iteramos por MCU igual que baseline. */
                    for (mcu_y = 0; mcu_y < mcus_y; mcu_y++)
                    {
                        for (mcu_x = 0; mcu_x < mcus_x; mcu_x++)
                        {
                            int ci;
                            for (ci = 0; ci < ns; ci++)
                            {
                                int comp_i = scan_comp[ci];
                                int bx, by;
                                for (by = 0; by < comps[comp_i].v; by++)
                                {
                                    for (bx = 0; bx < comps[comp_i].h; bx++)
                                    {
                                        int block_row = mcu_y * comps[comp_i].v + by;
                                        int block_col = mcu_x * comps[comp_i].h + bx;
                                        short *blk = coef[comp_i] +
                                            ((long)block_row * coef_bpr[comp_i] + block_col) * 64;
                                        int rc = (ah == 0)
                                            ? jpeg_decode_dc_first(&bits, &dc_huff[scan_dc[ci]], &dc_pred[comp_i], blk, al)
                                            : jpeg_decode_dc_refine(&bits, blk, al);
                                        if (rc != PDF_OK)
                                        { mcu_y = mcus_y; mcu_x = mcus_x; ci = ns; break; }
                                    }
                                }
                            }

                            restart_count++;
                            if (restart_interval > 0 && restart_count == restart_interval &&
                                !(mcu_x == mcus_x - 1 && mcu_y == mcus_y - 1))
                            {
                                restart_count = 0;
                                bits.bitbuf = 0; bits.bitcnt = 0;
                                if (bits.pos + 1 < bits.len && bits.data[bits.pos] == 0xFF &&
                                    bits.data[bits.pos + 1] >= 0xD0 && bits.data[bits.pos + 1] <= 0xD7)
                                    bits.pos += 2;
                                for (i = 0; i < ns; i++) dc_pred[i] = 0;
                            }
                        }
                    }
                }
                else
                {
                    /* scan no-interleaved (Ns==1): DC o AC de un solo
                     * componente. Los bloques se recorren en su propia
                     * grilla (dimensiones EXACTAS del componente, formula
                     * del estandar seccion A.2.4 -- puede ser mas chica
                     * que la grilla con relleno a MCU que usa el buffer,
                     * eso esta bien, el resto queda en cero). */
                    int comp_i = scan_comp[0];
                    long comp_samples_w = ((long)width  * comps[comp_i].h + maxh - 1) / maxh;
                    long comp_samples_h = ((long)height * comps[comp_i].v + maxv - 1) / maxv;
                    int bpl = (int)((comp_samples_w + 7) / 8);
                    int bpc = (int)((comp_samples_h + 7) / 8);
                    int br, bc;
                    int mcu_count_in_restart = 0;

                    for (br = 0; br < bpc; br++)
                    {
                        for (bc = 0; bc < bpl; bc++)
                        {
                            short *blk = coef[comp_i] + ((long)br * coef_bpr[comp_i] + bc) * 64;
                            int rc;
                            if (ss == 0)
                                rc = (ah == 0)
                                    ? jpeg_decode_dc_first(&bits, &dc_huff[scan_dc[0]], &dc_pred[0], blk, al)
                                    : jpeg_decode_dc_refine(&bits, blk, al);
                            else
                                rc = (ah == 0)
                                    ? jpeg_decode_ac_first(&bits, &ac_huff[scan_ac[0]], blk, ss, se, al, &eobrun)
                                    : jpeg_decode_ac_refine(&bits, &ac_huff[scan_ac[0]], blk, ss, se, al, &eobrun);
                            if (rc != PDF_OK)
                            { br = bpc; bc = bpl; break; }

                            mcu_count_in_restart++;
                            if (restart_interval > 0 && mcu_count_in_restart == restart_interval &&
                                !(br == bpc - 1 && bc == bpl - 1))
                            {
                                mcu_count_in_restart = 0;
                                bits.bitbuf = 0; bits.bitcnt = 0;
                                if (bits.pos + 1 < bits.len && bits.data[bits.pos] == 0xFF &&
                                    bits.data[bits.pos + 1] >= 0xD0 && bits.data[bits.pos + 1] <= 0xD7)
                                    bits.pos += 2;
                                dc_pred[0] = 0;
                                eobrun = 0;
                            }
                        }
                    }
                }

                pos = bits.pos; /* dejar 'pos' justo antes del proximo marcador, para que el bucle de afuera lo encuentre */
                continue;
            }
        }

        pos += seg_len;
    }

    /* Si llegamos aca es porque el bucle de arriba encontro EOI (0xD9)
     * -- baseline ya retorno antes, apenas termino su unico SOS, asi
     * que si estamos aca con 'progressive' es porque se acumularon
     * todos los scans progresivos y toca la reconstruccion final
     * (dequantizar + IDCT, UNA sola vez por bloque, con los
     * coeficientes ya completos -- ver DESIGN.md seccion 58). */
    if (progressive && have_sof)
    {
        /* 'bs' = tamanio de bloque de salida por eje, mismo criterio
         * que el camino baseline de arriba (ver jpeg_idct_block y
         * pdf_filter.h). 'coef_bpr[i]'/'coef_bpc[i]' (cantidad de
         * bloques 8x8 del bitstream) NO cambian con la reduccion --
         * se calcularon una sola vez al leer el SOF, en terminos de
         * bloques, no de pixeles. Lo que SI se recalcula aca son
         * 'comp_w[i]'/'comp_h[i]' (dimensiones del plano de SALIDA ya
         * reducido) -- se pisan las que se usaron arriba para
         * reservar 'coef[i]' porque de aca en mas ya no hacen falta
         * en su valor original (esos buffers ya estan llenos). */
        int bs = 8 / reduction;

        for (i = 0; i < ncomp; i++)
        {
            const int *qtab = qtables[comps[i].tq];
            int br, bc;
            comp_w[i] = coef_bpr[i] * bs;
            comp_h[i] = coef_bpc[i] * bs;
            planes[i] = (unsigned char *)pdf_arena_alloc(arena, (size_t)comp_w[i] * comp_h[i]);
            if (planes[i] == NULL) return PDF_ERR_NOMEM;

            for (br = 0; br < coef_bpc[i]; br++)
            {
                for (bc = 0; bc < coef_bpr[i]; bc++)
                {
                    short *blk = coef[i] + ((long)br * coef_bpr[i] + bc) * 64;
                    int dequant[64];
                    unsigned char block_px[64];
                    int k, px0, py0, yy, xx;

                    /* dequantizar: los coeficientes en 'coef[]' estan en
                     * orden NATURAL (no zigzag, ver como los escriben
                     * jpeg_decode_*_first/refine mas arriba, siempre
                     * indexando por PDF_JPEG_ZIGZAG[k]) -- la tabla de
                     * cuantizacion SI esta en orden zigzag (como viene
                     * en el archivo), asi que hay que recorrer 'k' en
                     * zigzag para aplicar el factor correcto a cada
                     * posicion natural. */
                    for (k = 0; k < 64; k++)
                        dequant[PDF_JPEG_ZIGZAG[k]] = blk[PDF_JPEG_ZIGZAG[k]] * qtab[k];

                    jpeg_idct_block(dequant, block_px, reduction);
                    px0 = bc * bs;
                    py0 = br * bs;
                    for (yy = 0; yy < bs; yy++)
                        for (xx = 0; xx < bs; xx++)
                            planes[i][(py0 + yy) * comp_w[i] + (px0 + xx)] = block_px[yy * bs + xx];
                }
            }
        }

        {
            int rc = jpeg_compose_rgb(arena, out, planes, comp_w, comps, ncomp,
                                       (width + reduction - 1) / reduction,
                                       (height + reduction - 1) / reduction,
                                       maxh, maxv, adobe_transform);
            if (rc != PDF_OK) return rc;
        }
        return PDF_OK;
    }

    return PDF_ERR_BADARG; /* EOI sin haber pasado por SOS */
}


/* ==================================================================== */
/* CCITTFaxDecode Group 4 (T.6 / modified READ, K<0)                    */
/*                                                                      */
/* Reescrito desde cero replicando la estructura exacta de libtiff      */
/* (Sam Leffler / Silicon Graphics, tif_fax3.c/.h -- licencia BSD-like, */
/* "Permission to use, copy, modify, distribute, and sell... hereby     */
/* granted without fee") en vez de reconstruir el algoritmo de memoria: */
/* la version anterior tenia bugs reales en el manejo de b1/b2 y del    */
/* color de inicio en modo Horizontal que costaron varias rondas de     */
/* debugging sin resolverse. Las TABLAS de codigos (blanco/negro) se    */
/* verificaron por separado contra la tabla de estados de libtiff       */
/* (tif_fax3sm.c, generada por su herramienta mkg3states) y resultaron  */
/* ser correctas desde el principio -- el problema siempre estuvo en la */
/* logica de decodificacion, no en las tablas.                          */
/*                                                                      */
/* A diferencia de la version anterior (que pintaba directo sobre el    */
/* bitmap mientras decodificaba, con la linea de referencia como        */
/* posiciones absolutas), esta version decodifica cada fila completa a  */
/* un array de LONGITUDES DE CORRIDA (run-lengths) primero, y recien    */
/* despues pinta -- exactamente como hace libtiff -- porque es mas facil */
/* de verificar contra la logica de referencia sin traducir semantica   */
/* de representacion en el camino.                                     */
/* ==================================================================== */

typedef struct { int bits, code, run; } pdf_ccitt_code;

/* Tablas verificadas: extraidas y contrastadas programaticamente contra
 * tif_fax3sm.c de libtiff (formato "invocado por peek de N bits"),
 * convertidas a codigos canonicos MSB-first. Cero diferencias contra
 * la tabla de estados de libtiff en las 104+104 entradas. */
static const pdf_ccitt_code PDF_CCITT_WHITE[] = {
    {8,0x35,0},{6,0x7,1},{4,0x7,2},{4,0x8,3},
    {4,0xB,4},{4,0xC,5},{4,0xE,6},{4,0xF,7},
    {5,0x13,8},{5,0x14,9},{5,0x7,10},{5,0x8,11},
    {6,0x8,12},{6,0x3,13},{6,0x34,14},{6,0x35,15},
    {6,0x2A,16},{6,0x2B,17},{7,0x27,18},{7,0xC,19},
    {7,0x8,20},{7,0x17,21},{7,0x3,22},{7,0x4,23},
    {7,0x28,24},{7,0x2B,25},{7,0x13,26},{7,0x24,27},
    {7,0x18,28},{8,0x2,29},{8,0x3,30},{8,0x1A,31},
    {8,0x1B,32},{8,0x12,33},{8,0x13,34},{8,0x14,35},
    {8,0x15,36},{8,0x16,37},{8,0x17,38},{8,0x28,39},
    {8,0x29,40},{8,0x2A,41},{8,0x2B,42},{8,0x2C,43},
    {8,0x2D,44},{8,0x4,45},{8,0x5,46},{8,0xA,47},
    {8,0xB,48},{8,0x52,49},{8,0x53,50},{8,0x54,51},
    {8,0x55,52},{8,0x24,53},{8,0x25,54},{8,0x58,55},
    {8,0x59,56},{8,0x5A,57},{8,0x5B,58},{8,0x4A,59},
    {8,0x4B,60},{8,0x32,61},{8,0x33,62},{8,0x34,63},
    {5,0x1B,64},{5,0x12,128},{6,0x17,192},{7,0x37,256},
    {8,0x36,320},{8,0x37,384},{8,0x64,448},{8,0x65,512},
    {8,0x68,576},{8,0x67,640},{9,0xCC,704},{9,0xCD,768},
    {9,0xD2,832},{9,0xD3,896},{9,0xD4,960},{9,0xD5,1024},
    {9,0xD6,1088},{9,0xD7,1152},{9,0xD8,1216},{9,0xD9,1280},
    {9,0xDA,1344},{9,0xDB,1408},{9,0x98,1472},{9,0x99,1536},
    {9,0x9A,1600},{6,0x18,1664},{9,0x9B,1728},{11,0x8,1792},
    {11,0xC,1856},{11,0xD,1920},{12,0x12,1984},{12,0x13,2048},
    {12,0x14,2112},{12,0x15,2176},{12,0x16,2240},{12,0x17,2304},
    {12,0x1C,2368},{12,0x1D,2432},{12,0x1E,2496},{12,0x1F,2560},
};
#define PDF_CCITT_WHITE_COUNT (int)(sizeof(PDF_CCITT_WHITE)/sizeof(PDF_CCITT_WHITE[0]))

static const pdf_ccitt_code PDF_CCITT_BLACK[] = {
    {10,0x37,0},{3,0x2,1},{2,0x3,2},{2,0x2,3},
    {3,0x3,4},{4,0x3,5},{4,0x2,6},{5,0x3,7},
    {6,0x5,8},{6,0x4,9},{7,0x4,10},{7,0x5,11},
    {7,0x7,12},{8,0x4,13},{8,0x7,14},{9,0x18,15},
    {10,0x17,16},{10,0x18,17},{10,0x8,18},{11,0x67,19},
    {11,0x68,20},{11,0x6C,21},{11,0x37,22},{11,0x28,23},
    {11,0x17,24},{11,0x18,25},{12,0xCA,26},{12,0xCB,27},
    {12,0xCC,28},{12,0xCD,29},{12,0x68,30},{12,0x69,31},
    {12,0x6A,32},{12,0x6B,33},{12,0xD2,34},{12,0xD3,35},
    {12,0xD4,36},{12,0xD5,37},{12,0xD6,38},{12,0xD7,39},
    {12,0x6C,40},{12,0x6D,41},{12,0xDA,42},{12,0xDB,43},
    {12,0x54,44},{12,0x55,45},{12,0x56,46},{12,0x57,47},
    {12,0x64,48},{12,0x65,49},{12,0x52,50},{12,0x53,51},
    {12,0x24,52},{12,0x37,53},{12,0x38,54},{12,0x27,55},
    {12,0x28,56},{12,0x58,57},{12,0x59,58},{12,0x2B,59},
    {12,0x2C,60},{12,0x5A,61},{12,0x66,62},{12,0x67,63},
    {10,0xF,64},{12,0xC8,128},{12,0xC9,192},{12,0x5B,256},
    {12,0x33,320},{12,0x34,384},{12,0x35,448},{13,0x6C,512},
    {13,0x6D,576},{13,0x4A,640},{13,0x4B,704},{13,0x4C,768},
    {13,0x4D,832},{13,0x72,896},{13,0x73,960},{13,0x74,1024},
    {13,0x75,1088},{13,0x76,1152},{13,0x77,1216},{13,0x52,1280},
    {13,0x53,1344},{13,0x54,1408},{13,0x55,1472},{13,0x5A,1536},
    {13,0x5B,1600},{13,0x64,1664},{13,0x65,1728},{11,0x8,1792},
    {11,0xC,1856},{11,0xD,1920},{12,0x12,1984},{12,0x13,2048},
    {12,0x14,2112},{12,0x15,2176},{12,0x16,2240},{12,0x17,2304},
    {12,0x1C,2368},{12,0x1D,2432},{12,0x1E,2496},{12,0x1F,2560},
};
#define PDF_CCITT_BLACK_COUNT (int)(sizeof(PDF_CCITT_BLACK)/sizeof(PDF_CCITT_BLACK[0]))

typedef struct
{
    const unsigned char *data;
    long len, pos_bit;
} pdf_ccitt_bits;

static int ccitt_peekbits(pdf_ccitt_bits *b, int n, unsigned int *out)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        long bitpos = b->pos_bit + i;
        long byte_i = bitpos / 8;
        int bit_i = 7 - (int)(bitpos % 8);
        int bit;
        if (byte_i >= b->len) return 0;
        bit = (b->data[byte_i] >> bit_i) & 1;
        v = (v << 1) | (unsigned int)bit;
    }
    *out = v;
    return 1;
}

static int ccitt_read_run(pdf_ccitt_bits *b, int white, int *out_run)
{
    const pdf_ccitt_code *table = white ? PDF_CCITT_WHITE : PDF_CCITT_BLACK;
    int count = white ? PDF_CCITT_WHITE_COUNT : PDF_CCITT_BLACK_COUNT;
    int total = 0;

    for (;;)
    {
        int matched = 0, i;
        for (i = 0; i < count; i++)
        {
            unsigned int v;
            if (!ccitt_peekbits(b, table[i].bits, &v)) continue;
            if ((int)v == table[i].code)
            {
                b->pos_bit += table[i].bits;
                total += table[i].run;
                matched = 1;
                if (table[i].run < 64) { *out_run = total; return PDF_OK; } /* terminating code */
                break; /* makeup code: seguir leyendo (run>=64: makeup + posible terminating) */
            }
        }
        if (!matched) return PDF_ERR_BADARG;
    }
}

typedef enum { CCITT_PASS, CCITT_HORIZ, CCITT_V0, CCITT_VR1, CCITT_VR2, CCITT_VR3,
               CCITT_VL1, CCITT_VL2, CCITT_VL3, CCITT_EOL, CCITT_ERR } pdf_ccitt_mode;

static pdf_ccitt_mode ccitt_read_mode(pdf_ccitt_bits *b)
{
    unsigned int v;
    if (ccitt_peekbits(b, 1, &v) && v == 1) { b->pos_bit += 1; return CCITT_V0; }
    if (ccitt_peekbits(b, 3, &v))
    {
        if (v == 1) { b->pos_bit += 3; return CCITT_HORIZ; }
        if (v == 3) { b->pos_bit += 3; return CCITT_VR1; }
        if (v == 2) { b->pos_bit += 3; return CCITT_VL1; }
    }
    if (ccitt_peekbits(b, 4, &v) && v == 1) { b->pos_bit += 4; return CCITT_PASS; }
    if (ccitt_peekbits(b, 6, &v))
    {
        if (v == 3) { b->pos_bit += 6; return CCITT_VR2; }
        if (v == 2) { b->pos_bit += 6; return CCITT_VL2; }
        /* OJO: "000001" (v==1) NO es un codigo de 6 bits real -- es
         * PREFIJO de VR3 "0000011" y VL3 "0000010" (7 bits). Un bug
         * real anterior trataba v==1 aca como "extension no soportada",
         * lo que capturaba CUALQUIER VR3/VL3 real por error (mismos
         * primeros 6 bits) y cortaba la fila ahi -- encontrado
         * comparando modo por modo contra una traza en Python en el
         * mismo punto exacto del bitstream. Por eso NO hay rama para
         * v==1 aca: se deja caer al chequeo de 7 bits de abajo. */
    }
    if (ccitt_peekbits(b, 7, &v))
    {
        if (v == 3) { b->pos_bit += 7; return CCITT_VR3; }
        if (v == 2) { b->pos_bit += 7; return CCITT_VL3; }
    }
    if (ccitt_peekbits(b, 12, &v) && v == 1) { b->pos_bit += 12; return CCITT_EOL; }
    return CCITT_ERR;
}

/* CHECK_b1 de la referencia: si ya se escribio al menos un run en esta
 * fila (cur_count>0), avanzar b1 consumiendo PARES de runs de la fila
 * de referencia mientras b1 <= a0. 'ref_idx' es el indice del PROXIMO
 * run de referencia a consumir (equivalente a 'pb' en la referencia). */
/* Lectura segura de ref_runs: si ref_idx se pasa de lo que realmente
 * se lleno esta fila (ref_count), NO hay que leer memoria vieja (basura
 * de una fila anterior, por la reutilizacion del buffer via swap) --
 * hay que devolver 'columns' como si fuera un centinela, igual que la
 * intencion original de los centinelas al final del array. Este era el
 * bug real que hacia divergir filas con muchas transiciones: los 4
 * centinelas explicitos no alcanzaban para filas con mas transiciones,
 * y las lecturas directas de ref_runs[ref_idx++] en los modos Pass/V0/
 * VR/VL (a diferencia de CHECK_b1, que si tenia limite) leian memoria
 * sin inicializar o con datos de 2 filas atras. */
static int ccitt_ref_run_at(const int *ref_runs, int ref_count, int idx, int columns)
{
    if (idx < 0 || idx >= ref_count) return columns;
    return ref_runs[idx];
}

static void ccitt_check_b1(const int *ref_runs, int ref_count, int *ref_idx,
                            int *b1, int a0, int cur_count, int columns)
{
    if (cur_count == 0) return; /* pa == thisrun: todavia no se escribio nada esta fila */
    while (*b1 <= a0 && *b1 < columns)
    {
        if (*ref_idx + 1 >= ref_count) break; /* centinela: no seguir de largo */
        *b1 += ref_runs[*ref_idx] + ref_runs[*ref_idx + 1];
        *ref_idx += 2;
    }
}

int pdf_filter_ccitt_g4(pdf_arena *arena, const unsigned char *src, long src_len,
                         int columns, int rows, int black_is_1, pdf_buf *out)
{
    int *ref_runs, *cur_runs; /* arrays de LONGITUDES de corrida (no posiciones) */
    int ref_count, cur_count;
    long row_bytes;
    unsigned char *bitmap;
    int y;
    pdf_ccitt_bits bits;
    int max_runs;

    if (columns <= 0 || rows <= 0 || columns > 8000)
        return PDF_ERR_BADARG;

    max_runs = columns + 8; /* peor caso: un run por pixel, + margen de centinelas */

    ref_runs = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)max_runs);
    cur_runs = (int *)pdf_arena_alloc(arena, sizeof(int) * (size_t)max_runs);
    if (ref_runs == NULL || cur_runs == NULL) return PDF_ERR_NOMEM;

    row_bytes = (columns + 7) / 8;
    bitmap = (unsigned char *)pdf_arena_alloc(arena, (size_t)row_bytes * rows);
    if (bitmap == NULL) return PDF_ERR_NOMEM;

    /* linea de referencia inicial (antes de la fila 0): imaginariamente
     * toda blanca -- un unico run de 'columns' (igual que libtiff:
     * refruns[0]=columns, ver Fax3PreDecode en tif_fax3.c). El resto
     * del padding (aca y al pasar de fila) tiene que ser TODO CERO, no
     * repetir 'columns' -- un par que mezcle el ultimo run real con un
     * centinela "columns" puede sumar el DOBLE del ancho de fila,
     * haciendo que b1 se dispare muy por encima de 'columns'. Se probo
     * alternar [columns,0,columns,0,...] primero, pero eso solo
     * funciona si la cantidad de runs reales de la fila es PAR -- con
     * una cantidad IMPAR (mitad de los casos reales) el primer valor de
     * padding que se empareja con el ultimo run real termina siendo
     * 'columns' de nuevo, por el desfasaje de paridad. Con padding
     * TODO CERO no importa la paridad: cualquier par que toque el
     * padding suma como maximo el ultimo valor real + 0, nunca de mas.
     * Bug real encontrado comparando fila por fila contra libtiff
     * (b1 llegaba a 256 en una fila de 128 columnas, cur_runs sumaba
     * el doble de 'columns'). */
    ref_runs[0] = columns;
    ref_runs[1] = 0;
    ref_runs[2] = 0;
    ref_runs[3] = 0;
    ref_count = 4;

    bits.data = src; bits.len = src_len; bits.pos_bit = 0;

    for (y = 0; y < rows; y++)
    {
        int a0 = 0;
        int b1, ref_idx;
        int run_length_pending = 0; /* acumulador de longitud "pendiente" de modo Pass (ver libtiff SETVALUE/RunLength) */
        unsigned char *row = bitmap + (long)y * row_bytes;

        cur_count = 0;
        ref_idx = 0;
        b1 = ref_runs[ref_idx++];

        while (a0 < columns)
        {
            pdf_ccitt_mode mode = ccitt_read_mode(&bits);

            if (mode == CCITT_PASS)
            {
                int b2;
                ccitt_check_b1(ref_runs, ref_count, &ref_idx, &b1, a0, cur_count, columns);
                b2 = b1 + ccitt_ref_run_at(ref_runs, ref_count, ref_idx++, columns);
                run_length_pending += b2 - a0;
                a0 = b2;
                b1 = b2 + ccitt_ref_run_at(ref_runs, ref_count, ref_idx++, columns);
            }
            else if (mode == CCITT_HORIZ)
            {
                int white_first = ((cur_count & 1) == 0); /* igual que (pa-thisrun)&1 de la referencia */
                int run1, run2;

                if (ccitt_read_run(&bits, white_first, &run1) != PDF_OK) return PDF_ERR_BADARG;
                if (cur_count < max_runs) cur_runs[cur_count++] = run_length_pending + run1;
                a0 += run1;
                run_length_pending = 0;

                if (ccitt_read_run(&bits, !white_first, &run2) != PDF_OK) return PDF_ERR_BADARG;
                if (cur_count < max_runs) cur_runs[cur_count++] = run2;
                a0 += run2;

                ccitt_check_b1(ref_runs, ref_count, &ref_idx, &b1, a0, cur_count, columns);
            }
            else if (mode >= CCITT_V0 && mode <= CCITT_VL3)
            {
                static const int offs[7] = { 0, 1, 2, 3, -1, -2, -3 };
                int delta = offs[mode - CCITT_V0];
                int newpos;

                ccitt_check_b1(ref_runs, ref_count, &ref_idx, &b1, a0, cur_count, columns);
                newpos = b1 - a0 + delta; /* SETVALUE(b1-a0+delta) -- con el b1 YA actualizado por CHECK_b1 */

                if (cur_count < max_runs) cur_runs[cur_count++] = run_length_pending + newpos;
                a0 += newpos;
                run_length_pending = 0;

                if (mode <= CCITT_VR3)
                    b1 = b1 + ccitt_ref_run_at(ref_runs, ref_count, ref_idx++, columns); /* avanzar b1 al siguiente run de referencia */
                else
                    { ref_idx--; b1 = b1 - ccitt_ref_run_at(ref_runs, ref_count, ref_idx, columns); } /* VL: retroceder b1 (deshacer el ultimo avance) */
            }
            else
            {
                /* EOL/EXT/ERR: cortar la decodificacion aca, dejando lo
                 * que ya se decodifico -- tolerante, no aborta el resto
                 * de la imagen/pagina. */
                break;
            }
        }

        if (run_length_pending > 0 && cur_count < max_runs)
            cur_runs[cur_count++] = run_length_pending; /* run final pendiente sin cerrar (limite de fila) */

        /* --- pintar la fila: recorrer cur_runs alternando color,
         * empezando SIEMPRE en blanco (igual que la funcion 'fill' de
         * la referencia). El bit de salida representa directamente el
         * SAMPLE de DeviceGray que va a consumir el desempaquetado
         * generico de pdf_image.c (bit=1 -> blanco/255, bit=0 ->
         * negro/0 -- el Decode array default [0 1] de DeviceGray).
         *
         * BUG REAL ENCONTRADO (confirmado contra
         * 615_89_Escorias_y_cementos_siderurgicos.pdf, un PDF real
         * escaneado sin /BlackIs1 declarado, o sea el default false):
         * la version anterior escribia bit=1 para las corridas NEGRAS
         * y bit=0 para las BLANCAS cuando black_is_1 era false -- exactamente
         * al reves de lo que espera pdf_image.c, asi que la pagina
         * entera salia con los colores invertidos (fondo negro, texto
         * blanco) en vez de fondo blanco con texto negro. 'black_is_1'
         * le da la vuelta a esta asignacion (indica que la CODIFICACION
         * original del PDF uso 1=negro en vez del default 0=negro). */
        {
            int x = 0, ri, color = 0; /* 0=blanco, 1=negro */
            memset(row, black_is_1 ? 0xFF : 0x00, (size_t)row_bytes); /* fondo negro por defecto */
            for (ri = 0; ri < cur_count && x < columns; ri++)
            {
                int run = cur_runs[ri];
                int xend = x + run;
                if (xend > columns) xend = columns;
                if (color == 0)
                {
                    int xx;
                    for (xx = x; xx < xend; xx++)
                    {
                        int byte_i = xx / 8, bit_i = 7 - (xx % 8);
                        if (black_is_1) row[byte_i] &= (unsigned char)~(1 << bit_i);
                        else            row[byte_i] |= (unsigned char)(1 << bit_i);
                    }
                }
                x = xend;
                color = 1 - color;
            }
        }

        /* la fila recien decodificada pasa a ser la referencia de la
         * proxima (con centinelas extra al final). */
        {
            int *tmp = ref_runs;
            ref_runs = cur_runs;
            cur_runs = tmp;
            ref_count = cur_count;
            if (ref_count + 4 <= max_runs)
            {
                ref_runs[ref_count]     = 0;
                ref_runs[ref_count + 1] = 0;
                ref_runs[ref_count + 2] = 0;
                ref_runs[ref_count + 3] = 0;
                ref_count += 4;
            }
        }
    }

    {
        long total_len = row_bytes * rows;
        memcpy(&out->data, &bitmap, sizeof(bitmap));
        memcpy(&out->len, &total_len, sizeof(total_len));
    }
    return PDF_OK;
}
