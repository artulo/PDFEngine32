/* pdf_xref.c
 *
 * Indice y resolucion de objetos indirectos.
 */

#include "pdf_xref.h"
#include "pdf_parser.h"
#include "pdf_filter.h"
#include <string.h>
#include <stdio.h>

/* --- xref / trailer ---------------------------------------------------- */

static int tok_is_keyword(const pdf_token *tok, const char *kw)
{
    return tok->type == PDF_TOK_KEYWORD && strcmp(tok->text, kw) == 0;
}

#define PDF_TAIL_SCAN_MAX 2048
#define PDF_XREF_MAX_ENTRIES 10000000L

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que pdf_obj_new_ref/pdf_obj_new_array/pdf_obj_new_stream en
 * pdf_object.c, ver comentarios ahi): esta miscompilacion de bcc32 7.70
 * NO esta limitada a miembros de union -- aqui corrompe escrituras
 * directas consecutivas a campos adyacentes de un STRUCT PLANO
 * ('pdf_xref_table.entries'/'.capacity'/'.count'). Reproducido
 * end-to-end contra un cross-reference STREAM real
 * (tests/Conveyor_Handbook.pdf): tras pdf_xref_ensure_capacity(...,
 * needed=2814), 'xref->count' quedaba leyendose como 2836 en el resto
 * de la funcion (ver pdf_xref_load_stream_section) -- un valor
 * corrompido consistente durante toda la ejecucion posterior, y la
 * funcion terminaba crasheando (no en el propio bucle, sino recien AL
 * RETORNAR de pdf_xref_load_stream_section, sintoma compatible con
 * corrupcion adicional de la pila mas alla de estos tres campos).
 * Mismo fix: cada campo se escribe via memcpy() desde una variable
 * local. Se aplica preventivamente tambien a pdf_xref_init_entries
 * (mismo patron de multiples escrituras consecutivas, aqui por
 * elemento de array) por consistencia, aunque no se aislo un
 * reproductor especifico para esa funcion. */
static void pdf_xref_init_entries(pdf_xref_entry *entries, long first, long count)
{
    long i;
    long neg_one = -1, zero_l = 0;
    int  zero_i = 0;
    for (i = first; i < count; i++)
    {
        memcpy(&entries[i].offset, &neg_one, sizeof(neg_one));
        memcpy(&entries[i].gen, &zero_l, sizeof(zero_l));
        memcpy(&entries[i].in_use, &zero_i, sizeof(zero_i));
        memcpy(&entries[i].compressed, &zero_i, sizeof(zero_i));
        memcpy(&entries[i].objstm_num, &zero_l, sizeof(zero_l));
        memcpy(&entries[i].objstm_index, &zero_l, sizeof(zero_l));
    }
}

static int pdf_xref_ensure_capacity(pdf_arena *arena, pdf_xref_table *xref, long needed)
{
    pdf_xref_entry *new_entries;
    long new_capacity;
    size_t bytes;
    long new_count;

    if (arena == NULL || xref == NULL || needed < 0)
        return PDF_ERR_BADARG;
    if (needed > PDF_XREF_MAX_ENTRIES)
        return PDF_ERR_OVERFLOW;
    if (needed <= xref->capacity)
    {
        if (needed > xref->count)
        {
            memcpy(&xref->count, &needed, sizeof(needed));
        }
        return PDF_OK;
    }

    new_capacity = xref->capacity > 0 ? xref->capacity : 16;
    while (new_capacity < needed)
    {
        if (new_capacity > PDF_XREF_MAX_ENTRIES / 2)
        {
            new_capacity = PDF_XREF_MAX_ENTRIES;
            break;
        }
        new_capacity *= 2;
    }

    if (new_capacity < needed)
        return PDF_ERR_OVERFLOW;

    bytes = (size_t)new_capacity * sizeof(pdf_xref_entry);
    if ((size_t)new_capacity != 0 && bytes / sizeof(pdf_xref_entry) != (size_t)new_capacity)
        return PDF_ERR_OVERFLOW;

    new_entries = (pdf_xref_entry *)pdf_arena_alloc(arena, bytes);
    if (new_entries == NULL)
        return PDF_ERR_NOMEM;

    if (xref->entries != NULL && xref->count > 0)
        memcpy(new_entries, xref->entries, sizeof(pdf_xref_entry) * (size_t)xref->count);

    pdf_xref_init_entries(new_entries, xref->count, new_capacity);

    new_count = (needed > xref->count) ? needed : xref->count;
    memcpy(&xref->entries, &new_entries, sizeof(new_entries));
    memcpy(&xref->capacity, &new_capacity, sizeof(new_capacity));
    memcpy(&xref->count, &new_count, sizeof(new_count));
    return PDF_OK;
}


static long pdf_find_startxref_offset(pdf_stream *st)
{
    unsigned char buf[PDF_TAIL_SCAN_MAX];
    long size, start, got;
    long i;

    size = pdf_stream_size(st);
    start = size > PDF_TAIL_SCAN_MAX ? size - PDF_TAIL_SCAN_MAX : 0;

    pdf_stream_seek(st, start);
    got = pdf_stream_read(st, buf, size - start);

    /* buscar la ULTIMA ocurrencia de "startxref" en el bloque final */
    for (i = got - 9; i >= 0; i--)
    {
        if (memcmp(buf + i, "startxref", 9) == 0)
        {
            pdf_token tnum;
            pdf_stream_seek(st, start + i + 9);
            pdf_lex_next(st, &tnum);
            if (tnum.type == PDF_TOK_INT)
                return tnum.ival;
            return -1;
        }
    }
    return -1;
}

static int pdf_xref_load_section(pdf_stream *st, pdf_arena *arena,
                                  pdf_xref_table *xref, long offset);

static long pdf_read_be(const unsigned char *p, int n)
{
    long v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

/* Deshace un predictor PNG (Predictor >= 10 en /DecodeParms) sobre datos
 * ya inflados con Flate -- muy comun en xref streams y ObjStm (mejora
 * la compresion de datos tabulares/binarios). Cada fila viene precedida
 * de 1 byte que indica el filtro PNG realmente usado en ESA fila (0..4:
 * None/Sub/Up/Average/Paeth), sin importar el valor nominal de
 * /Predictor -- ese solo indica "se usaron filtros PNG", el filtro real
 * se lee por fila. */
static int pdf_undo_png_predictor(pdf_arena *arena, pdf_buf *buf, int columns)
{
    long row_bytes, stride, nrows, r;
    unsigned char *out;
    unsigned char *prev_row;
    const int bpp = 1; /* xref streams/ObjStm: Colors=1, BPC=8 (default) */

    if (columns <= 0) return PDF_ERR_BADARG;
    row_bytes = columns;
    stride = row_bytes + 1;
    if (stride <= 0 || buf->len % stride != 0) return PDF_ERR_BADARG;
    nrows = buf->len / stride;

    out = (unsigned char *)pdf_arena_alloc(arena, (size_t)(row_bytes * nrows));
    if (out == NULL) return PDF_ERR_NOMEM;
    prev_row = (unsigned char *)pdf_arena_alloc(arena, (size_t)row_bytes);
    if (prev_row == NULL) return PDF_ERR_NOMEM;
    memset(prev_row, 0, (size_t)row_bytes);

    for (r = 0; r < nrows; r++)
    {
        const unsigned char *in_row = buf->data + r * stride;
        int filter_type = in_row[0];
        const unsigned char *raw = in_row + 1;
        unsigned char *cur = out + r * row_bytes;
        long i;

        for (i = 0; i < row_bytes; i++)
        {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev_row[i];
            int c = (i >= bpp) ? prev_row[i - bpp] : 0;
            int x = raw[i];
            int val;

            switch (filter_type)
            {
                case 1: val = x + a; break;                 /* Sub */
                case 2: val = x + b; break;                 /* Up */
                case 3: val = x + (a + b) / 2; break;        /* Average */
                case 4:                                       /* Paeth */
                {
                    int p  = a + b - c;
                    int pa = (p > a) ? (p - a) : (a - p);
                    int pb = (p > b) ? (p - b) : (b - p);
                    int pc = (p > c) ? (p - c) : (c - p);
                    int pred = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
                    val = x + pred;
                    break;
                }
                default: val = x; break;                     /* None */
            }
            cur[i] = (unsigned char)(val & 0xFF);
        }

        memcpy(prev_row, cur, (size_t)row_bytes);
    }

    {
        long total_len = row_bytes * nrows;
        memcpy(&buf->data, &out, sizeof(out));
        memcpy(&buf->len, &total_len, sizeof(total_len));
    }
    return PDF_OK;
}

/* Decodifica el stream crudo de 'obj' (asumiendo FlateDecode, el caso
 * casi universal para xref streams/ObjStm; si no tiene filtro, se usan
 * los bytes crudos tal cual). Con limite defensivo de tamanio. Si el
 * stream declara /DecodeParms con /Predictor >= 10, deshace el
 * predictor PNG despues del inflate (ver pdf_undo_png_predictor). */
static int pdf_decode_stream_generic(pdf_stream *st, pdf_obj *obj, pdf_arena *arena,
                                      const pdf_crypt *crypt, pdf_buf *out)
{
    unsigned char *raw;
    long raw_len;
    const char *filter;
    int rc;

    if (obj == NULL || obj->type != PDF_STREAM) return PDF_ERR_BADARG;
    if (obj->u.stm.raw_length <= 0 || obj->u.stm.raw_length > 200L * 1024L * 1024L)
        return PDF_ERR_BADARG;

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)obj->u.stm.raw_length);
    if (raw == NULL) return PDF_ERR_NOMEM;
    pdf_stream_seek(st, obj->u.stm.raw_offset);
    raw_len = pdf_stream_read(st, raw, obj->u.stm.raw_length);

    /* Desencriptar ANTES de cualquier filtro -- la encriptacion es la
     * capa mas externa (se aplica despues de Flate/etc al guardar, asi
     * que hay que deshacerla primero al leer). 'crypt'==NULL se usa
     * para streams exentos de encriptacion (xref streams). */
    if (crypt != NULL && crypt->active)
        raw_len = pdf_crypt_decrypt(crypt, obj->u.stm.obj_num, obj->u.stm.obj_gen, raw, raw_len);

    filter = pdf_dict_get_name(obj, "Filter");
    if (filter != NULL && strcmp(filter, "FlateDecode") == 0)
        rc = pdf_filter_flate(arena, raw, raw_len, 0, out);
    else
    {
        memcpy(&out->data, &raw, sizeof(raw));
        memcpy(&out->len, &raw_len, sizeof(raw_len));
        rc = PDF_OK;
    }
    if (rc != PDF_OK)
        return rc;

    {
        pdf_obj *parms = pdf_dict_get(obj, "DecodeParms");
        if (parms != NULL && parms->type == PDF_DICT)
        {
            long predictor = pdf_dict_get_int(parms, "Predictor", 1);
            if (predictor >= 10)
            {
                int columns = (int)pdf_dict_get_int(parms, "Columns", 1);
                return pdf_undo_png_predictor(arena, out, columns);
            }
        }
    }

    return PDF_OK;
}

static int pdf_xref_load_stream_section(pdf_stream *st, pdf_arena *arena,
                                         pdf_xref_table *xref, long offset)
{
    long got_num, got_gen;
    pdf_obj *xobj;
    pdf_obj *w_arr, *index_arr;
    int w0, w1, w2, rec_size;
    pdf_buf decoded;
    long prev, pos;

    xobj = pdf_parse_indirect_object(st, offset, &got_num, &got_gen, arena, NULL);
    if (xobj == NULL || xobj->type != PDF_STREAM)
        return PDF_ERR_BADARG;

    w_arr = pdf_dict_get(xobj, "W");
    if (w_arr == NULL || w_arr->type != PDF_ARRAY || w_arr->u.arr.count < 3)
        return PDF_ERR_BADARG;
    w0 = (int)pdf_obj_num(w_arr->u.arr.items[0], 1.0);
    w1 = (int)pdf_obj_num(w_arr->u.arr.items[1], 1.0);
    w2 = (int)pdf_obj_num(w_arr->u.arr.items[2], 1.0);
    if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 4 || w1 > 4 || w2 > 4)
        return PDF_ERR_BADARG;
    rec_size = w0 + w1 + w2;
    if (rec_size <= 0)
        return PDF_ERR_BADARG;

    if (pdf_decode_stream_generic(st, xobj, arena, NULL, &decoded) != PDF_OK)
        return PDF_ERR_BADARG;

    {
        long size = pdf_dict_get_int(xobj, "Size", 0);
        int ensure_rc;
        if (size <= 0) return PDF_ERR_BADARG;
        ensure_rc = pdf_xref_ensure_capacity(arena, xref, size);
        if (ensure_rc != PDF_OK) return ensure_rc;
    }

    index_arr = pdf_dict_get(xobj, "Index");
    pos = 0;

    {
        /* sin /Index explicito: default es un unico par [0, Size] */
        long default_pair[2];
        long *pairs;
        int npairs, pi;

        if (index_arr != NULL && index_arr->type == PDF_ARRAY && index_arr->u.arr.count >= 2)
        {
            npairs = index_arr->u.arr.count / 2;
            pairs = NULL; /* se lee directo de index_arr abajo */
        }
        else
        {
            default_pair[0] = 0;
            default_pair[1] = xref->count;
            npairs = 1;
            pairs = default_pair;
        }

        for (pi = 0; pi < npairs; pi++)
        {
            long start, count, i;

            if (pairs != NULL)
            {
                start = pairs[0]; count = pairs[1];
            }
            else
            {
                start = (long)pdf_obj_num(index_arr->u.arr.items[pi * 2], 0.0);
                count = (long)pdf_obj_num(index_arr->u.arr.items[pi * 2 + 1], 0.0);
            }

            for (i = 0; i < count; i++)
            {
                long objnum = start + i;
                long f0, f1, f2;

                if (pos + rec_size > decoded.len)
                    break;

                f0 = (w0 > 0) ? pdf_read_be(decoded.data + pos, w0) : 1;
                f1 = pdf_read_be(decoded.data + pos + w0, w1);
                f2 = (w2 > 0) ? pdf_read_be(decoded.data + pos + w0 + w1, w2) : 0;
                pos += rec_size;

                if (objnum >= 0 && objnum < xref->count &&
                    xref->entries[objnum].offset == -1 && !xref->entries[objnum].compressed)
                {
                    /* BUG REAL ENCONTRADO (misma familia que el comentario
                     * grande de pdf_xref_init_entries, arriba, y que
                     * project_bcc32_field_write_bug_widespread en la
                     * memoria del proyecto): estas dos ramas escribian
                     * campos ADYACENTES de 'pdf_xref_entry' con asignacion
                     * directa consecutiva -- el patron exacto que bcc32
                     * 7.70 miscompila. Confirmado como causa raiz de un
                     * bug real: en un PDF con objetos comprimidos (ObjStm),
                     * 'objstm_index' (o 'objstm_num') podia quedar
                     * corrompido para entradas puntuales, haciendo que
                     * pdf_load_from_objstm() extrajera el objeto EQUIVOCADO
                     * dentro del ObjStm correcto -- sintoma observado:
                     * anchos de fuente (/Widths) con valores y longitud de
                     * array incorrectos para fuentes especificas, mientras
                     * otras fuentes del mismo documento (resueltas desde
                     * objetos NO comprimidos) parseaban bien. Mismo fix ya
                     * establecido en todo este archivo: memcpy() desde
                     * variables locales en vez de escritura directa. */
                    int one_i = 1;
                    if (f0 == 1)
                    {
                        memcpy(&xref->entries[objnum].offset, &f1, sizeof(f1));
                        memcpy(&xref->entries[objnum].gen, &f2, sizeof(f2));
                        memcpy(&xref->entries[objnum].in_use, &one_i, sizeof(one_i));
                    }
                    else if (f0 == 2)
                    {
                        memcpy(&xref->entries[objnum].compressed, &one_i, sizeof(one_i));
                        memcpy(&xref->entries[objnum].objstm_num, &f1, sizeof(f1));
                        memcpy(&xref->entries[objnum].objstm_index, &f2, sizeof(f2));
                        memcpy(&xref->entries[objnum].in_use, &one_i, sizeof(one_i));
                    }
                    /* f0 == 0: entrada libre, no hacer nada (offset queda -1) */
                }
            }
        }
    }

    if (xref->trailer == NULL)
        xref->trailer = xobj; /* el dict del propio xref stream sirve de "trailer" (tiene /Root, /Size, etc.) */

    prev = pdf_dict_get_int(xobj, "Prev", -1);
    if (prev >= 0)
    {
        /* /Prev puede apuntar a OTRO xref stream o a una tabla clasica
         * (PDFs hibridos) -- hay que volver a mirar que hay ahi. */
        pdf_token ptok;
        pdf_stream_seek(st, prev);
        pdf_lex_next(st, &ptok);
        if (tok_is_keyword(&ptok, "xref"))
            pdf_xref_load_section(st, arena, xref, prev);
        else
            pdf_xref_load_stream_section(st, arena, xref, prev);
    }

    return PDF_OK;
}

static int pdf_xref_load_section(pdf_stream *st, pdf_arena *arena,
                                  pdf_xref_table *xref, long offset)
{
    pdf_token tok;
    pdf_obj *trailer;
    long prev;

    if (pdf_stream_seek(st, offset) != PDF_OK)
        return PDF_ERR_BADARG;

    pdf_lex_next(st, &tok);
    if (!tok_is_keyword(&tok, "xref"))
        return PDF_ERR_BADARG;

    /* subsecciones: "start count" seguido de 'count' entradas */
    for (;;)
    {
        long pos_before, sub_start, sub_count, i;

        pos_before = pdf_stream_tell(st);
        pdf_lex_next(st, &tok);
        if (tok.type != PDF_TOK_INT)
        {
            pdf_stream_seek(st, pos_before);
            break; /* fin de subsecciones: sigue "trailer" */
        }
        sub_start = tok.ival;

        pdf_lex_next(st, &tok);
        if (tok.type != PDF_TOK_INT)
            break;
        sub_count = tok.ival;

        for (i = 0; i < sub_count; i++)
        {
            pdf_token t_off, t_gen, t_flag;
            long objnum = sub_start + i;

            pdf_lex_next(st, &t_off);
            pdf_lex_next(st, &t_gen);
            pdf_lex_next(st, &t_flag);

            if (t_off.type != PDF_TOK_INT || t_gen.type != PDF_TOK_INT)
                continue;

            if (objnum >= 0 && objnum < xref->count &&
                xref->entries[objnum].offset == -1 && !xref->entries[objnum].compressed)
            {
                /* BUG REAL ENCONTRADO -- ver comentario extenso junto a
                 * pdf_xref_ensure_capacity mas arriba: misma
                 * miscompilacion, aca en la tabla xref CLASICA (no
                 * stream) -- sitio de escritura separado, no cubierto
                 * por el fix de pdf_xref_init_entries. Confirmado que
                 * rompia la resolucion de pagina en PDFs chicos con
                 * tabla xref clasica (tests/rects.pdf). */
                long off_v = t_off.ival;
                long gen_v = t_gen.ival;
                int inuse_v = tok_is_keyword(&t_flag, "n");
                memcpy(&xref->entries[objnum].offset, &off_v, sizeof(off_v));
                memcpy(&xref->entries[objnum].gen, &gen_v, sizeof(gen_v));
                memcpy(&xref->entries[objnum].in_use, &inuse_v, sizeof(inuse_v));
            }
        }
    }

    pdf_lex_next(st, &tok);
    if (!tok_is_keyword(&tok, "trailer"))
        return PDF_ERR_BADARG;

    pdf_lex_next(st, &tok);
    trailer = pdf_parse_object(st, &tok, arena);
    if (trailer == NULL || trailer->type != PDF_DICT)
        return PDF_ERR_BADARG;

    if (xref->trailer == NULL)
        xref->trailer = trailer; /* el primero que aparece es el mas nuevo */

    /* /XRefStm: PDF hibrido -- ademas de la tabla clasica, hay un xref
     * stream con entradas adicionales (para compatibilidad con lectores
     * viejos que no entienden streams comprimidos). Si esta, seguirlo
     * ANTES que /Prev (asi lo exige el estandar). */
    {
        long xrefstm = pdf_dict_get_int(trailer, "XRefStm", -1);
        if (xrefstm >= 0)
            pdf_xref_load_stream_section(st, arena, xref, xrefstm);
    }

    prev = pdf_dict_get_int(trailer, "Prev", -1);
    if (prev >= 0)
    {
        pdf_token ptok;
        pdf_stream_seek(st, prev);
        pdf_lex_next(st, &ptok);
        if (tok_is_keyword(&ptok, "xref"))
            pdf_xref_load_section(st, arena, xref, prev);
        else
            pdf_xref_load_stream_section(st, arena, xref, prev);
    }

    return PDF_OK;
}

/* Busca /Encrypt en el trailer ya cargado y, si esta presente, inicializa
 * xref->crypt (ver pdf_crypt.h -- solo RC4 clasico V1/V2 R2/R3 con
 * contrasenia de usuario vacia; otros esquemas quedan con crypt.active=0,
 * documentado como no soportado en vez de fallar silenciosamente con
 * streams corruptos). */
static void pdf_xref_detect_encryption(pdf_stream *st, pdf_arena *arena, pdf_xref_table *xref)
{
    pdf_obj *encrypt_ref, *encrypt_dict, *id_arr, *id0;
    const unsigned char *id0_data;
    long id0_len;

    if (xref->trailer == NULL) return;

    encrypt_ref = pdf_dict_get(xref->trailer, "Encrypt");
    if (encrypt_ref == NULL) return;

    encrypt_dict = encrypt_ref;
    if (encrypt_ref->type == PDF_REF)
        encrypt_dict = pdf_parse_indirect_object(st, pdf_xref_offset(xref, encrypt_ref->u.ref.num),
                                                   NULL, NULL, arena, NULL);
    if (encrypt_dict == NULL) return;

    id0_data = NULL; id0_len = 0;
    id_arr = pdf_dict_get(xref->trailer, "ID");
    if (id_arr != NULL && id_arr->type == PDF_ARRAY && id_arr->u.arr.count >= 1)
    {
        id0 = id_arr->u.arr.items[0];
        if (id0 != NULL && id0->type == PDF_STRING)
        {
            id0_data = (const unsigned char *)id0->u.str.data;
            id0_len  = id0->u.str.len;
        }
    }

    pdf_crypt_init(&xref->crypt, encrypt_dict, id0_data, id0_len);
}

void pdf_xref_reset(pdf_xref_table *xref)
{
    pdf_xref_entry *null_entries = NULL;
    long zero_l = 0;
    pdf_obj *null_obj = NULL;
    int zero_i = 0;
    pdf_obj **null_resolved = NULL;
    pdf_arena *null_arena = NULL;

    if (xref == NULL)
        return;
    /* BUG REAL ENCONTRADO -- ver comentario extenso junto a
     * pdf_xref_ensure_capacity mas arriba: misma miscompilacion de
     * bcc32 7.70 con escrituras directas consecutivas a campos
     * adyacentes de 'pdf_xref_table'. */
    memcpy(&xref->entries, &null_entries, sizeof(null_entries));
    memcpy(&xref->count, &zero_l, sizeof(zero_l));
    memcpy(&xref->capacity, &zero_l, sizeof(zero_l));
    memcpy(&xref->trailer, &null_obj, sizeof(null_obj));
    memcpy(&xref->crypt.active, &zero_i, sizeof(zero_i));
    memcpy(&xref->resolved, &null_resolved, sizeof(null_resolved));
    memcpy(&xref->cache_arena, &null_arena, sizeof(null_arena));
}

/* Arma xref->resolved (todo NULL) una vez que xref->count es
 * definitivo, y fija xref->cache_arena a la arena de vida larga que
 * pdf_xref_load() recibio -- ver comentario grande en pdf_xref.h. Si
 * la asignacion falla (arena sin presupuesto), no es fatal: el cache
 * simplemente queda desactivado (pdf_xref_load_object detecta
 * xref->resolved==NULL y sigue funcionando exactamente como antes de
 * este cambio, solo sin la aceleracion). */
static void pdf_xref_init_resolved_cache(pdf_arena *arena, pdf_xref_table *xref)
{
    pdf_obj **resolved;
    size_t bytes;

    if (arena == NULL || xref == NULL || xref->count <= 0)
        return;

    bytes = sizeof(pdf_obj *) * (size_t)xref->count;
    resolved = (pdf_obj **)pdf_arena_alloc(arena, bytes);
    if (resolved == NULL)
        return;
    memset(resolved, 0, bytes);

    memcpy(&xref->resolved, &resolved, sizeof(resolved));
    memcpy(&xref->cache_arena, &arena, sizeof(arena));
}

int pdf_xref_load(pdf_stream *st, pdf_arena *arena, pdf_xref_table *xref)
{
    long startxref_offset;
    pdf_token tok;

    pdf_xref_reset(xref);

    startxref_offset = pdf_find_startxref_offset(st);
    if (startxref_offset < 0)
        return PDF_ERR_BADARG;

    /* mirar que hay en el offset para decidir tabla clasica vs xref
     * stream: la clasica arranca con la palabra "xref"; un xref stream
     * arranca como cualquier objeto indirecto, con un numero ("N G obj"). */
    if (pdf_stream_seek(st, startxref_offset) != PDF_OK)
        return PDF_ERR_BADARG;
    pdf_lex_next(st, &tok);

    if (tok_is_keyword(&tok, "xref"))
    {
        /* --- tabla clasica: primera pasada liviana para conocer /Size
         * y dimensionar el array de una sola vez. --------------------- */
        long size;
        pdf_obj *probe_trailer;
        for (;;)
        {
            long pos_before;
            pos_before = pdf_stream_tell(st);
            pdf_lex_next(st, &tok);
            if (tok.type != PDF_TOK_INT) { pdf_stream_seek(st, pos_before); break; }
            {
                long sub_count, k;
                pdf_lex_next(st, &tok);
                if (tok.type != PDF_TOK_INT) break;
                sub_count = tok.ival;
                for (k = 0; k < sub_count; k++)
                {
                    pdf_token a, b, c;
                    pdf_lex_next(st, &a); pdf_lex_next(st, &b); pdf_lex_next(st, &c);
                }
            }
        }
        pdf_lex_next(st, &tok);
        if (!tok_is_keyword(&tok, "trailer"))
            return PDF_ERR_BADARG;
        pdf_lex_next(st, &tok);
        probe_trailer = pdf_parse_object(st, &tok, arena);
        size = pdf_dict_get_int(probe_trailer, "Size", 0);
        if (size <= 0)
            return PDF_ERR_BADARG;

        if (pdf_xref_ensure_capacity(arena, xref, size) != PDF_OK)
            return PDF_ERR_NOMEM;

        {
            int rc = pdf_xref_load_section(st, arena, xref, startxref_offset);
            if (rc == PDF_OK)
            {
                pdf_xref_detect_encryption(st, arena, xref);
                pdf_xref_init_resolved_cache(arena, xref);
            }
            return rc;
        }
    }
    else if (tok.type == PDF_TOK_INT)
    {
        /* --- xref stream: se parsea el objeto completo directo, que ya
         * trae /Size en su propio dict -- no hace falta una pasada
         * separada solo para eso. ---------------------------------- */
        int rc = pdf_xref_load_stream_section(st, arena, xref, startxref_offset);
        if (rc == PDF_OK)
        {
            pdf_xref_detect_encryption(st, arena, xref);
            pdf_xref_init_resolved_cache(arena, xref);
        }
        return rc;
    }

    return PDF_ERR_BADARG;
}

long pdf_xref_offset(const pdf_xref_table *xref, long num)
{
    if (xref == NULL || xref->entries == NULL || num < 0 || num >= xref->count)
        return -1;
    if (!xref->entries[num].in_use || xref->entries[num].compressed)
        return -1;
    return xref->entries[num].offset;
}

const pdf_xref_entry *pdf_xref_entry_at(const pdf_xref_table *xref, long num)
{
    if (xref == NULL || xref->entries == NULL || num < 0 || num >= xref->count)
        return NULL;
    return &xref->entries[num];
}

/* Carga un objeto que vive DENTRO de un ObjStm (PDF_STREAM con
 * /Type /ObjStm): decodifica el ObjStm, ubica el offset del objeto
 * pedido en su tabla de cabecera ("objnum offset" x N, en ASCII), y lo
 * parsea con un pdf_stream de memoria (los objetos dentro de un ObjStm
 * NO tienen el envoltorio "N G obj", van directo al valor). */
static pdf_obj *pdf_load_from_objstm(pdf_stream *st, const pdf_xref_table *xref,
                                      long objstm_num, long index, pdf_arena *arena)
{
    pdf_obj *objstm;
    long n, first;
    pdf_buf decoded;
    pdf_stream mem_st;
    long i;
    long target_offset = -1;

    objstm = pdf_xref_load_object(st, xref, objstm_num, arena);
    if (objstm == NULL || objstm->type != PDF_STREAM)
        return NULL;

    n     = pdf_dict_get_int(objstm, "N", 0);
    first = pdf_dict_get_int(objstm, "First", 0);
    if (n <= 0 || n > 200000 || first < 0)
        return NULL;

    if (pdf_decode_stream_generic(st, objstm, arena, &xref->crypt, &decoded) != PDF_OK)
        return NULL;

    pdf_stream_open_memory(&mem_st, decoded.data, decoded.len);

    for (i = 0; i < n; i++)
    {
        pdf_token t_num, t_off;
        pdf_lex_next(&mem_st, &t_num);
        pdf_lex_next(&mem_st, &t_off);
        if (t_num.type != PDF_TOK_INT || t_off.type != PDF_TOK_INT)
            return NULL;
        if (i == index)
            target_offset = t_off.ival;
    }

    if (target_offset < 0 || first + target_offset >= decoded.len)
        return NULL;

    {
        pdf_token vtok;
        pdf_stream_seek(&mem_st, first + target_offset);
        pdf_lex_next(&mem_st, &vtok);
        return pdf_parse_object(&mem_st, &vtok, arena);
    }
}

pdf_obj *pdf_xref_load_object(pdf_stream *st, const pdf_xref_table *xref,
                              long num, pdf_arena *arena)
{
    long got_num, got_gen;
    pdf_obj *obj;
    pdf_arena *alloc_arena;

    if (xref == NULL || xref->entries == NULL || num < 0 || num >= xref->count)
        return NULL;
    if (!xref->entries[num].in_use)
        return NULL;

    /* Cache de resolucion (ver comentario grande en pdf_xref.h) --
     * misma identidad de puntero en cada resolucion repetida del mismo
     * numero, y evita volver a parsear desde cero. 'xref->resolved'
     * queda en NULL si pdf_xref_init_resolved_cache() no se pudo armar
     * (arena sin presupuesto) -- en ese caso este bloque simplemente
     * no aplica y todo sigue igual que antes de este cambio. */
    if (xref->resolved != NULL && xref->resolved[num] != NULL)
        return xref->resolved[num];

    /* Alocar SIEMPRE en la arena de cache (vida larga -- doc_arena en
     * la practica), NUNCA en la 'arena' que paso el llamador -- asi el
     * puntero que se guarda en el cache sigue siendo valido aunque el
     * llamador haya pasado una arena de pagina de vida corta (se libera
     * en pdf_page_close). Si el cache no esta activo, se respeta la
     * 'arena' del llamador como siempre. */
    alloc_arena = (xref->cache_arena != NULL) ? xref->cache_arena : arena;

    if (xref->entries[num].compressed)
    {
        obj = pdf_load_from_objstm(st, xref, xref->entries[num].objstm_num,
                                    xref->entries[num].objstm_index, alloc_arena);
    }
    else
    {
        if (xref->entries[num].offset < 0)
            return NULL;

        obj = pdf_parse_indirect_object(st, xref->entries[num].offset,
                                         &got_num, &got_gen, alloc_arena, xref);
        if (obj == NULL)
            return NULL;
        if (got_num != num || got_gen != xref->entries[num].gen)
            return NULL;
    }

    if (obj != NULL && xref->resolved != NULL)
        xref->resolved[num] = obj;

    return obj;
}
