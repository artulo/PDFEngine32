/* pdf_object.c
 *
 * Ver pdf_object.h. Nota sobre arrays: como las arenas no soportan
 * "free" individual ni realloc real, un array que crece mas alla de su
 * capacidad inicial pide un bloque nuevo mas grande y copia -- el
 * bloque viejo queda como desperdicio hasta el proximo reset de la
 * arena. Es aceptable: los arrays de un PDF (kids, contents, etc.) son
 * chicos y esto no es un patron que se repita miles de veces por pagina.
 */

#include "pdf_object.h"
#include <string.h>

static pdf_obj *pdf_obj_alloc(pdf_arena *arena)
{
    pdf_obj *o = (pdf_obj *)pdf_arena_alloc(arena, sizeof(pdf_obj));
    return o;
}

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que los demas bugs documentados en este archivo): el patron de
 * riesgo NO es solo "2+ campos de un miembro de union" -- alcanza con
 * escribir 'o->type' seguido de UN SOLO campo de 'o->u' para
 * disparar la miscompilacion (confirmado end-to-end: un
 * pdf_obj_new_name("DeviceGray") corrompido hacia que
 * pdf_colorspace_from_obj recibiera un nombre distinto/basura,
 * fallando en reconocer un colorspace perfectamente valido). Fix
 * aplicado a TODOS los constructores simples de este archivo: 'type'
 * y el campo de 'u' se escriben ambos via memcpy() desde variables
 * locales. */
pdf_obj *pdf_obj_new_null(pdf_arena *arena)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_obj_type t = PDF_NULL;
    if (o == NULL) return NULL;
    memcpy(&o->type, &t, sizeof(t));
    return o;
}

pdf_obj *pdf_obj_new_bool(pdf_arena *arena, int v)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_obj_type t = PDF_BOOL;
    if (o == NULL) return NULL;
    memcpy(&o->type, &t, sizeof(t));
    memcpy(&o->u.boolean, &v, sizeof(v));
    return o;
}

pdf_obj *pdf_obj_new_int(pdf_arena *arena, long v)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_obj_type t = PDF_INT;
    if (o == NULL) return NULL;
    memcpy(&o->type, &t, sizeof(t));
    memcpy(&o->u.integer, &v, sizeof(v));
    return o;
}

pdf_obj *pdf_obj_new_real(pdf_arena *arena, double v)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_obj_type t = PDF_REAL;
    if (o == NULL) return NULL;
    memcpy(&o->type, &t, sizeof(t));
    memcpy(&o->u.real, &v, sizeof(v));
    return o;
}

pdf_obj *pdf_obj_new_string(pdf_arena *arena, const char *data, long len)
{
    pdf_obj *o;
    char *copy;

    o = pdf_obj_alloc(arena);
    if (o == NULL) return NULL;

    copy = NULL;
    if (len > 0)
    {
        copy = (char *)pdf_arena_alloc(arena, (size_t)len);
        if (copy == NULL) return NULL;
        memcpy(copy, data, (size_t)len);
    }

    /* BUG REAL ENCONTRADO (misma familia que pdf_obj_new_ref/
     * pdf_obj_new_array/pdf_obj_new_stream -- ver comentarios ahi):
     * dos escrituras directas consecutivas a campos adyacentes de un
     * miembro de union ('u.str.data'/'u.str.len') son parte del mismo
     * patron de riesgo de miscompilacion de bcc32 7.70 -- se aplica el
     * mismo fix preventivo via memcpy() aunque este caso puntual no se
     * haya confirmado corrompido con un reproductor aislado (a
     * diferencia de ref/array/stream), por consistencia y porque el
     * costo de la proteccion es nulo. */
    {
        pdf_obj_type t = PDF_STRING;
        memcpy(&o->type, &t, sizeof(t));
    }
    memcpy(&o->u.str.data, &copy, sizeof(copy));
    memcpy(&o->u.str.len, &len, sizeof(len));
    return o;
}

pdf_obj *pdf_obj_new_name(pdf_arena *arena, const char *name)
{
    pdf_obj *o;
    size_t len;
    char *copy;
    pdf_obj_type t;

    o = pdf_obj_alloc(arena);
    if (o == NULL) return NULL;

    len = strlen(name);
    copy = (char *)pdf_arena_alloc(arena, len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, name, len + 1);

    t = PDF_NAME;
    memcpy(&o->type, &t, sizeof(t));
    memcpy(&o->u.name, &copy, sizeof(copy));
    return o;
}

pdf_obj *pdf_obj_new_ref(pdf_arena *arena, long num, long gen)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    long tmp_num = num;
    long tmp_gen = gen;
    if (o == NULL) return NULL;
    if (tmp_num == 0 && tmp_gen != 0)
    {
        long swap = tmp_num;
        tmp_num = tmp_gen;
        tmp_gen = swap;
    }
    {
        pdf_obj_type t = PDF_REF;
        memcpy(&o->type, &t, sizeof(t));
    }
    memcpy(&o->u.ref.num, &tmp_num, sizeof(long));
    memcpy(&o->u.ref.gen, &tmp_gen, sizeof(long));
    return o;
}

#define PDF_ARRAY_MIN_CAP 4
#define PDF_ARRAY_MAX_CAP 1073741824

static int pdf_array_capacity_bytes(int capacity, size_t *bytes)
{
    if (bytes == NULL || capacity < 0)
        return PDF_ERR_BADARG;

    if (capacity == 0)
    {
        *bytes = 0;
        return PDF_OK;
    }

    if ((size_t)capacity > ((size_t)-1) / sizeof(pdf_obj *))
        return PDF_ERR_OVERFLOW;

    *bytes = sizeof(pdf_obj *) * (size_t)capacity;
    return PDF_OK;
}

pdf_obj *pdf_obj_new_array(pdf_arena *arena, int capacity_hint)
{
    pdf_obj *o;
    int cap;
    size_t bytes;
    pdf_obj **items;
    int zero_count;

    if (arena == NULL)
        return NULL;

    cap = capacity_hint > 0 ? capacity_hint : PDF_ARRAY_MIN_CAP;
    if (cap < PDF_ARRAY_MIN_CAP)
        cap = PDF_ARRAY_MIN_CAP;
    if (cap > PDF_ARRAY_MAX_CAP)
        return NULL;

    if (pdf_array_capacity_bytes(cap, &bytes) != PDF_OK)
        return NULL;

    o = pdf_obj_alloc(arena);
    if (o == NULL)
        return NULL;

    {
        pdf_obj_type t = PDF_ARRAY;
        memcpy(&o->type, &t, sizeof(t));
    }
    items = (pdf_obj **)pdf_arena_alloc(arena, bytes);
    if (items == NULL)
        return NULL;

    zero_count = 0;
    memcpy(&o->u.arr.items, &items, sizeof(items));
    memcpy(&o->u.arr.count, &zero_count, sizeof(zero_count));
    memcpy(&o->u.arr.capacity, &cap, sizeof(cap));
    return o;
}

int pdf_array_push(pdf_arena *arena, pdf_obj *array_obj, pdf_obj *item)
{
    int new_cap;
    size_t bytes;
    pdf_obj **new_items;
    pdf_obj **cur_items;
    int cur_count, cur_capacity;

    if (arena == NULL || array_obj == NULL || array_obj->type != PDF_ARRAY)
        return PDF_ERR_BADARG;

    memcpy(&cur_count, &array_obj->u.arr.count, sizeof(cur_count));
    memcpy(&cur_capacity, &array_obj->u.arr.capacity, sizeof(cur_capacity));
    memcpy(&cur_items, &array_obj->u.arr.items, sizeof(cur_items));

    if (cur_count < 0 || cur_capacity < 0 || cur_count > cur_capacity)
        return PDF_ERR_BADARG;

    if (cur_count < cur_capacity)
    {
        cur_items[cur_count] = item;
        cur_count++;
        memcpy(&array_obj->u.arr.count, &cur_count, sizeof(cur_count));
        return PDF_OK;
    }

    if (cur_capacity >= PDF_ARRAY_MAX_CAP || cur_capacity > PDF_ARRAY_MAX_CAP / 2)
        return PDF_ERR_OVERFLOW;

    new_cap = cur_capacity * 2;
    if (pdf_array_capacity_bytes(new_cap, &bytes) != PDF_OK)
        return PDF_ERR_OVERFLOW;

    new_items = (pdf_obj **)pdf_arena_alloc(arena, bytes);
    if (new_items == NULL)
        return PDF_ERR_NOMEM;

    if (cur_count > 0)
        memcpy(new_items, cur_items, sizeof(pdf_obj *) * (size_t)cur_count);

    memcpy(&array_obj->u.arr.items, &new_items, sizeof(new_items));
    memcpy(&array_obj->u.arr.capacity, &new_cap, sizeof(new_cap));
    new_items[cur_count] = item;
    cur_count++;
    memcpy(&array_obj->u.arr.count, &cur_count, sizeof(cur_count));

    return PDF_OK;
}

int pdf_array_remove_at(pdf_obj *array_obj, int index)
{
    pdf_obj **cur_items;
    int cur_count, i;

    if (array_obj == NULL || array_obj->type != PDF_ARRAY)
        return PDF_ERR_BADARG;

    memcpy(&cur_count, &array_obj->u.arr.count, sizeof(cur_count));
    memcpy(&cur_items, &array_obj->u.arr.items, sizeof(cur_items));

    if (index < 0 || index >= cur_count)
        return PDF_ERR_BADARG;

    for (i = index; i < cur_count - 1; i++)
        cur_items[i] = cur_items[i + 1];

    cur_count--;
    memcpy(&array_obj->u.arr.count, &cur_count, sizeof(cur_count));

    return PDF_OK;
}

pdf_obj *pdf_obj_new_dict(pdf_arena *arena)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_obj_type t = PDF_DICT;
    pdf_dict_entry *null_first = NULL;
    if (o == NULL) return NULL;
    memcpy(&o->type, &t, sizeof(t));
    memcpy(&o->u.dict.first, &null_first, sizeof(null_first));
    return o;
}

pdf_obj *pdf_obj_new_stream(pdf_arena *arena, pdf_obj *dict_obj,
                             long raw_offset, long raw_length,
                             long obj_num, long obj_gen)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_dict_entry *first;
    const unsigned char *no_synth = NULL;
    long zero_len = 0;
    if (o == NULL) return NULL;
    {
        pdf_obj_type t = PDF_STREAM;
        memcpy(&o->type, &t, sizeof(t));
    }
    first = (dict_obj != NULL && dict_obj->type == PDF_DICT)
          ? dict_obj->u.dict.first : NULL;
    memcpy(&o->u.stm.first, &first, sizeof(first));
    memcpy(&o->u.stm.raw_offset, &raw_offset, sizeof(raw_offset));
    memcpy(&o->u.stm.raw_length, &raw_length, sizeof(raw_length));
    memcpy(&o->u.stm.obj_num, &obj_num, sizeof(obj_num));
    memcpy(&o->u.stm.obj_gen, &obj_gen, sizeof(obj_gen));
    memcpy(&o->u.stm.synthetic_data, &no_synth, sizeof(no_synth));
    memcpy(&o->u.stm.synthetic_len, &zero_len, sizeof(zero_len));
    return o;
}

pdf_obj *pdf_obj_new_synthetic_stream(pdf_arena *arena, pdf_obj *dict_obj,
                                       const unsigned char *data, long len,
                                       long obj_num, long obj_gen)
{
    pdf_obj *o = pdf_obj_alloc(arena);
    pdf_dict_entry *first;
    unsigned char *copy;
    long zero = 0;
    if (o == NULL) return NULL;

    copy = NULL;
    if (len > 0 && data != NULL)
    {
        copy = (unsigned char *)pdf_arena_alloc(arena, (size_t)len);
        if (copy == NULL) return NULL;
        memcpy(copy, data, (size_t)len);
    }
    else
    {
        len = 0;
    }

    {
        pdf_obj_type t = PDF_STREAM;
        memcpy(&o->type, &t, sizeof(t));
    }
    first = (dict_obj != NULL && dict_obj->type == PDF_DICT)
          ? dict_obj->u.dict.first : NULL;
    memcpy(&o->u.stm.first, &first, sizeof(first));
    memcpy(&o->u.stm.raw_offset, &zero, sizeof(zero));
    memcpy(&o->u.stm.raw_length, &zero, sizeof(zero));
    memcpy(&o->u.stm.obj_num, &obj_num, sizeof(obj_num));
    memcpy(&o->u.stm.obj_gen, &obj_gen, sizeof(obj_gen));
    memcpy(&o->u.stm.synthetic_data, &copy, sizeof(copy));
    memcpy(&o->u.stm.synthetic_len, &len, sizeof(len));
    return o;
}

static pdf_dict_entry **pdf_dict_entry_slot(pdf_obj *obj)
{
    if (obj == NULL) return NULL;
    if (obj->type == PDF_DICT) return &obj->u.dict.first;
    if (obj->type == PDF_STREAM) return &obj->u.stm.first;
    return NULL;
}

int pdf_dict_set(pdf_arena *arena, pdf_obj *obj, const char *key, pdf_obj *val)
{
    pdf_dict_entry **slot;
    pdf_dict_entry *e;
    size_t len;
    char *keycopy;

    if (arena == NULL || obj == NULL || key == NULL)
        return PDF_ERR_BADARG;

    slot = pdf_dict_entry_slot(obj);
    if (slot == NULL)
        return PDF_ERR_BADARG;

    for (e = *slot; e != NULL; e = e->next)
    {
        if (strcmp(e->key, key) == 0)
        {
            memcpy(&e->val, &val, sizeof(val));
            return PDF_OK;
        }
    }

    e = (pdf_dict_entry *)pdf_arena_alloc(arena, sizeof(pdf_dict_entry));
    if (e == NULL)
        return PDF_ERR_NOMEM;

    len = strlen(key);
    if (len > (size_t)-1 - 1)
        return PDF_ERR_OVERFLOW;

    keycopy = (char *)pdf_arena_alloc(arena, len + 1);
    if (keycopy == NULL)
        return PDF_ERR_NOMEM;

    memcpy(keycopy, key, len + 1);

    {
        pdf_dict_entry *next_val = *slot;
        memcpy(&e->key, &keycopy, sizeof(keycopy));
        memcpy(&e->val, &val, sizeof(val));
        memcpy(&e->next, &next_val, sizeof(next_val));
    }
    *slot = e;

    return PDF_OK;
}

pdf_obj *pdf_dict_get(pdf_obj *obj, const char *key)
{
    pdf_dict_entry **slot;
    pdf_dict_entry *e;

    slot = pdf_dict_entry_slot(obj);
    if (slot == NULL)
        return NULL;

    for (e = *slot; e != NULL; e = e->next)
    {
        if (strcmp(e->key, key) == 0)
            return e->val;
    }
    return NULL;
}

long pdf_dict_get_int(pdf_obj *obj, const char *key, long def)
{
    pdf_obj *v = pdf_dict_get(obj, key);
    if (v == NULL) return def;
    if (v->type == PDF_INT) return v->u.integer;
    if (v->type == PDF_REAL) return (long)v->u.real;
    return def;
}

const char *pdf_dict_get_name(pdf_obj *obj, const char *key)
{
    pdf_obj *v = pdf_dict_get(obj, key);
    if (v == NULL || v->type != PDF_NAME) return NULL;
    return v->u.name;
}

double pdf_obj_num(pdf_obj *o, double def)
{
    if (o == NULL) return def;
    if (o->type == PDF_INT)  return (double)o->u.integer;
    if (o->type == PDF_REAL) return o->u.real;
    return def;
}
