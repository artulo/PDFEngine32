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

/* BUG REAL ENCONTRADO Y ARREGLADO -- una miscompilacion de bcc32 7.70
 * (no un error logico nuestro). Con ciertos PDFs reales (reportado
 * contra tests/Los_Kajchas_y_los_proyectos_de_industria.pdf -- texto
 * "ilegible", patron de barras en vez de letras) una referencia
 * indirecta como "/Font 121 0 R" ocasionalmente terminaba con 'num' y
 * 'gen' CAMBIADOS (num=0, gen=121) pese a que los valores que ENTRABAN
 * a esta funcion eran correctos (confirmado con trazas). Se aislo un
 * reproductor minimo (dict anidado armado a mano en un pdf_stream de
 * memoria, con una lectura previa de un token cualquiera antes de
 * parsear el dict -- imita el patron real de un ObjStm: header
 * "objnum offset" x N leido ANTES del objeto en si) que reproducia el
 * problema de forma perfectamente consistente. Se confirmo que es una
 * miscompilacion real (no UB nuestro) porque agregar simples printf()
 * de diagnostico "arreglaba" el sintoma sin cambiar la logica -- firma
 * clasica de un bug de generacion de codigo, no de un puntero colgante.
 *
 * El fix confirmado (verificado 8/8 en el reproductor minimo Y contra
 * el PDF real completo, con pdf_font_load ya resolviendo la fuente
 * correctamente) tiene DOS partes obligatorias juntas: escribir via
 * memcpy() en vez de asignacion directa, Y compilar este archivo junto
 * con pdf_parser.c SIN -O2 (ver win32/Build.bat, CFLAGS_ENG_NOOPT) --
 * ninguna de las dos cosas sola alcanzaba.
 *
 * OJO -- trampa real que costo mucho tiempo detectar: en esta maquina,
 * win32\Build.bat a veces NO recompila pdf_object.c/pdf_parser.c pese a
 * imprimir "Listo" (el mismo problema historico de resolucion de rutas
 * documentado en Build.bat para otros archivos) -- verificar SIEMPRE
 * que el .obj resultante contenga los cambios nuevos (o compilar estos
 * dos archivos a mano con bcc32 directo) antes de descartar un fix como
 * "no funciono".
 *
 * Se agrega ademas una red de seguridad basada en una garantia real del
 * estandar PDF: el objeto numero 0 SIEMPRE es la cabeza de la lista
 * libre (ISO 32000-1 7.5.4) y NUNCA una referencia valida en un PDF
 * bien formado -- si 'num' sale 0 pero 'gen' es distinto de 0, es
 * informacion suficiente para saber que algo se corrompio y vale la
 * pena corregirlo intercambiando num/gen de vuelta -- en el peor caso
 * (un PDF realmente malformado que de verdad tenga gen!=0 en una
 * referencia rota) no se pierde nada, ya era una referencia invalida
 * de cualquier forma. */
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

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que el bug de pdf_obj_new_ref/try_lex_ref ya documentado): bcc32 7.70
 * miscompila la secuencia de asignaciones directas consecutivas a los
 * campos int adyacentes de un miembro de union struct (aqui
 * 'u.arr.count'/'u.arr.capacity', antes 'u.ref.num'/'u.ref.gen') --
 * confirmado con un reproductor minimo aislado (arena+pdf_object solos,
 * sin parser): tras pdf_obj_new_array(arena,4) el objeto quedaba con
 * count=4 (deberia ser 0) y capacity=0 (deberia ser 4) -- los DOS
 * valores literalmente intercambiados -- y en pdf_array_push() el
 * puntero 'items' escrito quedaba como un contador (1,2,3,4...) en vez
 * de una direccion real. IMPORTANTE: a diferencia del bug de
 * pdf_obj_new_ref, este reproduce aunque el archivo YA se compila sin
 * -O2 (CFLAGS_ENG_NOOPT en Build.bat) -- confirma que para este patron
 * de codigo la miscompilacion no depende del nivel de optimizacion, asi
 * que la unica mitigacion confiable es evitar la asignacion directa a
 * los campos: cada escritura a un campo de 'u.arr' pasa por memcpy()
 * desde una variable local, igual criterio que pdf_obj_new_ref. Como
 * TODO objeto PDF_ARRAY pasa por estas dos funciones (MediaBox, Kids,
 * Contents, Widths, y -- relevante para Functions/Shadings -- /Domain,
 * /Range, /C0, /C1, /Coords), este bug podia corromper silenciosamente
 * cualquier array real sin que el sintoma fuera obvio (un MediaBox mal
 * leido cae al default carta 612x792, que "parece" razonable para
 * muchos PDFs reales -- por eso paso desapercibido hasta ahora). */
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

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que pdf_obj_new_ref/pdf_obj_new_array, ver comentarios ahi): esta
 * funcion escribe CINCO campos consecutivos del miembro de union
 * 'u.stm' via asignacion directa -- confirmado que dispara la misma
 * miscompilacion de bcc32 7.70 (reproducido end-to-end: al leer un
 * cross-reference STREAM real de tests/Conveyor_Handbook.pdf, el
 * objeto stream resultante quedaba con 'raw_offset'/'raw_length'
 * corrompidos, causando un crash mas adelante en
 * pdf_decode_stream_generic). Mismo fix: cada campo se escribe via
 * memcpy() desde una variable local en vez de asignacion directa. */
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

/* Ver pdf_obj_new_stream arriba -- mismo patron de escritura via
 * memcpy() (misma familia de bug de bcc32 7.70 documentada ahi y en
 * pdf_obj_new_ref/pdf_obj_new_array). */
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

    /* BUG REAL ENCONTRADO (limpieza de anotaciones de prueba, ver
     * DESIGN.md): esta funcion SIEMPRE agregaba una entrada NUEVA al
     * frente de la lista, sin buscar si 'key' ya existia -- llamarla
     * dos veces con la MISMA clave (p.ej. pisar un /Annots que ya
     * tenia contenido, en vez de crearlo desde /Annots ausente) dejaba
     * DOS entradas con el mismo nombre en el dict. pdf_dict_get()
     * (mas abajo) siempre devuelve la PRIMERA que encuentra recorriendo
     * la lista, y como la entrada nueva queda al FRENTE, LEER el dict
     * desde este mismo motor "andaba bien" -- pero write_dict_entries()
     * (pdf_write.c) escribe TODAS las entradas sin filtrar duplicados,
     * asi que el archivo guardado terminaba con la clave repetida DOS
     * VECES en la sintaxis real. La norma dice que ante una clave
     * duplicada "la ultima ocurrencia gana" -- como la entrada VIEJA
     * queda ULTIMA en la lista (la nueva se prepende adelante), un
     * lector conforme (confirmado con pikepdf/qpdf, que directamente
     * avisa "dictionary has duplicated key") terminaba usando el valor
     * VIEJO, ignorando por completo la actualizacion. Nunca se habia
     * disparado antes porque todo el codigo de este proyecto que llama
     * pdf_dict_set() sobre "Annots"/"V"/etc lo hacia SOLO cuando la
     * clave todavia no existia (el caso "falta" de las 3 resoluciones
     * de /Annots, ver Pdf_AnnotAddHighlight) -- limpiar TODAS las
     * anotaciones de una pagina que YA tenia /Annots fue el primer
     * caso real que reescribe una clave EXISTENTE.
     *
     * Fix: buscar la entrada existente primero -- si esta, actualizar
     * su 'val' IN PLACE (misma entrada, mismo lugar en la lista, cero
     * duplicados posibles); si no esta, agregar una nueva al frente
     * como antes. */
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

    /* Misma familia de bugs documentados en pdf_obj_new_ref/
     * pdf_obj_new_array/pdf_xref.c/pdf_stream.c: tres escrituras
     * directas consecutivas a campos adyacentes de un struct recien
     * alocado ('e->key'/'e->val'/'e->next') -- fix preventivo via
     * memcpy(), mismo estilo que el resto del motor. */
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
