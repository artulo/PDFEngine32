/* pdf_object.h
 *
 * Modelo de objetos PDF (numero, booleano, string, nombre, array, dict,
 * referencia indirecta, stream). Todos los nodos viven en una pdf_arena
 * (tipicamente doc_arena para objetos de larga vida, o page_arena/
 * decode_arena para objetos de vida corta durante el parseo de una
 * pagina) -- nunca malloc suelto, para que liberarlos sea tan barato
 * como resetear la arena entera.
 */

#ifndef PDF_OBJECT_H
#define PDF_OBJECT_H

#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_obj_type_e
{
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_INT,
    PDF_REAL,
    PDF_STRING,
    PDF_NAME,
    PDF_ARRAY,
    PDF_DICT,
    PDF_REF,      /* referencia indirecta "N G R" */
    PDF_STREAM    /* dict + puntero a los datos crudos (sin decodificar) */
} pdf_obj_type;

typedef struct pdf_obj_s pdf_obj;

typedef struct pdf_dict_entry_s
{
    const char              *key;   /* nombre sin la barra, copiado en arena */
    pdf_obj                  *val;
    struct pdf_dict_entry_s *next;
} pdf_dict_entry;

struct pdf_obj_s
{
    pdf_obj_type type;
    union
    {
        int            boolean;
        long           integer;
        double         real;
        struct { const char *data; long len; } str;   /* PDF_STRING */
        const char    *name;                            /* PDF_NAME  */
        struct { pdf_obj **items; int count; int capacity; }         arr;
        struct { pdf_dict_entry *first; }               dict;
        struct { long num; long gen; }                  ref;
        struct {
            pdf_dict_entry *first;   /* dict del stream */
            long             raw_offset; /* offset absoluto en el archivo del primer byte de datos */
            long             raw_length; /* segun /Length (puede ser indirecto: resuelto aparte) */
            long             obj_num;    /* numero de objeto ("N" de "N G obj") -- para desencriptar por objeto */
            long             obj_gen;    /* generacion ("G" de "N G obj") */
            /* Stream SINTETICO (AcroForm, ver pdf_obj_new_synthetic_stream
             * y pdf_form.c): NULL/0 = stream normal, bytes se leen del
             * archivo original en raw_offset/raw_length (comportamiento
             * de siempre, sin cambios). Si 'synthetic_data' != NULL, ESOS
             * son los bytes reales del stream (ya sin filtros, listos
             * para pdf_content_run) -- generado en memoria (p.ej. la
             * apariencia de un campo de texto recien editado), nunca
             * escrito todavia al archivo -- raw_offset/raw_length se
             * ignoran por completo en ese caso. */
            const unsigned char *synthetic_data;
            long                  synthetic_len;
        } stm;
    } u;
};

/* --- construccion de objetos, todos sobre 'arena' -------------------- */

pdf_obj *pdf_obj_new_null(pdf_arena *arena);
pdf_obj *pdf_obj_new_bool(pdf_arena *arena, int v);
pdf_obj *pdf_obj_new_int(pdf_arena *arena, long v);
pdf_obj *pdf_obj_new_real(pdf_arena *arena, double v);
pdf_obj *pdf_obj_new_string(pdf_arena *arena, const char *data, long len);
pdf_obj *pdf_obj_new_name(pdf_arena *arena, const char *name);
pdf_obj *pdf_obj_new_ref(pdf_arena *arena, long num, long gen);
pdf_obj *pdf_obj_new_array(pdf_arena *arena, int capacity_hint);
pdf_obj *pdf_obj_new_dict(pdf_arena *arena);
pdf_obj *pdf_obj_new_stream(pdf_arena *arena, pdf_obj *dict_obj,
                             long raw_offset, long raw_length,
                             long obj_num, long obj_gen);

/* Stream SINTETICO (ver comentario del campo 'synthetic_data' arriba):
 * 'data'/'len' se COPIAN a la arena (el llamador no necesita mantener
 * el buffer original vivo). Pensado para contenido generado en memoria
 * que todavia no existe en ningun archivo (p.ej. una apariencia de
 * campo AcroForm recien editada, ver pdf_form.c) -- 'obj_num' es el
 * numero de objeto NUEVO que el escritor (pdf_write.c) le va a asignar
 * al guardar; hasta entonces sirve solo para identificarlo en la lista
 * de objetos tocados. */
pdf_obj *pdf_obj_new_synthetic_stream(pdf_arena *arena, pdf_obj *dict_obj,
                                       const unsigned char *data, long len,
                                       long obj_num, long obj_gen);

/* Agrega un item a un array (PDF_ARRAY). Devuelve PDF_OK o un codigo de error. */
int pdf_array_push(pdf_arena *arena, pdf_obj *array_obj, pdf_obj *item);

/* Setea key->val en un dict (PDF_DICT o PDF_STREAM). Copia 'key' a la
 * arena. Devuelve PDF_OK o un codigo de error. */
int pdf_dict_set(pdf_arena *arena, pdf_obj *obj, const char *key, pdf_obj *val);

/* Busca 'key' en un dict/stream. Devuelve NULL si no existe. */
pdf_obj *pdf_dict_get(pdf_obj *obj, const char *key);

/* Atajos de lectura con default si el tipo no matchea o no existe. */
long   pdf_dict_get_int(pdf_obj *obj, const char *key, long def);
const char *pdf_dict_get_name(pdf_obj *obj, const char *key);

/* Convierte un pdf_obj numerico (PDF_INT o PDF_REAL) a double, con
 * default si no es numerico o es NULL. Helper compartido para no
 * reimplementar "obj_to_double" en cada modulo que consume operandos
 * (pdf_render.c, pdf_font.c, etc.). */
double pdf_obj_num(pdf_obj *o, double def);

#ifdef __cplusplus
}
#endif

#endif /* PDF_OBJECT_H */
