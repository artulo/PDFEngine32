/* pdf_image.c
 *
 * Ver pdf_image.h.
 */

#include "pdf_image.h"
#include "pdf_parser.h"
#include "pdf_filter.h"
#include "pdf_jpx.h"
#include "pdf_jbig2.h"
#include <string.h>
#include <math.h>

/* --- inversion de matriz afin 2x3 --------------------------------------- */

static int mat_invert(pdf_matrix m, pdf_matrix *out)
{
    double det = m.a * m.d - m.b * m.c;
    if (det > -1e-12 && det < 1e-12)
        return 0;
    out->a =  m.d / det;
    out->b = -m.b / det;
    out->c = -m.c / det;
    out->d =  m.a / det;
    out->e = -(m.e * out->a + m.f * out->c);
    out->f = -(m.e * out->b + m.f * out->d);
    return 1;
}

static pdf_point mat_transform(pdf_matrix m, double x, double y)
{
    pdf_point p;
    p.x = m.a * x + m.c * y + m.e;
    p.y = m.b * x + m.d * y + m.f;
    return p;
}

/* --- paleta para Indexed ------------------------------------------------- */

static int cmyk_to_rgb(double c, double m, double y, double k,
                        unsigned char *r, unsigned char *g, unsigned char *b)
{
    double rr = 1.0 - ((c + k > 1.0) ? 1.0 : c + k);
    double gg = 1.0 - ((m + k > 1.0) ? 1.0 : m + k);
    double bb = 1.0 - ((y + k > 1.0) ? 1.0 : y + k);
    *r = (unsigned char)(rr * 255.0 + 0.5);
    *g = (unsigned char)(gg * 255.0 + 0.5);
    *b = (unsigned char)(bb * 255.0 + 0.5);
    return 1;
}

/* --- decodificacion principal --------------------------------------------- */

/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 59): /SMask
 * nunca se leia -- una imagen con mascara de transparencia por-pixel
 * (comun: sombras suaves, logos con bordes antialiasados, fotos
 * recortadas) se dibujaba 100% opaca. /SMask es en si misma una
 * imagen (tipicamente DeviceGray de 8bpc, puede tener su propio
 * /Filter -- incluso DCTDecode) cuyo VALOR DE GRIS es la opacidad de
 * cada pixel de la imagen base (0=transparente, 255=opaco). Se
 * decodifica RECURSIVAMENTE con pdf_image_decode (nos da su .rgb
 * gris-replicado-a-RGB, del cual solo el canal R hace falta) y, si
 * sus dimensiones no coinciden con las de la imagen base (el
 * estandar lo permite explicitamente), se remuestrea por
 * vecino-mas-cercano al tamanio de la imagen base -- asi
 * pdf_image_draw puede indexar el resultado con las mismas
 * coordenadas que 'rgb' sin preocuparse por la diferencia. Devuelve
 * NULL si no hay /SMask o si algo en el camino falla (SMask con un
 * feature no soportado, etc.) -- el llamador sigue de largo SIN
 * mascara (opaca, el comportamiento de siempre) en vez de fallar
 * toda la imagen por la mascara.
 *
 * Se llama desde DOS lugares en pdf_image_decode: el camino generico
 * (cerca de su 'return PDF_OK' final) Y el camino DCTDecode (que
 * tiene su PROPIO 'return PDF_OK' anticipado, saltandose el generico
 * por completo) -- confirmado con un caso real
 * (boiler_light_up_procedure.pdf) que fallaba especificamente porque
 * la imagen con /SMask era un JPEG. */
/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 64): camino
 * rapido para el caso COMUN de /SMask (DeviceGray de 8bpc, con
 * FlateDecode o sin filtro, sin indexado -- la enorme mayoria de los
 * SMask reales) que evita por completo pasar por 'pdf_image_decode'
 * generico (que da RGB de 3 canales, DESPERDICIANDO 2/3 de la
 * memoria ya que un SMask solo necesita 1 canal). Devuelve 1 si pudo
 * decodificar por este camino (llenando 'out_gray', 1 byte/pixel,
 * SIN aplicar /Decode todavia -- eso lo hace el llamador), o 0 si el
 * formato es demasiado complejo para este atajo (DCT, CCITT,
 * indexado, bpc!=8, cadena de filtros con mas de uno) -- en ese caso
 * el llamador cae al camino generico de siempre. */
static int pdf_image_smask_fast_gray8(pdf_stream *st, const pdf_xref_table *xref,
                                       pdf_obj *smask_dict, pdf_arena *arena,
                                       int *out_w, int *out_h, unsigned char **out_gray)
{
    pdf_obj *cs, *bpc_obj, *filter_obj;
    int width, height, bpc;
    unsigned char *raw;
    long raw_len;

    if (smask_dict->type != PDF_STREAM) return 0;

    cs = pdf_dict_get(smask_dict, "ColorSpace");
    if (cs == NULL || cs->type != PDF_NAME || strcmp(cs->u.name, "DeviceGray") != 0)
        return 0;

    bpc_obj = pdf_dict_get(smask_dict, "BitsPerComponent");
    bpc = (bpc_obj != NULL) ? (int)pdf_obj_num(bpc_obj, 8.0) : 8;
    if (bpc != 8) return 0;

    filter_obj = pdf_dict_get(smask_dict, "Filter");
    if (filter_obj != NULL && filter_obj->type != PDF_NAME) return 0; /* cadena de filtros: fuera de alcance de este atajo */

    width  = (int)pdf_dict_get_int(smask_dict, "Width", 0);
    height = (int)pdf_dict_get_int(smask_dict, "Height", 0);
    if (width <= 0 || height <= 0) return 0;

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)smask_dict->u.stm.raw_length);
    if (raw == NULL) return 0;
    pdf_stream_seek(st, smask_dict->u.stm.raw_offset);
    raw_len = pdf_stream_read(st, raw, smask_dict->u.stm.raw_length);
    if (xref != NULL && xref->crypt.active)
        raw_len = pdf_crypt_decrypt(&xref->crypt, smask_dict->u.stm.obj_num, smask_dict->u.stm.obj_gen, raw, raw_len);

    if (filter_obj == NULL)
    {
        if (raw_len < (long)width * height) return 0; /* datos insuficientes: dejar que el camino generico lo maneje/falle igual */
        *out_gray = raw;
    }
    else if (strcmp(filter_obj->u.name, "FlateDecode") == 0)
    {
        pdf_buf dec;
        if (pdf_filter_flate(arena, raw, raw_len, 0, &dec) != PDF_OK) return 0;
        if (dec.len < (long)width * height) return 0;
        *out_gray = dec.data;
    }
    else
    {
        return 0; /* DCT/CCITT/etc en el SMask: fuera de alcance de este atajo, camino generico */
    }

    *out_w = width; *out_h = height;
    return 1;
}

static unsigned char *pdf_image_load_smask(pdf_stream *st, const pdf_xref_table *xref,
                                            pdf_obj *img_dict, pdf_arena *arena,
                                            int width, int height)
{
    pdf_obj *smask_ref = pdf_dict_get(img_dict, "SMask");
    pdf_obj *smask_dict = (smask_ref != NULL && smask_ref->type == PDF_REF)
        ? pdf_parser_load_object(st, xref, smask_ref->u.ref.num, arena)
        : smask_ref;
    pdf_image smask_img;
    unsigned char *a;
    int sx, sy;
    int invert = 0;
    int fast_w = 0, fast_h = 0;
    unsigned char *fast_gray = NULL;
    int used_fast;

    if (smask_dict == NULL || smask_dict->type != PDF_STREAM)
        return NULL;

    used_fast = pdf_image_smask_fast_gray8(st, xref, smask_dict, arena, &fast_w, &fast_h, &fast_gray);
    if (!used_fast)
    {
        if (pdf_image_decode(st, xref, smask_dict, arena, &smask_img, 0.0, 0.0) != PDF_OK)
            return NULL;
        if (smask_img.rgb == NULL)
            return NULL;
    }

    a = (unsigned char *)pdf_arena_alloc(arena, (size_t)width * height);
    if (a == NULL)
        return NULL;

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 59): confirmado con
     * un PDF real que trae '/Decode [1 0]' en el /SMask -- el default
     * para DeviceGray es [0 1] (muestra cruda 0->salida 0, 255->
     * salida 1), asi que [1 0] INVIERTE: muestra cruda 0 representa
     * opacidad 1 (opaco), 255 representa opacidad 0 (transparente).
     * Sin aplicar esto, el canal se lee al reves. No se intenta
     * soportar /Decode arbitrario (fuera de alcance, ver
     * pdf_image.h) -- solo se detecta este caso puntual pero real y
     * comun (mascara invertida). */
    {
        pdf_obj *dec = pdf_dict_get(smask_dict, "Decode");
        if (dec != NULL && dec->type == PDF_ARRAY && dec->u.arr.count == 2 &&
            pdf_obj_num(dec->u.arr.items[0], 0.0) > pdf_obj_num(dec->u.arr.items[1], 1.0))
            invert = 1;
    }

    if (used_fast)
    {
        if (fast_w == width && fast_h == height)
        {
            for (sy = 0; sy < height; sy++)
                for (sx = 0; sx < width; sx++)
                {
                    unsigned char g = fast_gray[sy * width + sx];
                    a[sy * width + sx] = invert ? (unsigned char)(255 - g) : g;
                }
        }
        else
        {
            for (sy = 0; sy < height; sy++)
            {
                int my = sy * fast_h / height;
                for (sx = 0; sx < width; sx++)
                {
                    int mx = sx * fast_w / width;
                    unsigned char g = fast_gray[my * fast_w + mx];
                    a[sy * width + sx] = invert ? (unsigned char)(255 - g) : g;
                }
            }
        }
        return a;
    }

    if (smask_img.width == width && smask_img.height == height)
    {
        for (sy = 0; sy < height; sy++)
            for (sx = 0; sx < width; sx++)
            {
                unsigned char g = smask_img.rgb[(sy * width + sx) * 3];
                a[sy * width + sx] = invert ? (unsigned char)(255 - g) : g;
            }
    }
    else
    {
        for (sy = 0; sy < height; sy++)
        {
            int my = sy * smask_img.height / height;
            for (sx = 0; sx < width; sx++)
            {
                int mx = sx * smask_img.width / width;
                unsigned char g = smask_img.rgb[(my * smask_img.width + mx) * 3];
                a[sy * width + sx] = invert ? (unsigned char)(255 - g) : g;
            }
        }
    }
    return a;
}

int pdf_image_decode(pdf_stream *st, const pdf_xref_table *xref,
                      pdf_obj *img_dict, pdf_arena *arena, pdf_image *out,
                      double dest_w_px, double dest_h_px)
{
    int width, height, bpc;
    int is_mask;
    pdf_obj *cs_obj;
    int ncomp;
    int is_indexed;
    unsigned char *palette;      /* base_ncomp bytes por entrada, hasta 256 entradas */
    int palette_base_ncomp;
    int palette_count;
    const char *filter_name;
    unsigned char *raw;
    long raw_len;
    unsigned char *sample_bytes; /* bytes ya decodificados (post-filtro), formato "crudo" width*height*ncomp*bpc/8-por-fila */
    long row_bytes;
    int x, y;
    int decode_inverted; /* /Decode [1 0] en vez del default [0 1] -- ver comentario mas abajo */
    int jpeg_reduction_hint; /* ver DESIGN.md seccion 87 -- solo se usa en el camino DCTDecode */

    if (out == NULL) return PDF_ERR_BADARG;
    memset(out, 0, sizeof(*out));

    if (st == NULL || img_dict == NULL || arena == NULL || img_dict->type != PDF_STREAM)
        return PDF_ERR_BADARG;

    /* Guarda critica: si /Length quedo sin resolver (referencia
     * indirecta que el xref no pudo encontrar -- ver pdf_parser.c) o es
     * un valor absurdo, raw_length puede ser -1 o negativo. Convertir
     * eso a size_t mas abajo (para pdf_arena_alloc) lo transforma en un
     * numero gigante (4GB en 32 bits) que puede desbordar el calculo
     * interno del allocator y terminar reservando un bloque CHICO por
     * accidente -- mientras el resto del codigo sigue creyendo que hay
     * espacio para 4GB, y termina escribiendo muy por fuera del buffer
     * real (corrupcion de heap silenciosa, exactamente el tipo de bug
     * que puede crashear en un punto totalmente distinto de donde esta
     * el error real). Cortar aca, limpio, en vez de arriesgarse. */
    if (img_dict->u.stm.raw_length <= 0 || img_dict->u.stm.raw_length > 500L * 1024L * 1024L)
        return PDF_ERR_BADARG;

    width  = (int)pdf_dict_get_int(img_dict, "Width", 0);
    height = (int)pdf_dict_get_int(img_dict, "Height", 0);
    bpc    = (int)pdf_dict_get_int(img_dict, "BitsPerComponent", 8);
    if (width <= 0 || height <= 0 || width > 20000 || height > 20000)
        return PDF_ERR_BADARG; /* limites defensivos contra streams corruptos */

    /* BUG REAL DE RENDIMIENTO (ver DESIGN.md seccion 87): decidir aca,
     * UNA vez por decode, si conviene pedirle a DCTDecode una
     * resolucion reducida -- solo tiene sentido cuando el llamador
     * conoce el tamanio de destino (dest_w_px/dest_h_px > 0, ver
     * pdf_image.h) Y la imagen se va a mostrar a la mitad o menos de
     * su tamanio nativo en AMBOS ejes (el eje que MENOS se reduce
     * manda -- si solo un eje se achica mucho pero el otro no, no es
     * seguro perder resolucion). Umbral 2.0 exacto: "mitad o menos". */
    {
        jpeg_reduction_hint = 1;
        if (dest_w_px > 0.0 && dest_h_px > 0.0)
        {
            double rx = (double)width  / dest_w_px;
            double ry = (double)height / dest_h_px;
            double rmin = (rx < ry) ? rx : ry;
            if (rmin >= 2.0)
                jpeg_reduction_hint = 2;
        }
    }

    {
        pdf_obj *im = pdf_dict_get(img_dict, "ImageMask");
        is_mask = (im != NULL && im->type == PDF_BOOL && im->u.boolean);
    }

    is_indexed = 0;
    palette = NULL;
    palette_base_ncomp = 3;
    palette_count = 0;
    ncomp = 1;

    if (!is_mask)
    {
        cs_obj = pdf_dict_get(img_dict, "ColorSpace");
        if (cs_obj != NULL && cs_obj->type == PDF_REF)
            cs_obj = pdf_parser_load_object(st, xref, cs_obj->u.ref.num, arena);

        if (cs_obj != NULL && cs_obj->type == PDF_NAME)
        {
            if (strcmp(cs_obj->u.name, "DeviceGray") == 0 || strcmp(cs_obj->u.name, "CalGray") == 0)
                ncomp = 1;
            else if (strcmp(cs_obj->u.name, "DeviceCMYK") == 0)
                ncomp = 4;
            else
                ncomp = 3; /* DeviceRGB, CalRGB, o desconocido: mejor apuesta */
        }
        else if (cs_obj != NULL && cs_obj->type == PDF_ARRAY && cs_obj->u.arr.count >= 4)
        {
            pdf_obj *family = cs_obj->u.arr.items[0];
            if (family != NULL && family->type == PDF_NAME && strcmp(family->u.name, "Indexed") == 0)
            {
                pdf_obj *base = cs_obj->u.arr.items[1];
                pdf_obj *hival = cs_obj->u.arr.items[2];
                pdf_obj *lookup = cs_obj->u.arr.items[3];

                is_indexed = 1;
                ncomp = 1;

                if (base != NULL && base->type == PDF_REF)
                    base = pdf_parser_load_object(st, xref, base->u.ref.num, arena);

                palette_base_ncomp = 3;
                if (base != NULL && base->type == PDF_NAME && strcmp(base->u.name, "DeviceGray") == 0)
                    palette_base_ncomp = 1;
                else if (base != NULL && base->type == PDF_NAME && strcmp(base->u.name, "DeviceCMYK") == 0)
                    palette_base_ncomp = 4;

                palette_count = (int)pdf_obj_num(hival, 255.0) + 1;
                if (palette_count > 256) palette_count = 256;

                if (lookup != NULL && lookup->type == PDF_REF)
                    lookup = pdf_parser_load_object(st, xref, lookup->u.ref.num, arena);

                if (lookup != NULL && lookup->type == PDF_STRING)
                {
                    palette = (unsigned char *)lookup->u.str.data;
                }
                else if (lookup != NULL && lookup->type == PDF_STREAM &&
                         lookup->u.stm.raw_length > 0 && lookup->u.stm.raw_length <= 500L * 1024L * 1024L)
                {
                    unsigned char *praw;
                    long pgot;
                    const char *pfilter;

                    praw = (unsigned char *)pdf_arena_alloc(arena, (size_t)lookup->u.stm.raw_length);
                    if (praw != NULL)
                    {
                        pdf_stream_seek(st, lookup->u.stm.raw_offset);
                        pgot = pdf_stream_read(st, praw, lookup->u.stm.raw_length);
                        if (xref != NULL && xref->crypt.active)
                            pgot = pdf_crypt_decrypt(&xref->crypt, lookup->u.stm.obj_num, lookup->u.stm.obj_gen, praw, pgot);
                        pfilter = pdf_dict_get_name(lookup, "Filter");
                        if (pfilter != NULL && strcmp(pfilter, "FlateDecode") == 0)
                        {
                            pdf_buf pdec;
                            if (pdf_filter_flate(arena, praw, pgot, 0, &pdec) == PDF_OK)
                                palette = pdec.data;
                        }
                        else
                        {
                            palette = praw;
                        }
                    }
                }
            }
            else
            {
                ncomp = 3; /* ICCBased/Separation/etc no resueltos: mejor apuesta RGB */
            }
        }
        else
        {
            ncomp = 3;
        }
    }

    /* BUG REAL ENCONTRADO (confirmado contra "enciclopedia de
     * soldadura.pdf" -- Im18, la portada escaneada de un libro): un
     * /Decode array explicito en el dict de la imagen invierte (o en
     * general remapea) el rango [0,1] default de cada componente,
     * INDEPENDIENTE de /BlackIs1 (que es un concepto propio de
     * CCITTFaxDecode, actua ANTES, sobre como los BITS del stream se
     * empaquetan en muestras -- /Decode actua DESPUES, sobre como esas
     * muestras ya desempaquetadas se interpretan como nivel de gris/
     * color). Esta imagen en particular declara /Decode [1 0] (el
     * inverso del default [0 1]) -- sin leerlo, el motor asumia
     * SIEMPRE el default, y una imagen DeviceGray de 1 bit con la
     * intencion real "0=blanco,1=negro" (via este Decode invertido)
     * se dibujaba con blanco y negro exactamente al reves: el fondo
     * de pagina (mayoria del area) salia NEGRO en vez de BLANCO.
     * Alcance de este fix: solo DeviceGray SIN paleta (Indexed usa
     * /Decode con una semantica distinta -- rango de INDICES de
     * paleta, no nivel de gris -- fuera de alcance, no es el caso que
     * motiva esto). */
    {
        decode_inverted = 0;
        if (!is_indexed && ncomp == 1)
        {
            pdf_obj *dec = pdf_dict_get(img_dict, "Decode");
            if (dec != NULL && dec->type == PDF_REF)
                dec = pdf_parser_load_object(st, xref, dec->u.ref.num, arena);
            if (dec != NULL && dec->type == PDF_ARRAY && dec->u.arr.count >= 2)
            {
                double d0 = pdf_obj_num(dec->u.arr.items[0], 0.0);
                double d1 = pdf_obj_num(dec->u.arr.items[1], 1.0);
                decode_inverted = (d0 > d1);
            }
        }
    }

    /* --- leer bytes crudos del stream --------------------------------- */

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)img_dict->u.stm.raw_length);
    if (raw == NULL) return PDF_ERR_NOMEM;
    pdf_stream_seek(st, img_dict->u.stm.raw_offset);
    raw_len = pdf_stream_read(st, raw, img_dict->u.stm.raw_length);
    if (xref != NULL && xref->crypt.active)
        raw_len = pdf_crypt_decrypt(&xref->crypt, img_dict->u.stm.obj_num, img_dict->u.stm.obj_gen, raw, raw_len);

    /* --- resolver /Filter, que puede ser un solo nombre O un array de
     * nombres encadenados (p.ej. "[/FlateDecode /DCTDecode]" -- muy
     * comun: el productor comprime el JPEG entero con Flate ademas de
     * la compresion JPEG propia). Se aplican todos los filtros MENOS EL
     * ULTIMO como preprocesamiento generico (solo Flate/ASCII85 tienen
     * sentido ahi); el ultimo filtro determina el camino final (igual
     * que antes: DCT/CCITT/Flate/sin filtro). ------------------------- */
    {
        pdf_obj *filter_obj = pdf_dict_get(img_dict, "Filter");
        const char *chain[4];
        int nfilters = 0;
        unsigned char *cur_data = raw;
        long cur_len = raw_len;
        int fi;

        if (filter_obj != NULL && filter_obj->type == PDF_NAME)
        {
            chain[nfilters++] = filter_obj->u.name;
        }
        else if (filter_obj != NULL && filter_obj->type == PDF_ARRAY)
        {
            for (fi = 0; fi < filter_obj->u.arr.count && nfilters < 4; fi++)
            {
                pdf_obj *item = filter_obj->u.arr.items[fi];
                if (item != NULL && item->type == PDF_NAME)
                    chain[nfilters++] = item->u.name;
            }
        }

        /* aplicar todos menos el ultimo como preprocesamiento generico */
        for (fi = 0; fi < nfilters - 1; fi++)
        {
            pdf_buf dec;
            if (strcmp(chain[fi], "FlateDecode") == 0)
            {
                if (pdf_filter_flate(arena, cur_data, cur_len, 0, &dec) != PDF_OK)
                    return PDF_ERR_UNSUPPORTED;
            }
            else if (strcmp(chain[fi], "ASCII85Decode") == 0)
            {
                if (pdf_filter_ascii85(arena, cur_data, cur_len, &dec) != PDF_OK)
                    return PDF_ERR_UNSUPPORTED;
            }
            else
            {
                return PDF_ERR_UNSUPPORTED; /* filtro intermedio no soportado */
            }
            cur_data = dec.data;
            cur_len  = dec.len;
        }

        filter_name = (nfilters > 0) ? chain[nfilters - 1] : NULL;
        raw     = cur_data; /* a partir de aca, 'raw'/'raw_len' son los bytes
                              * YA pasados por los filtros intermedios --
                              * el resto del codigo de abajo no cambia. */
        raw_len = cur_len;
    }

    /* --- JPXDecode (JPEG2000, ver DESIGN.md seccion 60): igual que
     * DCTDecode, entrega RGB24 completo sin pasar por el
     * desempaquetado generico de bits mas abajo. */
    if (filter_name != NULL && strcmp(filter_name, "JPXDecode") == 0)
    {
        pdf_jpx_image jpx;
        int jrc = pdf_filter_jpx(arena, raw, raw_len, &jpx);
        if (jrc != PDF_OK)
            return jrc;

        out->width   = jpx.width;
        out->height  = jpx.height;
        out->is_mask = 0;
        out->rgb     = jpx.rgb;
        out->alpha   = pdf_image_load_smask(st, xref, img_dict, arena, jpx.width, jpx.height);
        return PDF_OK;
    }

    /* --- DCTDecode: el filtro ya entrega RGB24 completo, sin pasar por
     * el desempaquetado generico de bits mas abajo. ---------------------- */
    if (filter_name != NULL && strcmp(filter_name, "DCTDecode") == 0)
    {
        pdf_jpeg_image jpg;
        int drc = pdf_filter_dct(arena, raw, raw_len, &jpg, jpeg_reduction_hint);
        if (drc != PDF_OK)
            return drc; /* propagar el motivo real (p.ej. PDF_ERR_NOMEM si
                         * la imagen no entra en el presupuesto del
                         * documento) en vez de reportar UNSUPPORTED */

        out->width   = jpg.width;
        out->height  = jpg.height;
        out->is_mask = 0;
        out->rgb     = jpg.rgb;
        /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 59): este 'return'
         * temprano especifico de DCTDecode se saltaba por completo la
         * deteccion de /SMask (que solo se chequeaba mas abajo, cerca
         * del 'return PDF_OK' generico) -- exactamente el caso real
         * que fallaba (el titulo de boiler_light_up_procedure.pdf es
         * un JPEG con /SMask). pdf_image_load_smask() ahora se llama
         * desde ACA tambien, no solo al final. */
        out->alpha = pdf_image_load_smask(st, xref, img_dict, arena, jpg.width, jpg.height);
        return PDF_OK;
    }

    /* --- CCITTFaxDecode: produce bits empaquetados 1bpp, que despues se
     * desempaquetan igual que una imagen cruda de 1 bpc. ------------------ */
    if (filter_name != NULL && strcmp(filter_name, "CCITTFaxDecode") == 0)
    {
        pdf_obj *parms = pdf_dict_get(img_dict, "DecodeParms");
        int columns = width, rows = height, k = 0, black_is_1 = 0;
        pdf_buf dec;

        if (parms == NULL) parms = pdf_dict_get(img_dict, "DP");
        if (parms != NULL && parms->type == PDF_REF)
            parms = pdf_parser_load_object(st, xref, parms->u.ref.num, arena);
        if (parms != NULL && parms->type == PDF_DICT)
        {
            columns = (int)pdf_dict_get_int(parms, "Columns", width);
            rows    = (int)pdf_dict_get_int(parms, "Rows", height);
            k       = (int)pdf_dict_get_int(parms, "K", 0);
            {
                pdf_obj *b1 = pdf_dict_get(parms, "BlackIs1");
                black_is_1 = (b1 != NULL && b1->type == PDF_BOOL && b1->u.boolean);
            }
        }
        if (rows <= 0) rows = height;

        if (k >= 0)
            return PDF_ERR_UNSUPPORTED; /* solo Group 4 (K<0) implementado */

        if (pdf_filter_ccitt_g4(arena, raw, raw_len, columns, rows, black_is_1, &dec) != PDF_OK)
            return PDF_ERR_UNSUPPORTED;

        sample_bytes = dec.data;
        bpc = 1;
        ncomp = 1;
        row_bytes = (columns + 7) / 8;
        width = columns; /* por si difiere levemente del /Width declarado */
    }
    /* --- JBIG2Decode (ver DESIGN.md secciones 91-92): mismo patron que
     * CCITTFaxDecode arriba -- produce bits empaquetados 1bpp que el
     * desempaquetado generico de mas abajo lee igual que cualquier
     * imagen cruda de 1 bpc (pdf_filter_jbig2 ya normaliza la
     * convencion de polaridad, ver ese archivo). El unico parametro
     * real es /JBIG2Globals (un stream compartido de segmentos,
     * tipicamente diccionarios de simbolos reusados entre paginas --
     * esta implementacion no soporta diccionarios de simbolos, pero
     * igual hay que pasarlo: podria traer legitimamente un segmento de
     * informacion de pagina u otro dato que si hace falta). */
    else if (filter_name != NULL && strcmp(filter_name, "JBIG2Decode") == 0)
    {
        pdf_obj *parms = pdf_dict_get(img_dict, "DecodeParms");
        const unsigned char *globals = NULL;
        long globals_len = 0;
        pdf_buf dec;

        if (parms == NULL) parms = pdf_dict_get(img_dict, "DP");
        if (parms != NULL && parms->type == PDF_REF)
            parms = pdf_parser_load_object(st, xref, parms->u.ref.num, arena);
        if (parms != NULL && parms->type == PDF_DICT)
        {
            pdf_obj *g = pdf_dict_get(parms, "JBIG2Globals");
            if (g != NULL && g->type == PDF_REF)
                g = pdf_parser_load_object(st, xref, g->u.ref.num, arena);
            if (g != NULL && g->type == PDF_STREAM)
            {
                unsigned char *gbuf = (unsigned char *)pdf_arena_alloc(arena, (size_t)g->u.stm.raw_length);
                long glen;
                if (gbuf != NULL)
                {
                    pdf_stream_seek(st, g->u.stm.raw_offset);
                    glen = pdf_stream_read(st, gbuf, g->u.stm.raw_length);
                    if (xref != NULL && xref->crypt.active)
                        glen = pdf_crypt_decrypt(&xref->crypt, g->u.stm.obj_num, g->u.stm.obj_gen, gbuf, glen);
                    globals = gbuf;
                    globals_len = glen;
                }
            }
        }

        if (pdf_filter_jbig2(arena, raw, raw_len, globals, globals_len, width, height, &dec) != PDF_OK)
            return PDF_ERR_UNSUPPORTED;

        sample_bytes = dec.data;
        bpc = 1;
        ncomp = 1;
        row_bytes = (width + 7) / 8;
    }
    else if (filter_name != NULL && strcmp(filter_name, "FlateDecode") == 0)
    {
        pdf_buf dec;
        if (pdf_filter_flate(arena, raw, raw_len, 0, &dec) != PDF_OK)
            return PDF_ERR_UNSUPPORTED;
        sample_bytes = dec.data;
        row_bytes = ((long)width * ncomp * bpc + 7) / 8;
    }
    else if (filter_name == NULL)
    {
        sample_bytes = raw;
        row_bytes = ((long)width * ncomp * bpc + 7) / 8;
    }
    else
    {
        return PDF_ERR_UNSUPPORTED; /* JPXDecode, RunLengthDecode, LZWDecode: no implementados */
    }

    /* --- desempaquetar a RGB24 o a mascara de 1 bit ---------------------- */

    if (is_mask)
    {
        out->mask_bits = (unsigned char *)pdf_arena_alloc(arena, (size_t)width * height);
        if (out->mask_bits == NULL) return PDF_ERR_NOMEM;

        for (y = 0; y < height; y++)
        {
            const unsigned char *row = sample_bytes + (long)y * row_bytes;
            for (x = 0; x < width; x++)
            {
                int byte_i = x / 8, bit_i = 7 - (x % 8);
                int bit = (row[byte_i] >> bit_i) & 1;
                /* Decode default [0 1]: bit 0 = pintar. No se sigue un
                 * /Decode custom (limitacion documentada). */
                out->mask_bits[(long)y * width + x] = (unsigned char)(bit == 0 ? 1 : 0);
            }
        }
        out->width = width; out->height = height; out->is_mask = 1;
        out->alpha = NULL; /* ImageMask no usa SMask (es su propia mascara de 1 bit) */
        return PDF_OK;
    }

    /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 64): el
     * caso RGB directo de 8 bits sin paleta (el mas comun -- imagenes
     * de foto/logo con colorspace RGB o ICCBased-de-3-componentes,
     * SIN transformacion de color de por medio) alocaba un buffer
     * NUEVO ('out->rgb', width*height*3) y copiaba pixel por pixel
     * desde 'sample_bytes' (el buffer YA descomprimido) -- pero
     * cuando no hay indexado, CMYK, escala de grises, ni padding de
     * fila, esos dos buffers contienen EXACTAMENTE los mismos bytes.
     * Confirmado con un caso real (un logo de 2915x1980 con /SMask,
     * Persona-Juridica-THERCONSULT...pdf): esta duplicacion
     * innecesaria (~17MB solo para esa imagen) hacia que el
     * presupuesto de memoria del documento (96MB, el default de
     * pdf_demo.prg) se agotara EXACTAMENTE al decodificar el /SMask
     * de esa imagen (que se decodifica DESPUES de la imagen base, ver
     * pdf_image_load_smask) -- pdf_arena_alloc devolvia NULL en
     * silencio, dejando 'alpha' en NULL, y la imagen se componia
     * OPACA (con su fondo negro real) en vez de transparente. Se
     * reutiliza 'sample_bytes' DIRECTO como 'out->rgb' en este caso
     * (evitando tanto la asignacion nueva como el bucle de copia
     * completo) -- ahorra ~17MB en este caso real especifico, y de
     * forma proporcional en cualquier imagen RGB grande similar. */
    if (!is_indexed && bpc == 8 && ncomp == 3 && row_bytes == (long)width * 3)
    {
        out->rgb = (unsigned char *)sample_bytes;
        out->width = width; out->height = height; out->is_mask = 0;
        out->alpha = pdf_image_load_smask(st, xref, img_dict, arena, width, height);
        return PDF_OK;
    }

    out->rgb = (unsigned char *)pdf_arena_alloc(arena, (size_t)width * height * 3);
    if (out->rgb == NULL) return PDF_ERR_NOMEM;

    for (y = 0; y < height; y++)
    {
        const unsigned char *row = sample_bytes + (long)y * row_bytes;
        unsigned char *orow = out->rgb + (long)y * width * 3;

        for (x = 0; x < width; x++)
        {
            unsigned char r, g, b;

            if (bpc == 8)
            {
                if (is_indexed)
                {
                    int idx = row[x];
                    if (palette != NULL && idx < palette_count)
                    {
                        const unsigned char *pe = palette + (long)idx * palette_base_ncomp;
                        if (palette_base_ncomp == 1) { r = g = b = pe[0]; }
                        else if (palette_base_ncomp == 4)
                            cmyk_to_rgb(pe[0]/255.0, pe[1]/255.0, pe[2]/255.0, pe[3]/255.0, &r, &g, &b);
                        else { r = pe[0]; g = pe[1]; b = pe[2]; }
                    }
                    else { r = g = b = 0; }
                }
                else if (ncomp == 1) { r = g = b = decode_inverted ? (unsigned char)(255 - row[x]) : row[x]; }
                else if (ncomp == 4)
                {
                    const unsigned char *px = row + (long)x * 4;
                    cmyk_to_rgb(px[0]/255.0, px[1]/255.0, px[2]/255.0, px[3]/255.0, &r, &g, &b);
                }
                else
                {
                    const unsigned char *px = row + (long)x * 3;
                    r = px[0]; g = px[1]; b = px[2];
                }
            }
            else if (bpc == 1)
            {
                int byte_i = x / 8, bit_i = 7 - (x % 8);
                int bit = (row[byte_i] >> bit_i) & 1;
                if (is_indexed && palette != NULL)
                {
                    const unsigned char *pe = palette + (long)bit * palette_base_ncomp;
                    if (palette_base_ncomp == 1) { r = g = b = pe[0]; }
                    else { r = pe[0]; g = pe[1]; b = pe[2]; }
                }
                else
                {
                    int white = decode_inverted ? (bit == 0) : (bit != 0);
                    r = g = b = (unsigned char)(white ? 255 : 0);
                }
            }
            else if (bpc == 4)
            {
                /* 2 muestras por byte (nibble alto = primera). Antes se
                 * trataba como "no soportado" -> gris neutro (128,128,128)
                 * -- esto es lo que causaba los logos como recuadros
                 * grises solidos en documentos reales (confirmado con
                 * Flujograma_Propuesto.pdf: los logos usan exactamente
                 * /BitsPerComponent 4 con paleta Indexed). */
                if (is_indexed)
                {
                    int byte_i = x / 2;
                    int idx = (x % 2 == 0) ? (row[byte_i] >> 4) : (row[byte_i] & 0x0F);
                    if (palette != NULL && idx < palette_count)
                    {
                        const unsigned char *pe = palette + (long)idx * palette_base_ncomp;
                        if (palette_base_ncomp == 1) { r = g = b = pe[0]; }
                        else if (palette_base_ncomp == 4)
                            cmyk_to_rgb(pe[0]/255.0, pe[1]/255.0, pe[2]/255.0, pe[3]/255.0, &r, &g, &b);
                        else { r = pe[0]; g = pe[1]; b = pe[2]; }
                    }
                    else { r = g = b = 0; }
                }
                else if (ncomp == 1)
                {
                    int byte_i = x / 2;
                    int v4 = (x % 2 == 0) ? (row[byte_i] >> 4) : (row[byte_i] & 0x0F);
                    r = g = b = (unsigned char)(v4 * 17); /* 0..15 -> 0..255 */
                }
                else
                {
                    int nc = (ncomp == 4) ? 4 : 3;
                    long base = (long)x * nc;
                    int cvals[4], k;
                    for (k = 0; k < nc; k++)
                    {
                        long sidx = base + k;
                        int byte_i = (int)(sidx / 2);
                        int v4 = (sidx % 2 == 0) ? (row[byte_i] >> 4) : (row[byte_i] & 0x0F);
                        cvals[k] = v4 * 17;
                    }
                    if (nc == 4)
                        cmyk_to_rgb(cvals[0]/255.0, cvals[1]/255.0, cvals[2]/255.0, cvals[3]/255.0, &r, &g, &b);
                    else
                    {
                        r = (unsigned char)cvals[0];
                        g = (unsigned char)cvals[1];
                        b = (unsigned char)cvals[2];
                    }
                }
            }
            else
            {
                r = g = b = 128; /* bpc no soportado (2/16): gris neutro en vez de basura */
            }

            orow[x*3+0] = r; orow[x*3+1] = g; orow[x*3+2] = b;
        }
    }

    out->width = width; out->height = height; out->is_mask = 0;
    out->alpha = pdf_image_load_smask(st, xref, img_dict, arena, width, height);

    return PDF_OK;
}

/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 59): mezcla
 * 'c' contra lo que YA esta pintado en (x,y) segun 'alpha8' (0-255),
 * en vez de sobreescribir directo -- asi una imagen con /SMask deja
 * ver lo que hay detras en sus zonas transparentes/semitransparentes
 * (el fondo de la pagina, u otro contenido ya dibujado antes) en vez
 * de tapar todo con un color solido equivocado. Lee el pixel actual
 * directo de 'bmp->pixels' (mismo patron que ya usa pdf_hbfunc.c en
 * otras partes del motor) -- no hay pdf_bitmap_get_pixel publica, y
 * agregar una solo para este uso interno no valia la pena. alpha8==0
 * (totalmente transparente) no toca nada; alpha8==255 (opaco) es
 * identico a pdf_bitmap_set_pixel de siempre. */
/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1, etapa 10 -- ver
 * DESIGN.md seccion 68): esta funcion tenia su PROPIO blending manual
 * (source-over sobre RGB directo), separado del compositor real de
 * pdf_bitmap.c -- ignoraba por completo bmp->opacity (ca del grupo/
 * ExtGState vigente), bmp->blend_mode (/BM), bmp->soft_mask (/SMask) y
 * el knockout de grupos, y no actualizaba bmp->alpha. Una imagen con
 * /SMask dentro de un grupo semitransparente, o con un blend mode no-
 * Normal activo, ignoraba esos efectos por completo. Ahora delega en
 * pdf_bitmap_set_pixel_coverage (el mismo choke point que usan los
 * fills vectoriales y el texto), que ya hace clip/opacity/blend_mode/
 * soft_mask/knockout correctamente -- ver pdf_bitmap.c:403-465. */
static void pdf_image_blend_pixel(pdf_bitmap *bmp, int x, int y, pdf_color c, unsigned char alpha8)
{
    if (alpha8 == 0)
        return; /* totalmente transparente: no pintar nada (atajo, mismo resultado que coverage 0.0) */
    pdf_bitmap_set_pixel_coverage(bmp, x, y, c, alpha8 / 255.0);
}

void pdf_image_draw(pdf_bitmap *bmp, const pdf_image *img,
                     pdf_matrix unit_to_pixel, pdf_color fill_color)
{
    pdf_matrix inv;
    pdf_point corners[4];
    double minx, miny, maxx, maxy;
    int x0, y0, x1, y1, x, y, i;
    double dest_w, dest_h;
    int box_w, box_h;

    if (bmp == NULL || img == NULL || (img->rgb == NULL && img->mask_bits == NULL))
        return;
    if (!mat_invert(unit_to_pixel, &inv))
        return;

    corners[0] = mat_transform(unit_to_pixel, 0.0, 0.0);
    corners[1] = mat_transform(unit_to_pixel, 1.0, 0.0);
    corners[2] = mat_transform(unit_to_pixel, 1.0, 1.0);
    corners[3] = mat_transform(unit_to_pixel, 0.0, 1.0);

    minx = maxx = corners[0].x;
    miny = maxy = corners[0].y;
    for (i = 1; i < 4; i++)
    {
        if (corners[i].x < minx) minx = corners[i].x;
        if (corners[i].x > maxx) maxx = corners[i].x;
        if (corners[i].y < miny) miny = corners[i].y;
        if (corners[i].y > maxy) maxy = corners[i].y;
    }

    x0 = (int)minx; if (x0 < 0) x0 = 0;
    y0 = (int)miny; if (y0 < 0) y0 = 0;
    x1 = (int)maxx + 1; if (x1 > bmp->width)  x1 = bmp->width;
    y1 = (int)maxy + 1; if (y1 > bmp->height) y1 = bmp->height;

    /* Cuando la imagen se dibuja mucho mas chica que su resolucion nativa
     * (comun en escaneos de alta resolucion insertados como thumbnail o
     * en una pagina chica), muestrear un solo pixel de origen por pixel
     * de destino salta la mayoria de los datos -- se ve "punteado"/con
     * aliasing en vez de una reduccion suave. Si hace falta reducir mas
     * de ~1.5x, se promedia un bloque de pixeles de origen por cada
     * pixel de destino (box filter) en vez de nearest-neighbor. */
    dest_w = maxx - minx; if (dest_w < 1.0) dest_w = 1.0;
    dest_h = maxy - miny; if (dest_h < 1.0) dest_h = 1.0;
    box_w = (int)((double)img->width  / dest_w + 0.5);
    box_h = (int)((double)img->height / dest_h + 0.5);
    if (box_w < 1) box_w = 1;
    if (box_h < 1) box_h = 1;
    if (box_w > 32) box_w = 32; /* limite defensivo: evitar costo excesivo en reducciones extremas */
    if (box_h > 32) box_h = 32;

    /* BUG REAL DE RENDIMIENTO (Arturo: "mejorar la velocidad ... con
     * demasiados graficos", medido contra 3240-3241-2.pdf -- una
     * pagina escaneada de pagina completa): 'mat_transform' hacia una
     * multiplicacion de matriz 2x3 COMPLETA por cada pixel de destino
     * (hasta 2.5 millones de veces para esta imagen) para convertir
     * (x,y) a espacio de imagen -- pero 'inv' es FIJA para todo el
     * dibujo, asi que uv(x,y) es LINEAL en x a lo largo de una fila:
     * uv.x avanza 'inv.a' y uv.y avanza 'inv.b' por cada pixel hacia
     * la derecha (misma idea que un DDA de rasterizacion de texturas).
     * Se arma el punto de partida de la fila UNA vez (con
     * mat_transform, seguro) y de ahi en mas se suma en vez de
     * multiplicar. */
    for (y = y0; y < y1; y++)
    {
        pdf_point uv = mat_transform(inv, x0 + 0.5, y + 0.5);
        for (x = x0; x < x1; x++, uv.x += inv.a, uv.y += inv.b)
        {
            int sx, sy;

            if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0)
                continue;

            sx = (int)(uv.x * img->width);
            sy = (int)((1.0 - uv.y) * img->height); /* v=1 (arriba) = fila 0 de la imagen */
            if (sx < 0) sx = 0;
            if (sx >= img->width)  sx = img->width  - 1;
            if (sy < 0) sy = 0;
            if (sy >= img->height) sy = img->height - 1;

            if (box_w <= 1 && box_h <= 1)
            {
                /* 1:1 o ampliacion: nearest-neighbor de siempre */
                if (img->is_mask)
                {
                    if (img->mask_bits[(long)sy * img->width + sx])
                        pdf_bitmap_set_pixel(bmp, x, y, fill_color);
                }
                else
                {
                    const unsigned char *px = img->rgb + ((long)sy * img->width + sx) * 3;
                    if (img->alpha != NULL)
                    {
                        pdf_color c;
                        c.r = px[0] / 255.0; c.g = px[1] / 255.0; c.b = px[2] / 255.0;
                        pdf_image_blend_pixel(bmp, x, y, c, img->alpha[(long)sy * img->width + sx]);
                    }
                    else
                    {
                        /* BUG REAL DE RENDIMIENTO (ver comentario grande de
                         * pdf_bitmap_set_pixel_rgb_u8 en pdf_bitmap.h/.c):
                         * el caso SIN canal alfa propio (la inmensa mayoria
                         * de imagenes DCTDecode -- fotos escaneadas,
                         * fondos) no necesita el viaje byte->double->byte
                         * de pdf_bitmap_set_pixel; se pasan los bytes tal
                         * cual. */
                        pdf_bitmap_set_pixel_rgb_u8(bmp, x, y, px[0], px[1], px[2]);
                    }
                }
            }
            else if (img->is_mask)
            {
                /* mascara: promedio -> si la mayoria de los pixeles del
                 * bloque son "pintar", se pinta (evita perder detalle
                 * fino de texto/lineas por muestreo puntual). */
                int sx0 = sx - box_w / 2, sy0 = sy - box_h / 2;
                int bx, by, paint_count = 0, total = 0;
                for (by = 0; by < box_h; by++)
                {
                    int ssy = sy0 + by;
                    if (ssy < 0 || ssy >= img->height) continue;
                    for (bx = 0; bx < box_w; bx++)
                    {
                        int ssx = sx0 + bx;
                        if (ssx < 0 || ssx >= img->width) continue;
                        total++;
                        if (img->mask_bits[(long)ssy * img->width + ssx])
                            paint_count++;
                    }
                }
                if (total > 0 && paint_count * 2 >= total)
                    pdf_bitmap_set_pixel(bmp, x, y, fill_color);
            }
            else
            {
                /* imagen de color: promedio de area (box filter) */
                int sx0 = sx - box_w / 2, sy0 = sy - box_h / 2;
                int bx, by, count = 0;
                long sum_r = 0, sum_g = 0, sum_b = 0;
                unsigned char fr, fg, fb;

                for (by = 0; by < box_h; by++)
                {
                    int ssy = sy0 + by;
                    if (ssy < 0 || ssy >= img->height) continue;
                    for (bx = 0; bx < box_w; bx++)
                    {
                        int ssx = sx0 + bx;
                        const unsigned char *px;
                        if (ssx < 0 || ssx >= img->width) continue;
                        px = img->rgb + ((long)ssy * img->width + ssx) * 3;
                        sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                        count++;
                    }
                }

                if (count == 0)
                {
                    const unsigned char *px = img->rgb + ((long)sy * img->width + sx) * 3;
                    fr = px[0]; fg = px[1]; fb = px[2];
                }
                else
                {
                    fr = (unsigned char)(sum_r / count);
                    fg = (unsigned char)(sum_g / count);
                    fb = (unsigned char)(sum_b / count);
                }
                /* alpha: se muestrea un solo punto (sx,sy) en vez de
                 * promediar la misma caja -- simplificacion razonable,
                 * los canales alpha (sombras/mascaras suaves) casi
                 * siempre son de baja frecuencia, no deberia notarse. */
                if (img->alpha != NULL)
                {
                    pdf_color c;
                    c.r = fr / 255.0; c.g = fg / 255.0; c.b = fb / 255.0;
                    pdf_image_blend_pixel(bmp, x, y, c, img->alpha[(long)sy * img->width + sx]);
                }
                else
                    pdf_bitmap_set_pixel_rgb_u8(bmp, x, y, fr, fg, fb);
            }
        }
    }
}
