/* pdf_cff.c
 *
 * Ver pdf_cff.h.
 */

#include "pdf_cff.h"
#include "pdf_error.h"
#include <string.h>
#include <math.h>

static unsigned int cff_u8(const unsigned char *p)
{
    return p[0];
}

static unsigned int cff_u16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

static long cff_off(const unsigned char *p, int sz)
{
    long v = 0;
    int i;
    for (i = 0; i < sz; i++)
        v = (v << 8) | p[i];
    return v;
}

/* ---- INDEX (Name/TopDICT/String/GlobalSubr/CharStrings) ------------- */

typedef struct cff_index_s
{
    long count;
    int  off_size;
    long offsets_start;
    long raw_data_start;
} cff_index;

static int cff_read_index(const unsigned char *data, long len, long pos,
                           cff_index *idx, long *end_pos)
{
    long count, offsets_start, raw_data_start, last_off;
    int off_size;

    if (pos + 2 > len) return 0;
    count = (long)cff_u16(data + pos);
    if (count == 0)
    {
        idx->count = 0;
        *end_pos = pos + 2;
        return 1;
    }

    if (pos + 3 > len) return 0;
    off_size = (int)cff_u8(data + pos + 2);
    if (off_size < 1 || off_size > 4) return 0;

    offsets_start = pos + 3;
    if (offsets_start + (count + 1) * off_size > len) return 0;

    raw_data_start = offsets_start + (count + 1) * off_size;
    last_off = cff_off(data + offsets_start + count * off_size, off_size);
    if (last_off < 1 || raw_data_start + last_off - 1 > len) return 0;

    idx->count = count;
    idx->off_size = off_size;
    idx->offsets_start = offsets_start;
    idx->raw_data_start = raw_data_start;
    *end_pos = raw_data_start + last_off - 1;
    return 1;
}

static int cff_index_get(const unsigned char *data, const cff_index *idx, long i,
                          const unsigned char **out_ptr, long *out_len)
{
    long o0, o1;
    if (i < 0 || i >= idx->count) return 0;
    o0 = cff_off(data + idx->offsets_start + i * idx->off_size, idx->off_size);
    o1 = cff_off(data + idx->offsets_start + (i + 1) * idx->off_size, idx->off_size);
    if (o1 < o0) return 0;
    *out_ptr = data + idx->raw_data_start + o0 - 1;
    *out_len = o1 - o0;
    return 1;
}

/* ---- Top DICT: solo nos interesan charset/Encoding/ROS -------------- */

typedef struct cff_top_dict_s
{
    long charset_off;
    long encoding_off;
    long charstrings_off;
    long private_size, private_off;
    double font_matrix[6];
    int  has_charset;
    int  has_encoding;
    int  has_charstrings;
    int  has_private;
    int  has_font_matrix;
    int  has_ros; /* CID-keyed: charset son CIDs, no nombres -- fuera de alcance */
} cff_top_dict;

#define CFF_MAX_DICT_OPERANDS 48

static void cff_parse_top_dict(const unsigned char *d, long dlen, cff_top_dict *out)
{
    long pos;
    double operands[CFF_MAX_DICT_OPERANDS];
    int n_operands;

    memset(out, 0, sizeof(*out));
    pos = 0;
    n_operands = 0;

    while (pos < dlen)
    {
        unsigned char b0 = d[pos];

        if (b0 <= 21)
        {
            int op = b0;
            pos++;
            if (b0 == 12)
            {
                if (pos >= dlen) break;
                op = 1200 + d[pos];
                pos++;
            }

            if (op == 15 && n_operands >= 1)
            {
                out->charset_off = (long)operands[n_operands - 1];
                out->has_charset = 1;
            }
            else if (op == 16 && n_operands >= 1)
            {
                out->encoding_off = (long)operands[n_operands - 1];
                out->has_encoding = 1;
            }
            else if (op == 17 && n_operands >= 1)
            {
                out->charstrings_off = (long)operands[n_operands - 1];
                out->has_charstrings = 1;
            }
            else if (op == 18 && n_operands >= 2)
            {
                out->private_size = (long)operands[n_operands - 2];
                out->private_off  = (long)operands[n_operands - 1];
                out->has_private = 1;
            }
            else if (op == 1207 && n_operands >= 6)
            {
                int k;
                for (k = 0; k < 6; k++)
                    out->font_matrix[k] = operands[n_operands - 6 + k];
                out->has_font_matrix = 1;
            }
            else if (op == 1230)
            {
                out->has_ros = 1;
            }

            n_operands = 0;
        }
        else if (b0 == 28)
        {
            if (pos + 3 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(short)(((int)d[pos + 1] << 8) | d[pos + 2]);
            pos += 3;
        }
        else if (b0 == 29)
        {
            long v;
            if (pos + 5 > dlen) break;
            v = ((long)d[pos + 1] << 24) | ((long)d[pos + 2] << 16) |
                ((long)d[pos + 3] << 8) | d[pos + 4];
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)v;
            pos += 5;
        }
        else if (b0 == 30)
        {
            /* numero real (nibbles hasta terminador 0xF): no nos hace
             * falta el valor (charset/Encoding/ROS son siempre
             * offsets/flags enteros), solo saltarlo. */
            pos++;
            while (pos < dlen)
            {
                unsigned char nb = d[pos++];
                if ((nb & 0x0F) == 0x0F || (nb >> 4) == 0x0F) break;
            }
            if (n_operands < CFF_MAX_DICT_OPERANDS) operands[n_operands++] = 0.0;
        }
        else if (b0 >= 32 && b0 <= 246)
        {
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)((int)b0 - 139);
            pos++;
        }
        else if (b0 >= 247 && b0 <= 250)
        {
            if (pos + 2 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(((int)b0 - 247) * 256 + d[pos + 1] + 108);
            pos += 2;
        }
        else if (b0 >= 251 && b0 <= 254)
        {
            if (pos + 2 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(-((int)b0 - 251) * 256 - d[pos + 1] - 108);
            pos += 2;
        }
        else
        {
            pos++; /* byte reservado (255) u otro invalido: saltar, tolerante */
        }
    }
}

/* Private DICT: solo nos interesa 'Subrs' (offset de las subrutinas
 * LOCALES, RELATIVO al inicio del Private DICT mismo -- a diferencia
 * de todos los demas offsets de este archivo, que son relativos al
 * inicio del programa CFF completo). Mismo parser de operandos que
 * cff_parse_top_dict (duplicado a proposito, en vez de generalizar con
 * una tabla de operadores de interes -- son solo 2 casos, no vale la
 * pena la indireccion). */
static void cff_parse_private_dict(const unsigned char *d, long dlen, long *out_subrs_off, int *has_subrs)
{
    long pos;
    double operands[CFF_MAX_DICT_OPERANDS];
    int n_operands;

    *out_subrs_off = 0;
    *has_subrs = 0;
    pos = 0;
    n_operands = 0;

    while (pos < dlen)
    {
        unsigned char b0 = d[pos];

        if (b0 <= 21)
        {
            int op = b0;
            pos++;
            if (b0 == 12)
            {
                if (pos >= dlen) break;
                op = 1200 + d[pos];
                pos++;
            }
            if (op == 19 && n_operands >= 1)
            {
                *out_subrs_off = (long)operands[n_operands - 1];
                *has_subrs = 1;
            }
            n_operands = 0;
        }
        else if (b0 == 28)
        {
            if (pos + 3 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(short)(((int)d[pos + 1] << 8) | d[pos + 2]);
            pos += 3;
        }
        else if (b0 == 29)
        {
            long v;
            if (pos + 5 > dlen) break;
            v = ((long)d[pos + 1] << 24) | ((long)d[pos + 2] << 16) |
                ((long)d[pos + 3] << 8) | d[pos + 4];
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)v;
            pos += 5;
        }
        else if (b0 == 30)
        {
            pos++;
            while (pos < dlen)
            {
                unsigned char nb = d[pos++];
                if ((nb & 0x0F) == 0x0F || (nb >> 4) == 0x0F) break;
            }
            if (n_operands < CFF_MAX_DICT_OPERANDS) operands[n_operands++] = 0.0;
        }
        else if (b0 >= 32 && b0 <= 246)
        {
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)((int)b0 - 139);
            pos++;
        }
        else if (b0 >= 247 && b0 <= 250)
        {
            if (pos + 2 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(((int)b0 - 247) * 256 + d[pos + 1] + 108);
            pos += 2;
        }
        else if (b0 >= 251 && b0 <= 254)
        {
            if (pos + 2 > dlen) break;
            if (n_operands < CFF_MAX_DICT_OPERANDS)
                operands[n_operands++] = (double)(-((int)b0 - 251) * 256 - d[pos + 1] - 108);
            pos += 2;
        }
        else
        {
            pos++;
        }
    }
}

static int cff_subr_bias(long count)
{
    if (count < 1240) return 107;
    if (count < 33900) return 1131;
    return 32768;
}

/* ---- Encoding: code -> GID (o directo code -> SID via supplements) -- */

static int cff_encoding_lookup(const unsigned char *data, long len, long eoff, int code,
                                int *out_gid, long *out_sid)
{
    unsigned char fmt;
    int has_sup;

    *out_gid = -1;
    *out_sid = -1;

    if (eoff < 0 || eoff >= len) return 0;
    fmt = data[eoff];
    has_sup = (fmt & 0x80) ? 1 : 0;
    fmt = (unsigned char)(fmt & 0x7F);

    if (fmt == 0)
    {
        int nCodes, i;
        long sup_off;

        if (eoff + 1 >= len) return 0;
        nCodes = data[eoff + 1];

        for (i = 0; i < nCodes; i++)
        {
            if (eoff + 2 + i >= len) break;
            if ((int)data[eoff + 2 + i] == code) { *out_gid = i + 1; break; }
        }

        if (has_sup && *out_gid < 0)
        {
            sup_off = eoff + 2 + nCodes;
            if (sup_off < len)
            {
                int nSups = data[sup_off], j;
                for (j = 0; j < nSups; j++)
                {
                    long rec = sup_off + 1 + (long)j * 3;
                    if (rec + 3 > len) break;
                    if ((int)data[rec] == code) { *out_sid = (long)cff_u16(data + rec + 1); break; }
                }
            }
        }
        return (*out_gid >= 0 || *out_sid >= 0);
    }

    if (fmt == 1)
    {
        int nRanges, i, running_gid;
        long sup_off;

        if (eoff + 1 >= len) return 0;
        nRanges = data[eoff + 1];
        running_gid = 1;

        for (i = 0; i < nRanges; i++)
        {
            long rec = eoff + 2 + (long)i * 2;
            int first, nLeft, k;
            if (rec + 2 > len) break;
            first = data[rec];
            nLeft = data[rec + 1];
            for (k = 0; k <= nLeft; k++)
                if (first + k == code) { *out_gid = running_gid + k; break; }
            running_gid += nLeft + 1;
            if (*out_gid >= 0) break;
        }

        if (has_sup && *out_gid < 0)
        {
            sup_off = eoff + 2 + (long)nRanges * 2;
            if (sup_off < len)
            {
                int nSups = data[sup_off], j;
                for (j = 0; j < nSups; j++)
                {
                    long rec = sup_off + 1 + (long)j * 3;
                    if (rec + 3 > len) break;
                    if ((int)data[rec] == code) { *out_sid = (long)cff_u16(data + rec + 1); break; }
                }
            }
        }
        return (*out_gid >= 0 || *out_sid >= 0);
    }

    return 0;
}

/* ---- Charset: GID -> SID (formato 0/1/2) ----------------------------- */

static int cff_charset_lookup(const unsigned char *data, long len, long coff, int gid, long *out_sid)
{
    unsigned char fmt;

    *out_sid = -1;
    if (coff < 0 || coff >= len) return 0;
    fmt = data[coff];

    if (fmt == 0)
    {
        long rec = coff + 1 + (long)(gid - 1) * 2;
        if (rec + 2 > len) return 0;
        *out_sid = (long)cff_u16(data + rec);
        return 1;
    }

    if (fmt == 1 || fmt == 2)
    {
        long p = coff + 1;
        int running_gid = 1;
        int entry_size = (fmt == 1) ? 3 : 4;

        while (p + entry_size <= len)
        {
            int first_sid = (int)cff_u16(data + p);
            long nLeft = (fmt == 1) ? (long)data[p + 2] : (long)cff_u16(data + p + 2);

            if (gid >= running_gid && gid <= running_gid + nLeft)
            {
                *out_sid = first_sid + (gid - running_gid);
                return 1;
            }
            running_gid += (int)(nLeft + 1);
            p += entry_size;
            if (running_gid > gid + 1) break; /* ya lo pasamos: no esta */
        }
        return 0;
    }

    return 0;
}

/* Standard Strings de CFF (T.5176.CFF.pdf, Appendix A), SID 96-149 --
 * exactamente el tramo donde viven ligaduras ("fi","fl") y simbolos
 * tipograficos comunes (comillas curvas, guiones em/en, diacriticos
 * sueltos, letras escandinavas AE/OE/Lslash/Oslash) que
 * WinAnsiEncoding NO cubre igual en las mismas posiciones de codigo.
 * SID 1-95 (letras/digitos/puntuacion basica de ASCII) y 150-390 (el
 * resto de la lista completa de 391) no se tabulan aca a proposito --
 * ver comentario junto al uso, en pdf_cff_glyph_name_for_code. */
static const char *CFF_STD_STRINGS_96_149[] = {
    "exclamdown", "cent", "sterling", "fraction", "yen",
    "florin", "section", "currency", "quotesingle", "quotedblleft",
    "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl",
    "endash", "dagger", "daggerdbl", "periodcentered", "paragraph",
    "bullet", "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright",
    "ellipsis", "perthousand", "questiondown", "grave", "acute",
    "circumflex", "tilde", "macron", "breve", "dotaccent",
    "dieresis", "ring", "cedilla", "hungarumlaut", "ogonek",
    "caron", "emdash", "AE", "ordfeminine", "Lslash",
    "Oslash", "OE", "ordmasculine", "ae", "dotlessi",
    "lslash", "oslash", "oe", "germandbls"
};
#define CFF_STD_STRINGS_96_149_COUNT \
    (sizeof(CFF_STD_STRINGS_96_149) / sizeof(CFF_STD_STRINGS_96_149[0]))

int pdf_cff_glyph_name_for_code(const unsigned char *data, long len,
                                 int code, char *out, int out_size)
{
    long pos, end;
    cff_index name_idx, top_idx, string_idx;
    cff_top_dict top;
    const unsigned char *top_dict_data;
    long top_dict_len;
    int hdr_size;
    int gid;
    long sid;

    if (data == NULL || len < 4 || code < 0 || code > 255 || out == NULL || out_size <= 0)
        return 0;

    hdr_size = (int)cff_u8(data + 2);
    pos = hdr_size;

    if (!cff_read_index(data, len, pos, &name_idx, &end)) return 0;
    pos = end;
    if (!cff_read_index(data, len, pos, &top_idx, &end)) return 0;
    pos = end;
    if (!cff_read_index(data, len, pos, &string_idx, &end)) return 0;

    if (!cff_index_get(data, &top_idx, 0, &top_dict_data, &top_dict_len)) return 0;
    cff_parse_top_dict(top_dict_data, top_dict_len, &top);

    if (top.has_ros) return 0;
    if (!top.has_charset || !top.has_encoding) return 0;
    if (top.encoding_off == 0 || top.encoding_off == 1) return 0; /* Standard/Expert predefinido */
    if (top.charset_off == 0 || top.charset_off == 1 || top.charset_off == 2) return 0; /* predefinido */

    gid = -1;
    sid = -1;
    if (!cff_encoding_lookup(data, len, top.encoding_off, code, &gid, &sid)) return 0;

    if (sid < 0)
    {
        if (gid < 0) return 0;
        if (!cff_charset_lookup(data, len, top.charset_off, gid, &sid)) return 0;
    }

    /* BUG REAL ENCONTRADO (confirmado contra
     * Utilization_and_efficiency_of_ground_gra.pdf): el SID resuelto
     * para la ligadura "ffi"/"fi" en un caso real dio 109 -- un SID
     * ESTANDAR de CFF (Appendix A del spec, "fi"), NO un nombre CUSTOM
     * de esta fuente (SID>=391, ver comentario de archivo). Devolver 0
     * a ciegas para TODO SID<391 (como hacia la primera version de
     * este modulo) descartaba justo el caso que se buscaba resolver.
     * SID<96 son letras/digitos/puntuacion basica de ASCII (Standard
     * Strings 1-95) que ya resuelven bien via WinAnsiEncoding directo
     * -- no hace falta tabularlos aca. SID 96-149 (Standard Strings,
     * Appendix A) es justo donde viven ligaduras y simbolos
     * tipograficos que WinAnsiEncoding NO cubre igual -- se tabulan
     * abajo (CFF_STD_STRINGS_96_149). SID 150-390 no estan tabulados
     * (mejor no resolver que arriesgar un nombre mal transcripto de
     * memoria) -- tolerante, el llamador simplemente no completa ese
     * codigo. */
    if (sid >= 391)
    {
        const unsigned char *sptr;
        long slen;
        if (!cff_index_get(data, &string_idx, sid - 391, &sptr, &slen)) return 0;
        if (slen <= 0 || slen >= out_size) return 0;
        memcpy(out, sptr, (size_t)slen);
        out[slen] = '\0';
        return 1;
    }

    {
        long idx = sid - 96;
        const char *nm;
        size_t nl;

        if (idx < 0 || idx >= (long)CFF_STD_STRINGS_96_149_COUNT) return 0;
        nm = CFF_STD_STRINGS_96_149[idx];
        nl = strlen(nm);
        if (nl >= (size_t)out_size) return 0;
        memcpy(out, nm, nl + 1);
        return 1;
    }
}

/* ======================================================================
 * Contornos reales (Type 2 charstrings) -- ver comentario largo junto a
 * la declaracion en pdf_cff.h.
 * ====================================================================== */

int pdf_cff_load(const unsigned char *data, long len, pdf_cff_font *out)
{
    long pos, end;
    cff_index name_idx, top_idx, string_idx, gsubr_idx, cs_idx;
    cff_top_dict top;
    const unsigned char *top_dict_data;
    long top_dict_len;
    int hdr_size;

    if (data == NULL || out == NULL || len < 4) return PDF_ERR_BADARG;
    memset(out, 0, sizeof(*out));

    hdr_size = (int)cff_u8(data + 2);
    pos = hdr_size;

    if (!cff_read_index(data, len, pos, &name_idx, &end)) return PDF_ERR_NOTFOUND;
    pos = end;
    if (!cff_read_index(data, len, pos, &top_idx, &end)) return PDF_ERR_NOTFOUND;
    pos = end;
    if (!cff_read_index(data, len, pos, &string_idx, &end)) return PDF_ERR_NOTFOUND;
    pos = end;
    if (!cff_read_index(data, len, pos, &gsubr_idx, &end)) return PDF_ERR_NOTFOUND;

    if (!cff_index_get(data, &top_idx, 0, &top_dict_data, &top_dict_len)) return PDF_ERR_NOTFOUND;
    cff_parse_top_dict(top_dict_data, top_dict_len, &top);

    if (top.has_ros) return PDF_ERR_UNSUPPORTED; /* CID-keyed: FDArray/FDSelect, fuera de alcance */
    if (!top.has_charstrings) return PDF_ERR_NOTFOUND;
    if (!top.has_charset || !top.has_encoding) return PDF_ERR_UNSUPPORTED;
    if (top.encoding_off == 0 || top.encoding_off == 1) return PDF_ERR_UNSUPPORTED; /* predefinido */
    if (top.charset_off == 0 || top.charset_off == 1 || top.charset_off == 2) return PDF_ERR_UNSUPPORTED;

    if (!cff_read_index(data, len, top.charstrings_off, &cs_idx, &end)) return PDF_ERR_NOTFOUND;

    out->data = data;
    out->data_len = len;
    out->cs_count = cs_idx.count;
    out->cs_off_size = cs_idx.off_size;
    out->cs_offsets_start = cs_idx.offsets_start;
    out->cs_data_start = cs_idx.raw_data_start;
    out->gs_count = gsubr_idx.count;
    out->gs_off_size = gsubr_idx.off_size;
    out->gs_offsets_start = gsubr_idx.offsets_start;
    out->gs_data_start = gsubr_idx.raw_data_start;
    out->gs_bias = cff_subr_bias(gsubr_idx.count);
    out->charset_off = top.charset_off;
    out->encoding_off = top.encoding_off;
    out->num_glyphs = (int)cs_idx.count;
    out->units_per_em = 1000.0;
    if (top.has_font_matrix && top.font_matrix[0] > 0.0)
        out->units_per_em = 1.0 / top.font_matrix[0];

    if (top.has_private && top.private_size > 0 &&
        top.private_off >= 0 && top.private_off + top.private_size <= len)
    {
        long subrs_off_rel;
        int has_subrs;
        cff_parse_private_dict(data + top.private_off, top.private_size, &subrs_off_rel, &has_subrs);
        if (has_subrs)
        {
            cff_index lsubr_idx;
            long absolute = top.private_off + subrs_off_rel;
            if (cff_read_index(data, len, absolute, &lsubr_idx, &end))
            {
                out->ls_count = lsubr_idx.count;
                out->ls_off_size = lsubr_idx.off_size;
                out->ls_offsets_start = lsubr_idx.offsets_start;
                out->ls_data_start = lsubr_idx.raw_data_start;
                out->ls_bias = cff_subr_bias(lsubr_idx.count);
            }
        }
    }

    return PDF_OK;
}

int pdf_cff_gid_for_code(const pdf_cff_font *font, int code)
{
    int gid;
    long sid;

    if (font == NULL || font->data == NULL) return 0;
    gid = -1;
    sid = -1;
    if (!cff_encoding_lookup(font->data, font->data_len, font->encoding_off, code, &gid, &sid))
        return 0;
    if (gid >= 0) return gid;

    /* Encoding solo dio un SID directo (via supplements), no un GID --
     * hace falta la busqueda inversa en el Charset (GID->SID) para
     * encontrar el GID cuyo SID coincide. O(numGlyphs), pero esta rama
     * es rara en la practica (la mayoria de las fuentes reales no usa
     * supplements de Encoding) y se ejecuta como mucho una vez por
     * caracter, no por frame. */
    {
        int g;
        for (g = 1; g < font->num_glyphs; g++)
        {
            long gsid;
            if (cff_charset_lookup(font->data, font->data_len, font->charset_off, g, &gsid) && gsid == sid)
                return g;
        }
    }
    return 0;
}

static int cff_get_charstring(const pdf_cff_font *font, int gid, const unsigned char **out_ptr, long *out_len)
{
    cff_index idx;
    idx.count = font->cs_count;
    idx.off_size = font->cs_off_size;
    idx.offsets_start = font->cs_offsets_start;
    idx.raw_data_start = font->cs_data_start;
    return cff_index_get(font->data, &idx, gid, out_ptr, out_len);
}

static int cff_get_global_subr(const pdf_cff_font *font, int idx_biased, const unsigned char **out_ptr, long *out_len)
{
    cff_index idx;
    idx.count = font->gs_count;
    idx.off_size = font->gs_off_size;
    idx.offsets_start = font->gs_offsets_start;
    idx.raw_data_start = font->gs_data_start;
    return cff_index_get(font->data, &idx, idx_biased, out_ptr, out_len);
}

static int cff_get_local_subr(const pdf_cff_font *font, int idx_biased, const unsigned char **out_ptr, long *out_len)
{
    cff_index idx;
    idx.count = font->ls_count;
    idx.off_size = font->ls_off_size;
    idx.offsets_start = font->ls_offsets_start;
    idx.raw_data_start = font->ls_data_start;
    return cff_index_get(font->data, &idx, idx_biased, out_ptr, out_len);
}

/* ---- interprete de Type 2 charstrings --------------------------------- */

#define CFF_MAX_STACK      48
#define CFF_MAX_CALL_DEPTH 10

typedef struct cff_exec_s
{
    const pdf_cff_font *font;
    pdf_cff_moveto_fn   moveto;
    pdf_cff_lineto_fn   lineto;
    pdf_cff_curveto_fn  curveto;
    void               *user;
    double              inv_upm;

    double stack[CFF_MAX_STACK];
    int    nstack;

    double x, y;
    int    nstems;
    int    width_done;
    int    depth;
    int    done;
    int    had_error;
} cff_exec;

static void cff_emit_moveto(cff_exec *e, double x, double y)
{
    e->x = x; e->y = y;
    e->moveto(e->user, x * e->inv_upm, y * e->inv_upm);
}

static void cff_emit_lineto(cff_exec *e, double x, double y)
{
    e->x = x; e->y = y;
    e->lineto(e->user, x * e->inv_upm, y * e->inv_upm);
}

static void cff_emit_curveto(cff_exec *e, double x1, double y1, double x2, double y2, double x3, double y3)
{
    e->x = x3; e->y = y3;
    e->curveto(e->user, x1 * e->inv_upm, y1 * e->inv_upm, x2 * e->inv_upm, y2 * e->inv_upm,
               x3 * e->inv_upm, y3 * e->inv_upm);
}

/* Descarta el operando inicial "extra" (el ancho del glyph, ver
 * Private DICT nominalWidthX/defaultWidthX -- NO nos interesa el
 * VALOR, este motor usa /Widths del PDF para el avance, ver
 * pdf_render.c) si esta presente. Solo aplica UNA vez por glyph, en el
 * primer operador que limpia la pila. 'expected' es la cantidad real
 * de operandos que esa operacion necesita. */
static void cff_drop_width_if(cff_exec *e, int expected)
{
    if (e->width_done) return;
    e->width_done = 1;
    if (e->nstack > expected)
    {
        int extra = e->nstack - expected, i;
        for (i = extra; i < e->nstack; i++) e->stack[i - extra] = e->stack[i];
        e->nstack -= extra;
    }
}

static void cff_stems(cff_exec *e)
{
    if (!e->width_done)
    {
        e->width_done = 1;
        if ((e->nstack % 2) == 1)
        {
            int i;
            for (i = 1; i < e->nstack; i++) e->stack[i - 1] = e->stack[i];
            e->nstack--;
        }
    }
    e->nstems += e->nstack / 2;
    e->nstack = 0;
}

static int cff_run(cff_exec *e, const unsigned char *code, long code_len)
{
    long pos = 0;

    if (e->depth > CFF_MAX_CALL_DEPTH) return 0;

    while (pos < code_len && !e->done && !e->had_error)
    {
        unsigned char b0 = code[pos];

        if (b0 >= 32 || b0 == 28)
        {
            double v;
            if (b0 == 28)
            {
                if (pos + 3 > code_len) { e->had_error = 1; break; }
                v = (double)(short)(((int)code[pos + 1] << 8) | code[pos + 2]);
                pos += 3;
            }
            else if (b0 <= 246) { v = (double)((int)b0 - 139); pos += 1; }
            else if (b0 <= 250)
            {
                if (pos + 2 > code_len) { e->had_error = 1; break; }
                v = (double)(((int)b0 - 247) * 256 + code[pos + 1] + 108);
                pos += 2;
            }
            else if (b0 <= 254)
            {
                if (pos + 2 > code_len) { e->had_error = 1; break; }
                v = (double)(-((int)b0 - 251) * 256 - code[pos + 1] - 108);
                pos += 2;
            }
            else
            {
                long raw;
                if (pos + 5 > code_len) { e->had_error = 1; break; }
                raw = ((long)code[pos + 1] << 24) | ((long)code[pos + 2] << 16) |
                      ((long)code[pos + 3] << 8) | code[pos + 4];
                v = (double)raw / 65536.0;
                pos += 5;
            }
            if (e->nstack < CFF_MAX_STACK) e->stack[e->nstack++] = v;
            continue;
        }

        pos++;
        {
            int op = b0;
            if (b0 == 12)
            {
                if (pos >= code_len) { e->had_error = 1; break; }
                op = 1200 + code[pos];
                pos++;
            }

            if (op == 1 || op == 3 || op == 18 || op == 23) /* h/vstem(hm) */
            {
                cff_stems(e);
            }
            else if (op == 19 || op == 20) /* hintmask / cntrmask */
            {
                long nbytes;
                cff_stems(e);
                nbytes = (e->nstems + 7) / 8;
                if (nbytes < 1) nbytes = 1;
                pos += nbytes;
                if (pos > code_len) { e->had_error = 1; break; }
            }
            else if (op == 21) /* rmoveto */
            {
                cff_drop_width_if(e, 2);
                if (e->nstack >= 2) cff_emit_moveto(e, e->x + e->stack[0], e->y + e->stack[1]);
                e->nstack = 0;
            }
            else if (op == 22) /* hmoveto */
            {
                cff_drop_width_if(e, 1);
                if (e->nstack >= 1) cff_emit_moveto(e, e->x + e->stack[0], e->y);
                e->nstack = 0;
            }
            else if (op == 4) /* vmoveto */
            {
                cff_drop_width_if(e, 1);
                if (e->nstack >= 1) cff_emit_moveto(e, e->x, e->y + e->stack[0]);
                e->nstack = 0;
            }
            else if (op == 5) /* rlineto */
            {
                int i;
                for (i = 0; i + 1 < e->nstack; i += 2)
                    cff_emit_lineto(e, e->x + e->stack[i], e->y + e->stack[i + 1]);
                e->nstack = 0;
            }
            else if (op == 6 || op == 7) /* hlineto / vlineto */
            {
                int i, horiz = (op == 6);
                for (i = 0; i < e->nstack; i++)
                {
                    if (horiz) cff_emit_lineto(e, e->x + e->stack[i], e->y);
                    else       cff_emit_lineto(e, e->x, e->y + e->stack[i]);
                    horiz = !horiz;
                }
                e->nstack = 0;
            }
            else if (op == 8) /* rrcurveto */
            {
                int i;
                for (i = 0; i + 5 < e->nstack; i += 6)
                {
                    double x1 = e->x + e->stack[i],     y1 = e->y + e->stack[i + 1];
                    double x2 = x1 + e->stack[i + 2],   y2 = y1 + e->stack[i + 3];
                    double x3 = x2 + e->stack[i + 4],   y3 = y2 + e->stack[i + 5];
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                }
                e->nstack = 0;
            }
            else if (op == 24) /* rcurveline */
            {
                int i, n = e->nstack - 2;
                for (i = 0; i + 5 < n; i += 6)
                {
                    double x1 = e->x + e->stack[i],     y1 = e->y + e->stack[i + 1];
                    double x2 = x1 + e->stack[i + 2],   y2 = y1 + e->stack[i + 3];
                    double x3 = x2 + e->stack[i + 4],   y3 = y2 + e->stack[i + 5];
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                }
                if (e->nstack - i >= 2)
                    cff_emit_lineto(e, e->x + e->stack[i], e->y + e->stack[i + 1]);
                e->nstack = 0;
            }
            else if (op == 25) /* rlinecurve */
            {
                int i, n = e->nstack - 6;
                for (i = 0; i + 1 < n; i += 2)
                    cff_emit_lineto(e, e->x + e->stack[i], e->y + e->stack[i + 1]);
                if (e->nstack - i >= 6)
                {
                    double x1 = e->x + e->stack[i],     y1 = e->y + e->stack[i + 1];
                    double x2 = x1 + e->stack[i + 2],   y2 = y1 + e->stack[i + 3];
                    double x3 = x2 + e->stack[i + 4],   y3 = y2 + e->stack[i + 5];
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                }
                e->nstack = 0;
            }
            else if (op == 26) /* vvcurveto: dx1? {dya dxb dyb dyc}+ */
            {
                int i = 0;
                double dx1 = 0.0;
                if ((e->nstack % 4) == 1) { dx1 = e->stack[0]; i = 1; }
                for (; i + 3 < e->nstack; i += 4)
                {
                    double x1 = e->x + dx1,             y1 = e->y + e->stack[i];
                    double x2 = x1 + e->stack[i + 1],   y2 = y1 + e->stack[i + 2];
                    double x3 = x2,                     y3 = y2 + e->stack[i + 3];
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    dx1 = 0.0;
                }
                e->nstack = 0;
            }
            else if (op == 27) /* hhcurveto: dy1? {dxa dxb dyb dxc}+ */
            {
                int i = 0;
                double dy1 = 0.0;
                if ((e->nstack % 4) == 1) { dy1 = e->stack[0]; i = 1; }
                for (; i + 3 < e->nstack; i += 4)
                {
                    double x1 = e->x + e->stack[i],     y1 = e->y + dy1;
                    double x2 = x1 + e->stack[i + 1],   y2 = y1 + e->stack[i + 2];
                    double x3 = x2 + e->stack[i + 3],   y3 = y2;
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    dy1 = 0.0;
                }
                e->nstack = 0;
            }
            else if (op == 30 || op == 31) /* vhcurveto / hvcurveto */
            {
                int i = 0;
                int horiz = (op == 31);
                while (e->nstack - i >= 4)
                {
                    int remaining = e->nstack - i;
                    int last = (remaining == 5);
                    double x1, y1, x2, y2, x3, y3;
                    if (horiz)
                    {
                        x1 = e->x + e->stack[i];       y1 = e->y;
                        x2 = x1 + e->stack[i + 1];     y2 = y1 + e->stack[i + 2];
                        y3 = y2 + e->stack[i + 3];
                        x3 = last ? x2 + e->stack[i + 4] : x2;
                    }
                    else
                    {
                        x1 = e->x;                     y1 = e->y + e->stack[i];
                        x2 = x1 + e->stack[i + 1];     y2 = y1 + e->stack[i + 2];
                        x3 = x2 + e->stack[i + 3];
                        y3 = last ? y2 + e->stack[i + 4] : y2;
                    }
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    i += last ? 5 : 4;
                    horiz = !horiz;
                }
                e->nstack = 0;
            }
            else if (op == 1234) /* hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
            {
                if (e->nstack >= 7)
                {
                    double dx1 = e->stack[0], dx2 = e->stack[1], dy2 = e->stack[2], dx3 = e->stack[3],
                           dx4 = e->stack[4], dx5 = e->stack[5], dx6 = e->stack[6];
                    double x1 = e->x + dx1,   y1 = e->y;
                    double x2 = x1 + dx2,     y2 = y1 + dy2;
                    double x3 = x2 + dx3,     y3 = y2;
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    {
                        double x4 = e->x + dx4, y4 = e->y;
                        double x5 = x4 + dx5,   y5 = y4 - dy2;
                        double x6 = x5 + dx6,   y6 = y5;
                        cff_emit_curveto(e, x4, y4, x5, y5, x6, y6);
                    }
                }
                e->nstack = 0;
            }
            else if (op == 1235) /* flex: ...12 args + fd (ignorado) */
            {
                if (e->nstack >= 12)
                {
                    double x1 = e->x + e->stack[0],   y1 = e->y + e->stack[1];
                    double x2 = x1 + e->stack[2],     y2 = y1 + e->stack[3];
                    double x3 = x2 + e->stack[4],     y3 = y2 + e->stack[5];
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    {
                        double x4 = e->x + e->stack[6],   y4 = e->y + e->stack[7];
                        double x5 = x4 + e->stack[8],     y5 = y4 + e->stack[9];
                        double x6 = x5 + e->stack[10],    y6 = y5 + e->stack[11];
                        cff_emit_curveto(e, x4, y4, x5, y5, x6, y6);
                    }
                }
                e->nstack = 0;
            }
            else if (op == 1236) /* hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6 */
            {
                if (e->nstack >= 9)
                {
                    double y_start = e->y;
                    double dx1 = e->stack[0], dy1 = e->stack[1], dx2 = e->stack[2], dy2 = e->stack[3],
                           dx3 = e->stack[4], dx4 = e->stack[5], dx5 = e->stack[6], dy5 = e->stack[7],
                           dx6 = e->stack[8];
                    double x1 = e->x + dx1,   y1 = e->y + dy1;
                    double x2 = x1 + dx2,     y2 = y1 + dy2;
                    double x3 = x2 + dx3,     y3 = y2;
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    {
                        double x4 = e->x + dx4, y4 = e->y;
                        double x5 = x4 + dx5,   y5 = y4 + dy5;
                        double x6 = x5 + dx6,   y6 = y_start;
                        cff_emit_curveto(e, x4, y4, x5, y5, x6, y6);
                    }
                }
                e->nstack = 0;
            }
            else if (op == 1237) /* flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6 */
            {
                if (e->nstack >= 11)
                {
                    double x_start = e->x, y_start = e->y;
                    double dx1 = e->stack[0], dy1 = e->stack[1], dx2 = e->stack[2], dy2 = e->stack[3],
                           dx3 = e->stack[4], dy3 = e->stack[5], dx4 = e->stack[6], dy4 = e->stack[7],
                           dx5 = e->stack[8], dy5 = e->stack[9], d6 = e->stack[10];
                    double sum_dx = dx1 + dx2 + dx3 + dx4 + dx5;
                    double sum_dy = dy1 + dy2 + dy3 + dy4 + dy5;
                    double x1 = e->x + dx1,   y1 = e->y + dy1;
                    double x2 = x1 + dx2,     y2 = y1 + dy2;
                    double x3 = x2 + dx3,     y3 = y2 + dy3;
                    cff_emit_curveto(e, x1, y1, x2, y2, x3, y3);
                    {
                        double x4 = e->x + dx4, y4 = e->y + dy4;
                        double x5 = x4 + dx5,   y5 = y4 + dy5;
                        double x6, y6;
                        if (fabs(sum_dx) > fabs(sum_dy)) { x6 = x5 + d6; y6 = y_start; }
                        else                               { x6 = x_start; y6 = y5 + d6; }
                        cff_emit_curveto(e, x4, y4, x5, y5, x6, y6);
                    }
                }
                e->nstack = 0;
            }
            else if (op == 10) /* callsubr (local) */
            {
                if (e->nstack >= 1)
                {
                    int idx = (int)e->stack[--e->nstack] + e->font->ls_bias;
                    const unsigned char *sptr; long slen;
                    if (e->depth < CFF_MAX_CALL_DEPTH && cff_get_local_subr(e->font, idx, &sptr, &slen))
                    {
                        e->depth++;
                        cff_run(e, sptr, slen);
                        e->depth--;
                    }
                }
            }
            else if (op == 29) /* callgsubr (global) */
            {
                if (e->nstack >= 1)
                {
                    int idx = (int)e->stack[--e->nstack] + e->font->gs_bias;
                    const unsigned char *sptr; long slen;
                    if (e->depth < CFF_MAX_CALL_DEPTH && cff_get_global_subr(e->font, idx, &sptr, &slen))
                    {
                        e->depth++;
                        cff_run(e, sptr, slen);
                        e->depth--;
                    }
                }
            }
            else if (op == 11) /* return */
            {
                return 1;
            }
            else if (op == 14) /* endchar */
            {
                /* seac-like (4 argumentos: composicion antigua de
                 * acentos) NO se implementa a proposito -- deprecado y
                 * rarisimo en fuentes reales modernas (ver pdf_cff.h).
                 * Se descarta el ancho opcional (1 o 5 argumentos) y
                 * se termina el glyph tolerantemente. */
                if (!e->width_done)
                {
                    e->width_done = 1;
                    if (e->nstack == 1 || e->nstack == 5)
                    {
                        int i;
                        for (i = 1; i < e->nstack; i++) e->stack[i - 1] = e->stack[i];
                        e->nstack--;
                    }
                }
                e->done = 1;
                e->nstack = 0;
            }
            else
            {
                /* operador no reconocido (aritmetica/logica para
                 * glyphs "programados", u otro escape 12-xx no
                 * soportado) -- limpiar la pila y seguir, tolerante,
                 * en vez de crashear o interpretar basura. */
                e->nstack = 0;
            }
        }
    }

    return !e->had_error;
}

int pdf_cff_glyph_outline(const pdf_cff_font *font, int gid,
                           pdf_cff_moveto_fn moveto, pdf_cff_lineto_fn lineto,
                           pdf_cff_curveto_fn curveto, void *user)
{
    cff_exec e;
    const unsigned char *cs_ptr;
    long cs_len;

    if (font == NULL || moveto == NULL || lineto == NULL || curveto == NULL)
        return PDF_ERR_BADARG;
    if (font->units_per_em <= 0.0) return PDF_ERR_BADARG;
    if (gid < 0 || gid >= font->num_glyphs) return PDF_ERR_NOTFOUND;
    if (!cff_get_charstring(font, gid, &cs_ptr, &cs_len)) return PDF_ERR_NOTFOUND;
    if (cs_len <= 0) return PDF_ERR_NOTFOUND; /* glyph vacio (espacio): nada que dibujar, no es error */

    memset(&e, 0, sizeof(e));
    e.font = font;
    e.moveto = moveto;
    e.lineto = lineto;
    e.curveto = curveto;
    e.user = user;
    e.inv_upm = 1.0 / font->units_per_em;

    if (!cff_run(&e, cs_ptr, cs_len))
        return PDF_ERR_BADARG;

    return PDF_OK;
}
