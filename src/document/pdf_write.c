/* pdf_write.c
 *
 * Ver pdf_write.h.
 */

#include "pdf_write.h"
#include "pdf_error.h"
#include <string.h>
#include <stdio.h>

/* ====================================================================
 * Escaneo del ultimo "startxref" real del archivo original -- necesario
 * para el /Prev del trailer nuevo. Se re-escanea en vez de confiar en
 * un campo separado de pdf_xref_table a proposito: mantiene este
 * modulo totalmente autocontenido, sin tocar la logica de lectura de
 * xref ya verificada (ver pdf_xref.c).
 * ==================================================================== */

#define PDF_WRITE_TAIL_SCAN 2048L

static long find_last_startxref(FILE *fp, long file_size)
{
    unsigned char buf[PDF_WRITE_TAIL_SCAN];
    long start, read_len, i;
    long best = -1;

    if (fp == NULL || file_size <= 0) return -1;

    start = file_size - PDF_WRITE_TAIL_SCAN;
    if (start < 0) start = 0;
    read_len = file_size - start;
    if (read_len <= 0 || read_len > PDF_WRITE_TAIL_SCAN) return -1;

    fseek(fp, start, SEEK_SET);
    read_len = (long)fread(buf, 1, (size_t)read_len, fp);

    for (i = 0; i + 9 <= read_len; i++)
    {
        if (memcmp(buf + i, "startxref", 9) == 0)
        {
            long j = i + 9;
            long val = 0;
            int has_digit = 0;
            while (j < read_len && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n' || buf[j] == '\t'))
                j++;
            while (j < read_len && buf[j] >= '0' && buf[j] <= '9')
            {
                val = val * 10 + (buf[j] - '0');
                j++; has_digit = 1;
            }
            if (has_digit)
                best = val; /* ultima ocurrencia valida gana */
        }
    }
    return best;
}

/* ====================================================================
 * Serializacion generica pdf_obj -> sintaxis PDF
 * ==================================================================== */

static void write_name_escaped(FILE *out, const char *name)
{
    const char *p;
    if (name == NULL) { fprintf(out, "/"); return; }
    fputc('/', out);
    for (p = name; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == '(' || c == ')' || c == '<' || c == '>' ||
            c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
            c == '%' || c == '#' || c >= 0x7F)
            fprintf(out, "#%02X", c);
        else
            fputc((int)c, out);
    }
}

static void write_string_literal(FILE *out, const char *data, long len)
{
    long i;
    fputc('(', out);
    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)data[i];
        if (c == '(' || c == ')' || c == '\\')
            fputc('\\', out);
        fputc((int)c, out);
    }
    fputc(')', out);
}

static void write_obj_value(FILE *out, pdf_obj *val);

static void write_dict_entries(FILE *out, pdf_dict_entry *first)
{
    pdf_dict_entry *e;
    fprintf(out, "<<");
    for (e = first; e != NULL; e = e->next)
    {
        fputc(' ', out);
        write_name_escaped(out, e->key);
        fputc(' ', out);
        write_obj_value(out, e->val);
    }
    fprintf(out, " >>");
}

static void write_obj_value(FILE *out, pdf_obj *val)
{
    if (val == NULL) { fprintf(out, "null"); return; }

    switch (val->type)
    {
        case PDF_NULL:
            fprintf(out, "null");
            break;
        case PDF_BOOL:
            fprintf(out, val->u.boolean ? "true" : "false");
            break;
        case PDF_INT:
            fprintf(out, "%ld", val->u.integer);
            break;
        case PDF_REAL:
            fprintf(out, "%.6f", val->u.real);
            break;
        case PDF_STRING:
            write_string_literal(out, val->u.str.data, val->u.str.len);
            break;
        case PDF_NAME:
            write_name_escaped(out, val->u.name);
            break;
        case PDF_REF:
            fprintf(out, "%ld %ld R", val->u.ref.num, val->u.ref.gen);
            break;
        case PDF_ARRAY:
        {
            int i;
            fputc('[', out);
            for (i = 0; i < val->u.arr.count; i++)
            {
                if (i > 0) fputc(' ', out);
                write_obj_value(out, val->u.arr.items[i]);
            }
            fputc(']', out);
            break;
        }
        case PDF_DICT:
            write_dict_entries(out, val->u.dict.first);
            break;
        case PDF_STREAM:
            /* Un stream JAMAS se embebe inline en la sintaxis real de
             * PDF -- si aparece como VALOR de una clave (p.ej. /AP/N
             * apuntando directo a un stream sintetico recien generado,
             * ver pdf_form_generate_text_appearance), el llamador ya
             * le asigno su propio obj_num/obj_gen (pdf_hbfunc.c) --
             * acá solo se emite la referencia; el objeto en si se
             * escribe aparte (ver el loop principal mas abajo, que
             * itera 'touched_objs' -- ese stream tiene que estar
             * incluido ahi tambien). */
            fprintf(out, "%ld %ld R", val->u.stm.obj_num, val->u.stm.obj_gen);
            break;
        default:
            fprintf(out, "null");
            break;
    }
}

static void write_indirect_object(FILE *out, pdf_obj *obj, long num, long gen)
{
    fprintf(out, "%ld %ld obj\n", num, gen);

    if (obj->type == PDF_STREAM)
    {
        pdf_dict_entry *e;
        int has_length = 0;

        fprintf(out, "<<");
        for (e = obj->u.stm.first; e != NULL; e = e->next)
        {
            fputc(' ', out);
            write_name_escaped(out, e->key);
            fputc(' ', out);
            write_obj_value(out, e->val);
            if (strcmp(e->key, "Length") == 0) has_length = 1;
        }
        if (!has_length)
            fprintf(out, " /Length %ld", obj->u.stm.synthetic_len);
        fprintf(out, " >>\nstream\n");

        /* Esta version solo escribe streams SINTETICOS (generados en
         * memoria, ver pdf_obj_new_synthetic_stream) -- todavia no hay
         * ningun camino que edite un stream YA EXISTENTE del archivo
         * original, asi que no hace falta leer de 'st' aca. Si eso
         * cambia en el futuro, agregar la rama que copia
         * raw_offset/raw_length del original. */
        if (obj->u.stm.synthetic_data != NULL && obj->u.stm.synthetic_len > 0)
            fwrite(obj->u.stm.synthetic_data, 1, (size_t)obj->u.stm.synthetic_len, out);

        fprintf(out, "\nendstream\nendobj\n");
    }
    else
    {
        /* BUG REAL ENCONTRADO (fase de resaltado de texto, ver DESIGN.md):
         * esta rama solo sabia serializar PDF_DICT -- si se marca como
         * "tocado" un objeto PDF_ARRAY (caso real: /Annots de una pagina
         * guardado como referencia indirecta a un array suelto, layout
         * comun en PDFs reales), escribia "<< >>" (dict vacio) en vez del
         * array, perdiendo TODO su contenido anterior. write_obj_value ya
         * distingue PDF_DICT/PDF_ARRAY/etc correctamente -- para PDF_DICT
         * el resultado es identico a antes (misma write_dict_entries). */
        write_obj_value(out, obj);
        fprintf(out, "\nendobj\n");
    }
}

/* ====================================================================
 * Entry point
 * ==================================================================== */

#define PDF_WRITE_PATH_MAX 900

int pdf_write_incremental_update(pdf_stream *st, const pdf_xref_table *xref,
                                  pdf_arena *arena,
                                  pdf_obj **touched_objs,
                                  const long *touched_nums, const long *touched_gens,
                                  int n_touched, const char *out_path)
{
    FILE *in_fp, *out_fp;
    char tmp_path[PDF_WRITE_PATH_MAX + 16];
    char bak_path[PDF_WRITE_PATH_MAX + 16];
    long file_size, prev_xref_offset, new_xref_start, size_val;
    unsigned char copybuf[8192];
    long remaining;
    int i;
    pdf_obj *root_ref, *id_arr;

    (void)arena; /* xref_offsets usa un buffer local fijo -- ver abajo */

    if (st == NULL || st->fp == NULL || xref == NULL || out_path == NULL ||
        touched_objs == NULL || touched_nums == NULL || touched_gens == NULL ||
        n_touched <= 0 || n_touched > 4096)
        return PDF_ERR_BADARG;

    if (strlen(out_path) > (size_t)PDF_WRITE_PATH_MAX)
        return PDF_ERR_BADARG;

    in_fp = st->fp;
    file_size = pdf_stream_size(st);
    if (file_size <= 0)
        return PDF_ERR_IO;

    prev_xref_offset = find_last_startxref(in_fp, file_size);
    if (prev_xref_offset < 0)
        return PDF_ERR_BADARG; /* no se pudo determinar el xref original -- no seguir a ciegas */

    sprintf(tmp_path, "%s.pdftmp", out_path);
    sprintf(bak_path, "%s.bak", out_path);

    out_fp = fopen(tmp_path, "wb");
    if (out_fp == NULL)
        return PDF_ERR_IO;

    /* 1) copiar el archivo original byte a byte */
    fseek(in_fp, 0, SEEK_SET);
    remaining = file_size;
    while (remaining > 0)
    {
        long want = (remaining > (long)sizeof(copybuf)) ? (long)sizeof(copybuf) : remaining;
        long got = (long)fread(copybuf, 1, (size_t)want, in_fp);
        if (got <= 0) break;
        fwrite(copybuf, 1, (size_t)got, out_fp);
        remaining -= got;
    }
    fprintf(out_fp, "\n"); /* separador defensivo, por si el original no terminaba en salto de linea */

    /* 2) objetos tocados, guardando offset para la tabla xref nueva.
     * Buffer fijo (no arena) -- n_touched ya esta acotado arriba a un
     * numero chico (PDF_HB_MAX_TOUCHED_OBJS en pdf_hbfunc.c es 128). */
    {
        long offsets_buf[4096];

        size_val = xref->count;
        for (i = 0; i < n_touched; i++)
        {
            offsets_buf[i] = ftell(out_fp);
            write_indirect_object(out_fp, touched_objs[i], touched_nums[i], touched_gens[i]);
            if (touched_nums[i] + 1 > size_val)
                size_val = touched_nums[i] + 1;
        }

        /* 3) tabla xref clasica -- solo los objetos tocados (una
         * subseccion "num 1" por cada uno: mas simple/robusto que
         * agrupar consecutivos, a costa de unos pocos bytes extra --
         * el numero de campos editados de una vez es chico). */
        new_xref_start = ftell(out_fp);
        fprintf(out_fp, "xref\n");
        for (i = 0; i < n_touched; i++)
        {
            fprintf(out_fp, "%ld 1\n", touched_nums[i]);
            fprintf(out_fp, "%010ld %05ld n \n", offsets_buf[i], touched_gens[i]);
        }
    }

    /* 4) trailer -- /Root e /ID iguales al original, /Prev encadena a
     * la seccion xref anterior (que el lector, este motor incluido,
     * sigue sin importar si es tabla clasica o xref stream). */
    fprintf(out_fp, "trailer\n<< /Size %ld", size_val);

    root_ref = pdf_dict_get(xref->trailer, "Root");
    if (root_ref != NULL)
    {
        fprintf(out_fp, " /Root ");
        write_obj_value(out_fp, root_ref);
    }
    id_arr = pdf_dict_get(xref->trailer, "ID");
    if (id_arr != NULL)
    {
        fprintf(out_fp, " /ID ");
        write_obj_value(out_fp, id_arr);
    }
    fprintf(out_fp, " /Prev %ld >>\n", prev_xref_offset);
    fprintf(out_fp, "startxref\n%ld\n%%%%EOF\n", new_xref_start);

    fclose(out_fp);

    /* 5) reemplazo seguro: el original pasa a .bak (nunca se borra) ANTES
     * de mover el temporal a su lugar -- si el ultimo rename() falla,
     * se intenta recuperar el original desde el .bak automaticamente. */
    remove(bak_path); /* si no existe, remove() falla y no importa */
    if (rename(out_path, bak_path) != 0)
    {
        /* out_path no existia o no se pudo mover -- igual seguimos:
         * puede ser la primera vez que se guarda este archivo desde
         * esta sesion y el nombre coincide con el original que SI
         * existe pero en otra ruta relativa -- de cualquier forma, si
         * el rename final tampoco anda, se reporta error. */
    }

    if (rename(tmp_path, out_path) != 0)
    {
        /* fallo el paso final -- intentar devolver el original a su
         * lugar para no dejar a Arturo sin el archivo ni el guardado. */
        rename(bak_path, out_path);
        remove(tmp_path);
        return PDF_ERR_IO;
    }

    return PDF_OK;
}
