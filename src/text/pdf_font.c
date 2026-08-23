/* pdf_font.c
 *
 * Ver pdf_font.h.
 */

#include "pdf_font.h"
#include "pdf_error.h"
#include "pdf_filter.h"
#include "pdf_cff.h"
#include "pdf_afm.h"
#include <string.h>
#include <stdlib.h>

/* Adobe Glyph List reducida a lo que realmente hace falta aca (ver
 * pdf_cff.h): pdf_cff_glyph_name_for_code SOLO devuelve nombres
 * CUSTOM de la fuente (SID>=391) -- letras/digitos/puntuacion
 * estandar (que ya resuelven bien via WinAnsiEncoding directo) nunca
 * llegan aca, asi que no hace falta la AGL completa (~4000 nombres),
 * solo las ligaduras y simbolos comunes que las herramientas de
 * subconjunto de fuentes suelen definir como glyphs propios. */
static const struct { const char *name; unsigned short uni; } AGL_LITE[] = {
    { "ff",  0xFB00 }, { "fi",  0xFB01 }, { "fl",  0xFB02 },
    { "ffi", 0xFB03 }, { "ffl", 0xFB04 },
    { "lslash", 0x0142 }, { "Lslash", 0x0141 },
    { "dotlessi", 0x0131 },
    { "germandbls", 0x00DF },
    { "emdash", 0x2014 }, { "endash", 0x2013 },
    { "quoteleft", 0x2018 }, { "quoteright", 0x2019 },
    { "quotedblleft", 0x201C }, { "quotedblright", 0x201D },
    { "bullet", 0x2022 }, { "ellipsis", 0x2026 },
    { "trademark", 0x2122 }, { "copyright", 0x00A9 }, { "registered", 0x00AE },
    { "minus", 0x2212 }, { "multiply", 0x00D7 }, { "divide", 0x00F7 },
    { NULL, 0 }
};

static unsigned short agl_lite_lookup(const char *name)
{
    int i;
    for (i = 0; AGL_LITE[i].name != NULL; i++)
        if (strcmp(AGL_LITE[i].name, name) == 0) return AGL_LITE[i].uni;
    return 0;
}

/* BUG REAL ENCONTRADO (render de fuentes real, confirmado contra
 * Utilization_and_efficiency_of_ground_gra.pdf): el fallback "codigo
 * == unicode" (cuando no hay /ToUnicode ni nombre CFF resuelto) es
 * CORRECTO para ASCII (0-127) y para casi toda la mitad alta
 * (160-255, que WinAnsiEncoding hace coincidir con Latin-1
 * Supplement) -- pero NO para 128-159 (0x80-0x9F): WinAnsiEncoding
 * (=CP1252 de Windows) redefine ese rango con comillas curvas,
 * guiones em/en, puntos suspensivos, Euro, etc., que NO coinciden con
 * sus posiciones equivalentes en Unicode/Latin-1 (esas posiciones son
 * caracteres de control C1 invisibles). Ejemplo real: codigo 133
 * (0x85) es "…" (HORIZONTAL ELLIPSIS, U+2026) segun WinAnsiEncoding,
 * NO U+0085 (el control "NEL") -- pasar el codigo crudo hacia una
 * fuente sustituta buscaba un glyph en U+0085 (que no existe en
 * ninguna fuente real) y caia a la caja placeholder. 0 = sin
 * redefinicion en este rango (raro, casi todo 0x80-0x9F esta
 * asignado) -- el llamador sigue con "codigo tal cual" en ese caso. */
static const unsigned short WINANSI_80_9F[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178
};

static int name_contains_ci(const char *haystack, const char *needle)
{
    size_t hn = strlen(haystack);
    size_t nn = strlen(needle);
    size_t i;

    if (nn == 0 || nn > hn) return 0;

    for (i = 0; i + nn <= hn; i++)
    {
        size_t j;
        int match = 1;
        for (j = 0; j < nn; j++)
        {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* --- parser minimo de CMap /ToUnicode (ver DESIGN.md seccion 55) -------
 *
 * NO es un parser PostScript general -- el formato real de un CMap
 * /ToUnicode es un subconjunto muy chico y predecible de PostScript
 * (siempre generado por herramientas, nunca escrito a mano), asi que
 * alcanza con buscar los bloques 'beginbfchar'/'beginbfrange' y leer
 * los tokens <hex> que traen adentro -- no hace falta un interprete
 * PostScript completo. Tolerante: cualquier cosa que no entendamos
 * (bfrange con arreglo explicito de destinos, rangos con mas de un
 * caracter UTF-16 por codigo -- ligaduras, etc.) simplemente se
 * ignora o se aproxima con el primer code point, nunca se aborta el
 * parseo entero por un bloque raro. */

/* Busca la proxima ocurrencia de 'needle' en buf[pos..len), devuelve
 * el offset justo DESPUES de 'needle', o -1 si no aparece. */
static long cmap_find_after(const char *buf, long len, long pos, const char *needle)
{
    long nn = (long)strlen(needle);
    long i;
    for (i = pos; i + nn <= len; i++)
    {
        if (memcmp(buf + i, needle, (size_t)nn) == 0)
            return i + nn;
    }
    return -1;
}

/* Lee el proximo token <HEXHEX...> a partir de buf[pos), saltando
 * espacios. Devuelve el offset DESPUES de '>' via *out_end, y el valor
 * hex decodificado (hasta 4 bytes / 8 digitos, los primeros 2 bytes
 * como un solo code point UTF-16BE -- suficiente para BMP, que es
 * practicamente siempre lo que aparece en /ToUnicode de documentos
 * reales) via *out_val. 'out_nbytes' recibe cuantos bytes (mitad de
 * digitos hex) tenia el token, para distinguir codigos de 1 byte
 * (<01>) de codigos de 2 bytes (<0041>). Devuelve 0 si no encuentra
 * un token hex valido antes de otra cosa (fin de bloque, etc.). */
static int cmap_read_hex(const char *buf, long len, long pos,
                          long *out_end, unsigned int *out_val, int *out_nbytes)
{
    long i = pos;
    unsigned int val = 0;
    int ndigits = 0;

    while (i < len && (unsigned char)buf[i] <= ' ') i++; /* saltar ws */
    if (i >= len || buf[i] != '<')
        return 0;
    i++;

    while (i < len && buf[i] != '>')
    {
        int c = (unsigned char)buf[i];
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v >= 0 && ndigits < 8) { val = (val << 4) | (unsigned int)v; ndigits++; }
        i++;
    }
    if (i >= len) return 0; /* nunca cerro '>' -- CMap truncado/corrupto */
    i++; /* consumir '>' */

    *out_end    = i;
    *out_nbytes = (ndigits + 1) / 2;
    /* si el token tiene mas de 2 bytes (ligadura/caracter fuera del
     * BMP codificado como varios uint16), nos quedamos solo con el
     * PRIMER code point UTF-16 (los primeros 4 digitos hex) -- una
     * aproximacion razonable para texto latino tipico, que es el caso
     * abrumadoramente comun en los PDFs que este motor rendariza. */
    if (ndigits > 4)
        val = val >> ((ndigits - 4) * 4);
    *out_val = val;
    return 1;
}

/* Parsea 'data'/'len' (el contenido YA DECODIFICADO del stream
 * /ToUnicode) y llena font->to_unicode[]. Tolerante ante bloques que
 * no reconoce -- ver comentario arriba. */
/* Escribe el mapeo code->unicode en el destino correcto segun el tipo
 * de fuente (ver DESIGN.md seccion 62): 'to_unicode[256]' para
 * fuentes simples, 'cid_to_unicode[65536]' (si esta alocado) para
 * fuentes CID -- centraliza esta decision para no duplicarla en cada
 * lugar que parsea /ToUnicode (bfchar y bfrange). */
static void set_unicode(pdf_font *font, unsigned int code, unsigned int uni)
{
    if (font->is_cid)
    {
        if (font->cid_to_unicode != NULL && code < 65536)
            font->cid_to_unicode[code] = (unsigned short)uni;
    }
    else if (code < 256)
    {
        font->to_unicode[code] = (unsigned short)uni;
    }
}

static void parse_tounicode_cmap(const char *data, long len, pdf_font *font)
{
    long pos = 0;

    for (;;)
    {
        long bfchar_pos = cmap_find_after(data, len, pos, "beginbfchar");
        long bfrange_pos = cmap_find_after(data, len, pos, "beginbfrange");
        long block_start, block_end_search_from;
        int is_range;

        if (bfchar_pos < 0 && bfrange_pos < 0)
            break; /* no quedan mas bloques */

        if (bfchar_pos >= 0 && (bfrange_pos < 0 || bfchar_pos < bfrange_pos))
        { block_start = bfchar_pos; is_range = 0; }
        else
        { block_start = bfrange_pos; is_range = 1; }

        block_end_search_from = cmap_find_after(data, len, block_start,
                                                 is_range ? "endbfrange" : "endbfchar");
        if (block_end_search_from < 0)
            break; /* bloque sin cierre -- CMap truncado, no seguir */

        {
            long p = block_start;
            long block_end = block_end_search_from - (is_range ? (long)strlen("endbfrange") : (long)strlen("endbfchar"));

            while (p < block_end)
            {
                unsigned int code, uni;
                long after;
                int nbytes_code, nbytes_uni;

                if (!cmap_read_hex(data, block_end, p, &after, &code, &nbytes_code))
                    break; /* no hay mas tokens hex en este bloque */
                p = after;

                if (!is_range)
                {
                    /* bfchar: <code> <unicode> */
                    if (!cmap_read_hex(data, block_end, p, &after, &uni, &nbytes_uni))
                        break;
                    p = after;
                    set_unicode(font, code, uni);
                }
                else
                {
                    /* bfrange: <startcode> <endcode> <startunicode>
                     * (la forma con arreglo explicito "[<u1> <u2>...]"
                     * en vez de un solo <startunicode> no se soporta --
                     * se detecta y se salta el bloque para no leer
                     * basura, tolerante). */
                    unsigned int endcode;
                    long q = p;
                    while (q < block_end && (unsigned char)data[q] <= ' ') q++;
                    if (q < block_end && data[q] == '[')
                        break; /* forma con arreglo explicito: no soportada, saltar bloque */

                    if (!cmap_read_hex(data, block_end, p, &after, &endcode, &nbytes_uni))
                        break;
                    p = after;
                    if (!cmap_read_hex(data, block_end, p, &after, &uni, &nbytes_uni))
                        break;
                    p = after;

                    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 62):
                     * esta cota "endcode-code<256" tenia sentido SOLO
                     * para fuentes simples (codigos 0-255) -- para
                     * fuentes CID los rangos bfrange legitimos
                     * cubren MUCHOS mas de 256 codigos de uso comun
                     * (ej. todo el alfabeto latino en un subconjunto
                     * embebido puede venir como un unico rango de
                     * varios miles). Se usa el limite real del
                     * destino (65536 para CID, 256 para simple) en
                     * vez de un 256 fijo. */
                    {
                        unsigned int max_range = font->is_cid ? 65536u : 256u;
                        if (endcode >= code && endcode - code < max_range)
                        {
                            unsigned int c;
                            for (c = code; c <= endcode; c++)
                                set_unicode(font, c, uni + (c - code));
                        }
                    }
                }
            }
        }

        pos = block_end_search_from;
    }
}

/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 49): resuelve
 * 'obj' si es una referencia indirecta (comun para /Widths -- ver
 * comentario en pdf_font.h). Si no es referencia, devuelve 'obj' tal
 * cual (incluido NULL). Si 'st'/'xref' son NULL (llamador viejo/sin
 * contexto de resolucion), tambien devuelve 'obj' tal cual -- mismo
 * comportamiento (roto) que antes, mejor que crashear. */
static pdf_obj *resolve_ref(pdf_obj *obj, pdf_stream *st,
                             const pdf_xref_table *xref, pdf_arena *arena)
{
    if (obj == NULL || obj->type != PDF_REF)
        return obj;
    if (st == NULL || xref == NULL || arena == NULL)
        return obj;
    return pdf_parser_load_object(st, xref, obj->u.ref.num, arena);
}

/* Lee y parsea /ToUnicode (si esta presente) hacia font->to_unicode[].
 * Tolerante en cada paso: cualquier fallo (sin /ToUnicode, filtro no
 * soportado, CMap con sintaxis inesperada) deja to_unicode en todo
 * ceros (== "sin mapeo, usar el codigo tal cual"), nunca aborta la
 * carga del resto de la fuente. */
static void load_tounicode(pdf_obj *font_dict, pdf_font *font,
                            pdf_stream *st, const pdf_xref_table *xref, pdf_arena *arena)
{
    pdf_obj *tu = resolve_ref(pdf_dict_get(font_dict, "ToUnicode"), st, xref, arena);
    const char *filter_name;
    unsigned char *raw;
    long got;
    const char *cmap_data;
    long cmap_len;

    if (tu == NULL || tu->type != PDF_STREAM)
        return; /* sin /ToUnicode: nada que hacer, comportamiento normal */
    if (tu->u.stm.raw_length <= 0 || tu->u.stm.raw_length > 16L * 1024L * 1024L)
        return; /* CMap absurdamente grande (o vacio): mejor no tocarlo */

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)tu->u.stm.raw_length);
    if (raw == NULL) return;

    pdf_stream_seek(st, tu->u.stm.raw_offset);
    got = pdf_stream_read(st, raw, tu->u.stm.raw_length);
    if (xref != NULL && xref->crypt.active)
        got = pdf_crypt_decrypt(&xref->crypt, tu->u.stm.obj_num, tu->u.stm.obj_gen, raw, got);

    filter_name = pdf_dict_get_name(tu, "Filter");
    if (filter_name == NULL)
    {
        cmap_data = (const char *)raw;
        cmap_len  = got;
    }
    else if (strcmp(filter_name, "FlateDecode") == 0)
    {
        pdf_buf dec;
        if (pdf_filter_flate(arena, raw, got, 0, &dec) != PDF_OK)
            return;
        cmap_data = (const char *)dec.data;
        cmap_len  = dec.len;
    }
    else
    {
        return; /* ASCII85/otro filtro sobre /ToUnicode: raro, no soportado */
    }

    parse_tounicode_cmap(cmap_data, cmap_len, font);
}

/* Descomprime el stream de programa de fuente embebido 'ff' (llamado
 * tanto para /FontFile2 como para /CIDToGIDMap -- ambos son streams
 * binarios, opcionalmente FlateDecode) hacia 'arena'. Devuelve NULL si
 * 'ff' no es un stream, esta vacio/es enorme, o el filtro no es
 * soportado (algo distinto de FlateDecode/ninguno) -- tolerante, mismo
 * criterio que el resto del motor. '*out_len' recibe el tamanio
 * descomprimido si no es NULL. */
static const unsigned char *load_stream_bytes(pdf_obj *ff, pdf_stream *st,
                                               const pdf_xref_table *xref, pdf_arena *arena,
                                               long max_len, long *out_len)
{
    unsigned char *raw;
    long raw_len;
    pdf_obj *filter;

    if (ff == NULL || ff->type != PDF_STREAM) return NULL;
    if (ff->u.stm.raw_length <= 0 || ff->u.stm.raw_length > max_len) return NULL;

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)ff->u.stm.raw_length);
    if (raw == NULL) return NULL;
    pdf_stream_seek(st, ff->u.stm.raw_offset);
    raw_len = pdf_stream_read(st, raw, ff->u.stm.raw_length);
    if (xref != NULL && xref->crypt.active)
        raw_len = pdf_crypt_decrypt(&xref->crypt, ff->u.stm.obj_num, ff->u.stm.obj_gen, raw, raw_len);

    filter = pdf_dict_get(ff, "Filter");
    if (filter != NULL && filter->type == PDF_NAME && strcmp(filter->u.name, "FlateDecode") == 0)
    {
        pdf_buf dec;
        if (pdf_filter_flate(arena, raw, raw_len, 0, &dec) != PDF_OK) return NULL;
        if (out_len != NULL) *out_len = dec.len;
        return dec.data;
    }
    if (filter == NULL)
    {
        if (out_len != NULL) *out_len = raw_len;
        return raw;
    }
    return NULL; /* otro filtro: fuera de alcance de este atajo */
}

/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 66): cuando
 * /BaseFont es un nombre generico sin sentido (p.ej. "CIDFont+F1",
 * confirmado en un PDF real), la heuristica de 'is_serif' (seccion
 * 65) ayuda con el fallback promedio, pero sigue siendo una
 * aproximacion (Times New Roman "generico" en vez de la variante
 * EXACTA -- Bold, Italic, etc -- que el documento realmente usa,
 * causando un desajuste de anchos considerable igual). La fuente
 * TrueType embebida (/FontFile2) SIEMPRE trae su nombre real en la
 * tabla 'name' del formato sfnt -- METADATA estandar (un pequenio
 * directorio y algunos strings), no contornos de glyph, asi que leer
 * esto no viola el principio de diseño de este motor ("no se
 * parsean/dibujan contornos embebidos"). Si se puede extraer, da una
 * coincidencia EXACTA con una familia real de Windows en vez de una
 * aproximacion binaria serif/sans-serif.
 *
 * Formato sfnt (TrueType/OpenType), todos los campos big-endian:
 *   offset 0: version(4) numTables(2) searchRange(2) entrySelector(2) rangeShift(2)
 *   offset 12: 'numTables' entradas de 16 bytes: tag(4) checksum(4) offset(4) length(4)
 *   tabla 'name': format(2) count(2) stringOffset(2), luego 'count'
 *     registros de 12 bytes: platformID(2) encodingID(2) languageID(2)
 *     nameID(2) length(2) offset(2) -- offset relativo a
 *     tableStart+stringOffset.
 * Se busca nameID==4 (nombre completo, "Times New Roman Bold") con
 * preferencia platformID==3 (Windows, UTF-16BE) -- si no hay,
 * nameID==1 (familia) o platformID==1 (Mac, 1 byte/caracter) como
 * respaldo. Tolerante en cada paso: cualquier dato fuera de rango o
 * con forma inesperada hace que la funcion devuelva 0 sin tocar
 * 'out' -- el llamador sigue con el comportamiento de siempre
 * (nombre original + fallback is_serif de la seccion 65). */
static int ttf_name_from_table(const unsigned char *data, long len, char *out, int out_size,
                                int *out_is_bold, int *out_is_italic)
{
    long num_tables, i;
    long name_off = -1;

    *out_is_bold = -1; /* -1 = no se pudo determinar desde nameID=2, el llamador conserva lo que ya tenia */
    *out_is_italic = -1;

    if (len < 12) return 0;
    num_tables = ((long)data[4] << 8) | data[5];
    if (num_tables < 0 || num_tables > 64) return 0; /* sanidad: un sfnt real no tiene tantas tablas */

    for (i = 0; i < num_tables; i++)
    {
        long rec = 12 + i * 16;
        if (rec + 16 > len) return 0;
        if (data[rec] == 'n' && data[rec+1] == 'a' && data[rec+2] == 'm' && data[rec+3] == 'e')
        {
            name_off = ((long)data[rec+8] << 24) | ((long)data[rec+9] << 16) |
                       ((long)data[rec+10] << 8) | data[rec+11];
            break;
        }
    }
    if (name_off < 0 || name_off + 6 > len) return 0;

    {
        long count = ((long)data[name_off+2] << 8) | data[name_off+3];
        long str_base = name_off + (((long)data[name_off+4] << 8) | data[name_off+5]);
        long j;
        long best_off = -1, best_len = 0;
        int best_platform = -1, best_nameid = -1, best_lang_en = 0;
        long sub_off = -1, sub_len = 0;
        int sub_platform = -1, sub_lang_en = 0;

        if (count < 0 || count > 200) return 0;

        for (j = 0; j < count; j++)
        {
            long rec = name_off + 6 + j * 12;
            int platform, nameid, langid, lang_en;
            long slen, soff;
            if (rec + 12 > len) break;
            platform = ((int)data[rec] << 8) | data[rec+1];
            langid   = ((int)data[rec+4] << 8) | data[rec+5];
            nameid   = ((int)data[rec+6] << 8) | data[rec+7];
            slen = ((long)data[rec+8] << 8) | data[rec+9];
            soff = ((long)data[rec+10] << 8) | data[rec+11];
            if (nameid != 1 && nameid != 2 && nameid != 4) continue; /* 1=familia (preferido, SIN estilo), 2=subfamilia ("Bold"/"Italic"/"Regular", para el peso/estilo), 4=nombre completo (respaldo) */
            if (str_base + soff + slen > len || soff < 0 || slen < 0) continue;

            /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 66): la
             * tabla 'name' suele traer el MISMO nombre en varios
             * idiomas (confirmado en un PDF real: "Times New Roman
             * Bold" en ingles Y "Times New Roman Negreta" en
             * español, entre otros) -- sin priorizar el idioma, se
             * puede terminar tomando una variante NO INGLESA que
             * despues no coincide con la busqueda de "Bold"/"Italic"
             * (en ingles) que hace este mismo modulo mas abajo, ni
             * con los nombres de familia que Windows realmente usa
             * (que son en ingles incluso en instalaciones no
             * inglesas). Se prioriza explicitamente el registro en
             * ingles (languageID 0x0409, "English - United States",
             * con margen para aceptar CUALQUIER variante regional de
             * ingles -- el campo primary-language-id, bits bajos del
             * LANGID, identifica el idioma base "ingles" independiente
             * de la region especifica).
             *
             * Se usa nameID==1 (Font FAMILY -- "Times New Roman", SIN
             * el sufijo de estilo) como el NOMBRE a pasarle a
             * CreateFontA -- pasarle un family name que YA incluye
             * "Bold" en el texto, ADEMAS de pedir FW_BOLD por
             * separado, corre el riesgo de que Windows busque una
             * familia LITERALMENTE llamada "Times New Roman Bold"
             * (que no existe como familia separada) en vez de "Times
             * New Roman" + peso Bold -- reproduciria el mismo
             * problema que se esta arreglando. El peso/estilo real se
             * obtiene aparte, de nameID==2 (Subfamily -- justamente
             * "Bold"/"Italic"/"Regular"/"Bold Italic", el campo
             * disenado para esto). */
            lang_en = (platform == 3) ? ((langid & 0x3FF) == 0x09) : (platform == 1 && langid == 0);

            if (nameid == 2)
            {
                if (sub_off < 0 || (lang_en && !sub_lang_en) ||
                    (lang_en == sub_lang_en && platform == 3 && sub_platform != 3))
                {
                    sub_off = str_base + soff; sub_len = slen;
                    sub_platform = platform; sub_lang_en = lang_en;
                }
                continue;
            }

            if (best_off < 0 ||
                (lang_en && !best_lang_en) ||
                (lang_en == best_lang_en && nameid == 1 && best_nameid != 1) ||
                (lang_en == best_lang_en && nameid == best_nameid && platform == 3 && best_platform != 3))
            {
                best_off = str_base + soff;
                best_len = slen;
                best_platform = platform;
                best_nameid = nameid;
                best_lang_en = lang_en;
            }
        }
        if (best_off < 0) return 0;

        /* subfamilia (Bold/Italic/Regular): decodificar aparte hacia un
         * buffer chico y buscar las palabras clave -- mismo criterio
         * que el resto del motor (name_contains_ci). */
        if (sub_off >= 0)
        {
            char subbuf[32];
            int sn = 0;
            long k;
            if (sub_platform == 1)
            {
                for (k = 0; k < sub_len && sn < (int)sizeof(subbuf) - 1; k++)
                {
                    unsigned char c = data[sub_off + k];
                    if (c >= 32 && c < 127) subbuf[sn++] = (char)c;
                }
            }
            else
            {
                for (k = 0; k + 1 < sub_len && sn < (int)sizeof(subbuf) - 1; k += 2)
                {
                    unsigned char c = data[sub_off + k + 1];
                    if (data[sub_off + k] == 0 && c >= 32 && c < 127)
                        subbuf[sn++] = (char)c;
                }
            }
            subbuf[sn] = '\0';
            if (sn > 0)
            {
                *out_is_bold   = name_contains_ci(subbuf, "Bold");
                *out_is_italic = name_contains_ci(subbuf, "Italic") || name_contains_ci(subbuf, "Oblique");
            }
        }


        {
            int n = 0;
            long k;
            if (best_platform == 1) /* Mac: 1 byte por caracter */
            {
                for (k = 0; k < best_len && n < out_size - 1; k++)
                {
                    unsigned char c = data[best_off + k];
                    if (c >= 32 && c < 127) out[n++] = (char)c;
                }
            }
            else /* Windows/otros: UTF-16BE -- se toma el byte bajo (alcanza para nombres de fuente en ingles/basicos) */
            {
                for (k = 0; k + 1 < best_len && n < out_size - 1; k += 2)
                {
                    unsigned char c = data[best_off + k + 1];
                    if (data[best_off + k] == 0 && c >= 32 && c < 127)
                        out[n++] = (char)c;
                }
            }
            out[n] = '\0';
            return n > 0;
        }
    }
}

int pdf_font_load(pdf_obj *font_dict, pdf_font *font,
                   pdf_stream *st, const pdf_xref_table *xref, pdf_arena *arena)
{
    pdf_obj *base, *widths, *missing;
    pdf_obj *fdesc_for_cff;
    int i;

    if (font_dict == NULL || font == NULL)
        return PDF_ERR_BADARG;

    fdesc_for_cff = NULL;

    memset(font, 0, sizeof(*font));
    for (i = 0; i < 256; i++)
        font->widths[i] = -1.0;

    base = pdf_dict_get(font_dict, "BaseFont");
    if (base != NULL && base->type == PDF_NAME)
    {
        strncpy(font->base_font, base->u.name, sizeof(font->base_font) - 1);
        font->base_font[sizeof(font->base_font) - 1] = 0;
        font->is_bold   = name_contains_ci(font->base_font, "Bold");
        font->is_italic = name_contains_ci(font->base_font, "Italic")
                        || name_contains_ci(font->base_font, "Oblique");
    }

    font->first_char = (int)pdf_dict_get_int(font_dict, "FirstChar", 0);
    font->last_char  = (int)pdf_dict_get_int(font_dict, "LastChar", -1);

    /* /Encoding /Differences: marca 'has_custom_diff[code]' para cada
     * codigo que el PDF remapeo a un glyph propio del font original --
     * ver comentario largo junto a 'has_custom_diff' en pdf_font.h.
     * Formato T.32000-1 9.6.6.1: array alternando NUMEROS (fijan el
     * "codigo actual") y NOMBRES (asignan ese nombre al codigo actual,
     * despues incrementarlo) -- no nos interesa el nombre en si (no se
     * mantiene una Adobe Glyph List completa), solo que ESTE codigo
     * fue remapeado, para no confiar en "codigo == unicode" mas
     * adelante si no hay /ToUnicode que lo desmienta. */
    {
        pdf_obj *enc = resolve_ref(pdf_dict_get(font_dict, "Encoding"), st, xref, arena);
        if (enc != NULL && enc->type == PDF_DICT)
        {
            pdf_obj *diffs = resolve_ref(pdf_dict_get(enc, "Differences"), st, xref, arena);
            if (diffs != NULL && diffs->type == PDF_ARRAY)
            {
                int cur = 0, di;
                for (di = 0; di < diffs->u.arr.count; di++)
                {
                    pdf_obj *item = resolve_ref(diffs->u.arr.items[di], st, xref, arena);
                    if (item == NULL) continue;
                    if (item->type == PDF_INT || item->type == PDF_REAL)
                    {
                        cur = (int)pdf_obj_num(item, 0.0);
                    }
                    else if (item->type == PDF_NAME)
                    {
                        if (cur >= 0 && cur < 256)
                            font->has_custom_diff[cur] = 1;
                        cur++;
                    }
                }
            }
        }
    }

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 65): /Flags del
     * FontDescriptor (bit 2 = Serif, valor 2) -- unica pista
     * disponible cuando /BaseFont es un nombre generico sin sentido
     * (p.ej. "CIDFont+F1", confirmado en un PDF real) que no permite
     * elegir una fuente sustituta por nombre. El FontDescriptor vive
     * directo en el dict de fuente para fuentes SIMPLES, pero para
     * Type0 (CID) vive dentro de /DescendantFonts[0] -- se intentan
     * ambos lugares, el que exista. */
    {
        pdf_obj *fdesc = resolve_ref(pdf_dict_get(font_dict, "FontDescriptor"), st, xref, arena);
        if (fdesc == NULL)
        {
            pdf_obj *darr = resolve_ref(pdf_dict_get(font_dict, "DescendantFonts"), st, xref, arena);
            if (darr != NULL && darr->type == PDF_ARRAY && darr->u.arr.count > 0)
            {
                pdf_obj *dfont = resolve_ref(darr->u.arr.items[0], st, xref, arena);
                if (dfont != NULL)
                    fdesc = resolve_ref(pdf_dict_get(dfont, "FontDescriptor"), st, xref, arena);
            }
        }
        if (fdesc != NULL)
        {
            pdf_obj *flags_obj = pdf_dict_get(fdesc, "Flags");

            fdesc_for_cff = fdesc;

            if (flags_obj != NULL)
            {
                long flags = (long)pdf_obj_num(flags_obj, 0.0);
                font->is_serif = (flags & 2L) != 0;
            }

            /* /FontFile2 (TrueType embebido): se descomprime UNA sola
             * vez y se reusa para dos cosas --
             * 1) BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md
             *    seccion 66): si /BaseFont es generico ("CIDFont+F1"),
             *    extraer el nombre REAL desde la tabla 'name' -- una
             *    coincidencia EXACTA de familia (y hasta variante
             *    Bold/Italic especifica) es mucho mejor que el
             *    fallback binario serif/sans-serif de arriba. Solo se
             *    reemplaza 'base_font' si la extraccion tuvo exito --
             *    si falla se sigue con el 'base_font' original (el
             *    nombre del PDF, sea util o no) sin ningun cambio.
             * 2) Parsear con pdf_ttf_load (ver pdf_ttf.h) hacia
             *    'font->embedded_ttf' -- si tiene exito, pdf_render.c
             *    dibuja el contorno REAL del glyph en vez de
             *    sustituir con una fuente de sistema. */
            {
                long ttf_len = 0;
                const unsigned char *ttf_bytes =
                    load_stream_bytes(resolve_ref(pdf_dict_get(fdesc, "FontFile2"), st, xref, arena),
                                       st, xref, arena, 32L * 1024L * 1024L, &ttf_len);

                if (ttf_bytes != NULL)
                {
                    char embedded_name[64];
                    int emb_bold = -1, emb_italic = -1;

                    if (ttf_name_from_table(ttf_bytes, ttf_len, embedded_name, sizeof(embedded_name),
                                             &emb_bold, &emb_italic))
                    {
                        strncpy(font->base_font, embedded_name, sizeof(font->base_font) - 1);
                        font->base_font[sizeof(font->base_font) - 1] = 0;
                        /* is_bold/is_italic: preferir lo extraido del
                         * nameID=2 (Subfamily) de la fuente embebida
                         * cuando esta disponible (-1 = no se pudo
                         * determinar desde ahi) -- es mas confiable
                         * que buscar "Bold" en el nombre generico
                         * original del PDF ("CIDFont+F1" no dice
                         * nada). Si no se pudo determinar, se conserva
                         * lo que ya se habia calculado antes con el
                         * nombre original. */
                        if (emb_bold >= 0) font->is_bold = emb_bold;
                        if (emb_italic >= 0) font->is_italic = emb_italic;
                    }

                    font->has_embedded_ttf =
                        (pdf_ttf_load(ttf_bytes, ttf_len, &font->embedded_ttf) == PDF_OK);
                }
            }
        }
    }

    /* /MissingWidth en rigor vive dentro de /FontDescriptor, no del
     * dict de fuente directamente -- este parser simplificado no sigue
     * esa referencia (el FontDescriptor rara vez importa si no vamos a
     * parsear /FontFile de todos modos). Default razonable si falta. */
    missing = resolve_ref(pdf_dict_get(font_dict, "MissingWidth"), st, xref, arena);
    font->missing_width = (missing != NULL) ? pdf_obj_num(missing, 0.0) : 0.0;

    /* /Widths puede venir como referencia indirecta ("10 0 R") en vez
     * de array inline -- muy comun en PDFs reales (confirmado en los 4
     * fonts de Flujograma_Propuesto.pdf). Antes de este fix no se
     * resolvia, y el chequeo de tipo fallaba en silencio dejando TODOS
     * los anchos en -1 (ver pdf_font_get_width: cae al generico 500
     * parejo para cada caracter, sin importar la letra). */
    widths = resolve_ref(pdf_dict_get(font_dict, "Widths"), st, xref, arena);
    if (widths != NULL && widths->type == PDF_ARRAY)
    {
        int n = widths->u.arr.count;
        for (i = 0; i < n; i++)
        {
            int code = font->first_char + i;
            /* items individuales tambien podrian ser referencia
             * indirecta (raro, pero valido segun el estandar) -- se
             * resuelven igual por las dudas, es barato. */
            pdf_obj *item = resolve_ref(widths->u.arr.items[i], st, xref, arena);
            if (code >= 0 && code < 256)
                font->widths[code] = pdf_obj_num(item, -1.0);
        }
    }
    else
    {
        /* BUG REAL ENCONTRADO (confirmado contra
         * 2006-ThermodynamicModellingofKIVCETLeadSmeltingProcess-
         * TMS-2.pdf): un font dict de una de las 14 fuentes
         * "estandar" (Times/Helvetica/Courier + variantes) puede NO
         * traer /Widths en absoluto (ni /FontDescriptor) -- 100%
         * valido segun el estandar, el lector debe conocer esas
         * metricas de memoria. Sin esto, TODO caracter caia al
         * generico 500/1000 em (ver pdf_font_get_width), y el
         * desvio acumulado de posicion (Times real tiene anchos
         * MUY dispares, de 'i'=278 a 'm'=778+) hacia que las lineas
         * de texto justificado -- que el generador PDF corta en dos
         * TJ con un Td intermedio calculado con las metricas REALES
         * -- quedaran con el segundo tramo superpuesto sobre el
         * final del primero en vez de continuarlo (ver pdf_afm.h). */
        if (pdf_afm_is_courier(font->base_font))
        {
            int c;
            for (c = 32; c <= 126; c++) font->widths[c] = 600.0;
        }
        else
        {
            int c;
            for (c = 32; c <= 126; c++)
            {
                int w = pdf_afm_width(font->base_font, c);
                if (w >= 0) font->widths[c] = (double)w;
            }
        }
    }

    /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 62):
     * fuentes compuestas (/Subtype /Type0) -- MUY distintas de las
     * fuentes simples de arriba. El string de Tj/TJ trae 2 BYTES por
     * caracter (un CID de 16 bits), no 1 -- sin esto, el motor leia
     * "medio caracter" a la vez, produciendo texto con letras
     * mezcladas/duplicadas sin relacion aparente con el original
     * (confirmado con un PDF real). Solo se soporta /Encoding
     * /Identity-H (el CID coincide directo con los 2 bytes del
     * string, sin tabla de mapeo adicional) -- el caso casi
     * universal para fuentes CID embebidas generadas por
     * herramientas modernas (Chrome/wkhtmltopdf/librerias de
     * reportes, etc.); otros CMaps con nombre (p.ej. UniGB-UCS2-H)
     * o CMaps embebidos como stream quedan sin soportar (se
     * detectan y la fuente sigue tratandose como Type0/2-bytes de
     * todos modos -- mejor esfuerzo, mismo criterio tolerante que
     * el resto del motor, en vez de fallar toda la carga). */
    {
        pdf_obj *subtype = pdf_dict_get(font_dict, "Subtype");
        if (subtype != NULL && subtype->type == PDF_NAME &&
            strcmp(subtype->u.name, "Type0") == 0)
        {
            pdf_obj *desc_arr = resolve_ref(pdf_dict_get(font_dict, "DescendantFonts"), st, xref, arena);
            pdf_obj *desc = NULL;

            font->is_cid = 1;
            font->cid_default_width = 1000.0; /* default del estandar (9.7.4.3) si no hay /DW */

            /* ver comentario grande junto a 'is_cid_one_byte' en
             * pdf_font.h -- variante real de 1 byte por caracter,
             * detectada por el sufijo que la propia herramienta
             * generadora deja en el nombre. */
            if (strstr(font->base_font, "OneByteIdentityH") != NULL)
                font->is_cid_one_byte = 1;

            if (desc_arr != NULL && desc_arr->type == PDF_ARRAY && desc_arr->u.arr.count > 0)
                desc = resolve_ref(desc_arr->u.arr.items[0], st, xref, arena);

            if (desc != NULL)
            {
                pdf_obj *dw = resolve_ref(pdf_dict_get(desc, "DW"), st, xref, arena);
                pdf_obj *w_arr = resolve_ref(pdf_dict_get(desc, "W"), st, xref, arena);

                if (dw != NULL)
                    font->cid_default_width = pdf_obj_num(dw, 1000.0);

                if (w_arr != NULL && w_arr->type == PDF_ARRAY)
                {
                    long n = w_arr->u.arr.count;
                    long k = 0;

                    font->cid_widths = (float *)pdf_arena_alloc(arena, sizeof(float) * 65536);
                    if (font->cid_widths != NULL)
                    {
                        long ci;
                        for (ci = 0; ci < 65536; ci++) font->cid_widths[ci] = -1.0f;

                        /* formato T.32000-1 9.7.4.3: "[ c [w1 w2 ...] c1 c2 w ... ]"
                         * -- c seguido de un ARRAY = anchos individuales
                         * consecutivos empezando en c; c1 c2 w (tres
                         * numeros) = rango c1..c2 con ancho uniforme w. */
                        while (k < n)
                        {
                            pdf_obj *c_obj = resolve_ref(w_arr->u.arr.items[k], st, xref, arena);
                            long c0 = (long)pdf_obj_num(c_obj, -1.0);
                            if (c0 < 0 || k + 1 >= n) break;

                            {
                                pdf_obj *next = resolve_ref(w_arr->u.arr.items[k+1], st, xref, arena);
                                if (next != NULL && next->type == PDF_ARRAY)
                                {
                                    long m;
                                    for (m = 0; m < next->u.arr.count; m++)
                                    {
                                        long cid = c0 + m;
                                        if (cid >= 0 && cid < 65536)
                                        {
                                            pdf_obj *wi = resolve_ref(next->u.arr.items[m], st, xref, arena);
                                            font->cid_widths[cid] = (float)pdf_obj_num(wi, -1.0);
                                        }
                                    }
                                    k += 2;
                                }
                                else if (k + 2 < n)
                                {
                                    long c1 = (long)pdf_obj_num(next, -1.0);
                                    pdf_obj *w_obj = resolve_ref(w_arr->u.arr.items[k+2], st, xref, arena);
                                    double wv = pdf_obj_num(w_obj, -1.0);
                                    long cid;
                                    if (c1 >= c0 && c1 - c0 < 65536)
                                        for (cid = c0; cid <= c1; cid++)
                                            if (cid >= 0 && cid < 65536)
                                                font->cid_widths[cid] = (float)wv;
                                    k += 3;
                                }
                                else break; /* datos incompletos: no seguir */
                            }
                        }
                    }
                }
            }

            font->cid_to_unicode = (unsigned short *)pdf_arena_alloc(arena, sizeof(unsigned short) * 65536);
            if (font->cid_to_unicode != NULL)
                memset(font->cid_to_unicode, 0, sizeof(unsigned short) * 65536);

            /* /CIDToGIDMap: cuando ES un stream (no el default
             * /Identity ni ausente), mapea cada CID a su glyph index
             * real -- T.32000-1 9.7.4.4, 2 bytes big-endian por
             * entrada, indexado directo por CID. 'font->cid_to_gid'
             * queda NULL (identidad, GID=CID) en el caso comun
             * (/Identity o ausente) -- ver comentario en pdf_font.h.
             * Ningun PDF de prueba analizado hasta ahora trajo este
             * caso, pero es parte del estandar y barato de cubrir ya
             * que 'desc' y el resto de la infraestructura CID ya
             * estan resueltos aca mismo. */
            if (desc != NULL)
            {
                pdf_obj *c2g = resolve_ref(pdf_dict_get(desc, "CIDToGIDMap"), st, xref, arena);
                long map_len = 0;
                const unsigned char *map_bytes =
                    load_stream_bytes(c2g, st, xref, arena, 4L * 1024L * 1024L, &map_len);

                if (map_bytes != NULL && map_len > 0)
                {
                    font->cid_to_gid = (unsigned short *)pdf_arena_alloc(arena, sizeof(unsigned short) * 65536);
                    if (font->cid_to_gid != NULL)
                    {
                        long cid, n_entries = map_len / 2;
                        memset(font->cid_to_gid, 0, sizeof(unsigned short) * 65536);
                        if (n_entries > 65536) n_entries = 65536;
                        for (cid = 0; cid < n_entries; cid++)
                            font->cid_to_gid[cid] = (unsigned short)
                                (((unsigned int)map_bytes[cid * 2] << 8) | map_bytes[cid * 2 + 1]);
                    }
                }
            }
        }
    }

    /* /ToUnicode: ver comentario junto a 'to_unicode' en pdf_font.h y
     * DESIGN.md seccion 55 -- fuentes con subconjunto embebido y sin
     * /Encoding estandar necesitan esto para que el texto sea
     * legible en vez de "tofu" (cuadraditos de glyph faltante). Para
     * fuentes CID (is_cid=1, ya seteado arriba) esta MISMA llamada
     * llena 'cid_to_unicode' en vez de 'to_unicode' -- ver
     * set_unicode() en parse_tounicode_cmap. */
    load_tounicode(font_dict, font, st, xref, arena);

    /* BUG REAL ENCONTRADO (render de fuentes real, confirmado contra
     * Utilization_and_efficiency_of_ground_gra.pdf -- PDF real de
     * Elsevier/Arbortext): sin esto, un codigo remapeado por la fuente
     * embebida /FontFile3 (Type1/CFF) a una ligadura como "ffi" (comun:
     * las herramientas de maquetacion editorial reusan una posicion NO
     * USADA de WinAnsiEncoding, p.ej. codigo 222 "Thorn", para esto,
     * SIN avisar por /Differences ni /ToUnicode) se renderizaba con la
     * letra WinAnsi literal ("Þ") en vez de la ligadura real -- Acrobat
     * lo resuelve leyendo el Encoding INTERNO de la fuente embebida;
     * esto hace lo mismo para el subconjunto de nombres (ligaduras y
     * simbolos comunes, ver AGL_LITE arriba) que realmente aparecen en
     * la practica. Se ejecuta DESPUES de load_tounicode a proposito:
     * un /ToUnicode real (si existe) SIEMPRE gana -- esto solo completa
     * huecos (to_unicode[c]==0), nunca pisa un mapeo ya resuelto. Solo
     * aplica a fuentes simples (is_cid=0): las fuentes CID ya resuelven
     * GID directo via Identity-H, sin pasar por este problema. */
    if (!font->is_cid && fdesc_for_cff != NULL)
    {
        long cff_len = 0;
        const unsigned char *cff_bytes =
            load_stream_bytes(resolve_ref(pdf_dict_get(fdesc_for_cff, "FontFile3"), st, xref, arena),
                               st, xref, arena, 8L * 1024L * 1024L, &cff_len);

        if (cff_bytes != NULL)
        {
            int c;
            for (c = 0; c < 256; c++)
            {
                char name[64];
                unsigned short uni;

                if (font->to_unicode[c] != 0) continue;
                if (!pdf_cff_glyph_name_for_code(cff_bytes, cff_len, c, name, sizeof(name))) continue;

                uni = agl_lite_lookup(name);
                if (uni != 0) font->to_unicode[c] = uni;
            }

            /* Contornos reales (ver DESIGN.md, ronda "interprete CFF
             * completo"): mismos bytes ya descomprimidos arriba,
             * reusados para parsear CharStrings/Subrs -- si el CFF es
             * CID-keyed o le falta CharStrings, pdf_cff_load falla a
             * proposito (fuera de alcance) y has_embedded_cff queda 0,
             * el llamador (pdf_render.c) cae a fuente de sistema. */
            font->has_embedded_cff = (pdf_cff_load(cff_bytes, cff_len, &font->embedded_cff) == PDF_OK);
        }
    }

    return PDF_OK;
}

double pdf_font_get_width(const pdf_font *font, int code)
{
    if (font == NULL)
        return 500.0;
    if (font->is_cid)
    {
        if (code >= 0 && code < 65536 && font->cid_widths != NULL && font->cid_widths[code] >= 0.0f)
            return (double)font->cid_widths[code];
        return font->cid_default_width;
    }
    if (code < 0 || code > 255)
        return 500.0;
    if (font->widths[code] >= 0.0)
        return font->widths[code];
    return (font->missing_width > 0.0) ? font->missing_width : 500.0;
}

int pdf_font_get_unicode(const pdf_font *font, int code)
{
    if (font == NULL)
        return code;
    if (font->is_cid)
    {
        if (code >= 0 && code < 65536 && font->cid_to_unicode != NULL && font->cid_to_unicode[code] != 0)
            return (int)font->cid_to_unicode[code];
        return code; /* sin mapeo: CID tal cual (mismo criterio que el caso simple) */
    }
    if (code < 0 || code > 255)
        return code;
    if (font->to_unicode[code] != 0)
        return (int)font->to_unicode[code];
    if (font->has_custom_diff[code])
        return 0xFFFD; /* /Differences remapeo este codigo y no hay /ToUnicode que diga a que --
                         * ver comentario junto a 'has_custom_diff' en pdf_font.h: no confiar en
                         * "codigo == unicode" aca, podria coincidir con una letra real por
                         * casualidad (bug real encontrado con render de fuentes real). */
    if (code >= 128 && code <= 159 && WINANSI_80_9F[code - 128] != 0)
        return (int)WINANSI_80_9F[code - 128]; /* ver comentario junto a WINANSI_80_9F arriba */
    return code; /* sin mapeo: comportamiento de siempre, codigo tal cual */
}
