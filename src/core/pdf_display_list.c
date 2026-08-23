/* pdf_display_list.c
 *
 * Implementacion de la display list de PDFEngine32.
 */
#include "pdf_display_list.h"
#include <string.h>

struct pdf_display_list_record_s
{
    pdf_display_list_record *next;
    char *opname;
    pdf_obj **args;
    int nargs;
    pdf_obj *inline_dict;
    unsigned char *inline_data;
    long inline_len;
    int is_inline_image;
};

static char *dl_strdup(pdf_arena *arena, const char *s)
{
    size_t n;
    char *p;

    if (arena == NULL || s == NULL)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)pdf_arena_alloc(arena, n);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    return p;
}

static pdf_obj *dl_clone_obj(pdf_arena *arena, const pdf_obj *src);

static pdf_obj *dl_clone_dict(pdf_arena *arena, const pdf_obj *src)
{
    pdf_obj *dst;
    pdf_dict_entry *e;
    int rc;

    dst = pdf_obj_new_dict(arena);
    if (dst == NULL)
        return NULL;

    e = src->u.dict.first;
    while (e != NULL)
    {
        pdf_obj *v = dl_clone_obj(arena, e->val);
        if (v == NULL)
            return NULL;
        rc = pdf_dict_set(arena, dst, e->key, v);
        if (rc != PDF_OK)
            return NULL;
        e = e->next;
    }
    return dst;
}

static pdf_obj *dl_clone_obj(pdf_arena *arena, const pdf_obj *src)
{
    pdf_obj *dst;
    int i;
    int rc;

    if (arena == NULL || src == NULL)
        return NULL;

    switch (src->type)
    {
    case PDF_NULL:
        return pdf_obj_new_null(arena);

    case PDF_BOOL:
        return pdf_obj_new_bool(arena, src->u.boolean);

    case PDF_INT:
        return pdf_obj_new_int(arena, src->u.integer);

    case PDF_REAL:
        return pdf_obj_new_real(arena, src->u.real);

    case PDF_STRING:
        return pdf_obj_new_string(arena, src->u.str.data, src->u.str.len);

    case PDF_NAME:
        return pdf_obj_new_name(arena, src->u.name);

    case PDF_REF:
        return pdf_obj_new_ref(arena, src->u.ref.num, src->u.ref.gen);

    case PDF_ARRAY:
        dst = pdf_obj_new_array(arena, src->u.arr.count);
        if (dst == NULL)
            return NULL;
        for (i = 0; i < src->u.arr.count; i++)
        {
            pdf_obj *v = dl_clone_obj(arena, src->u.arr.items[i]);
            if (v == NULL)
                return NULL;
            rc = pdf_array_push(arena, dst, v);
            if (rc != PDF_OK)
                return NULL;
        }
        return dst;

    case PDF_DICT:
        return dl_clone_dict(arena, src);

    case PDF_STREAM:
        {
            pdf_obj temp_dict;
            pdf_obj *dict_copy;

            temp_dict.type = PDF_DICT;
            temp_dict.u.dict.first = src->u.stm.first;
            dict_copy = dl_clone_dict(arena, &temp_dict);
            if (dict_copy == NULL)
                return NULL;

            dst = pdf_obj_new_stream(arena, dict_copy,
                                     src->u.stm.raw_offset,
                                     src->u.stm.raw_length,
                                     src->u.stm.obj_num,
                                     src->u.stm.obj_gen);
            return dst;
        }

    default:
        return NULL;
    }
}

static void dl_record_op(void *user, const char *opname,
                         pdf_obj **args, int nargs)
{
    pdf_display_list *list = (pdf_display_list *)user;
    pdf_display_list_record *rec;
    int i;

    if (list == NULL || !list->initialized || list->error != PDF_OK)
        return;
    if (opname == NULL || nargs < 0)
    {
        list->error = PDF_ERR_BADARG;
        return;
    }

    rec = (pdf_display_list_record *)pdf_arena_alloc(&list->arena,
                                                     sizeof(*rec));
    if (rec == NULL)
    {
        list->error = PDF_ERR_NOMEM;
        return;
    }
    memset(rec, 0, sizeof(*rec));

    rec->opname = dl_strdup(&list->arena, opname);
    if (rec->opname == NULL)
    {
        list->error = PDF_ERR_NOMEM;
        return;
    }

    rec->nargs = nargs;
    if (nargs > 0)
    {
        rec->args = (pdf_obj **)pdf_arena_alloc(&list->arena,
                              sizeof(pdf_obj *) * (size_t)nargs);
        if (rec->args == NULL)
        {
            list->error = PDF_ERR_NOMEM;
            return;
        }
        for (i = 0; i < nargs; i++)
        {
            rec->args[i] = dl_clone_obj(&list->arena, args[i]);
            if (rec->args[i] == NULL)
            {
                list->error = PDF_ERR_NOMEM;
                return;
            }
        }
    }

    if (list->last != NULL)
        list->last->next = rec;
    else
        list->first = rec;
    list->last = rec;
    list->count++;
}

static void dl_record_inline_image(void *user, pdf_obj *dict_obj,
                                   const unsigned char *data, long len)
{
    pdf_display_list *list = (pdf_display_list *)user;
    pdf_display_list_record *rec;

    if (list == NULL || !list->initialized || list->error != PDF_OK)
        return;
    if (dict_obj == NULL || len < 0 || (len > 0 && data == NULL))
    {
        list->error = PDF_ERR_BADARG;
        return;
    }

    rec = (pdf_display_list_record *)pdf_arena_alloc(&list->arena,
                                                     sizeof(*rec));
    if (rec == NULL)
    {
        list->error = PDF_ERR_NOMEM;
        return;
    }
    memset(rec, 0, sizeof(*rec));

    rec->inline_dict = dl_clone_obj(&list->arena, dict_obj);
    if (rec->inline_dict == NULL)
    {
        list->error = PDF_ERR_NOMEM;
        return;
    }

    if (len > 0)
    {
        rec->inline_data = (unsigned char *)pdf_arena_alloc(&list->arena,
                                                            (size_t)len);
        if (rec->inline_data == NULL)
        {
            list->error = PDF_ERR_NOMEM;
            return;
        }
        memcpy(rec->inline_data, data, (size_t)len);
    }
    rec->inline_len = len;
    rec->is_inline_image = 1;

    if (list->last != NULL)
        list->last->next = rec;
    else
        list->first = rec;
    list->last = rec;
    list->count++;
}

static const pdf_device_ops s_display_list_ops =
{
    dl_record_op,
    dl_record_inline_image
};

int pdf_display_list_init(pdf_display_list *list, pdf_ledger *ledger,
                          size_t block_size)
{
    int rc;

    if (list == NULL || ledger == NULL)
        return PDF_ERR_BADARG;

    memset(list, 0, sizeof(*list));
    if (block_size == 0)
        block_size = 64UL * 1024UL;

    rc = pdf_arena_init(&list->arena, ledger, block_size, "display-list");
    if (rc != PDF_OK)
        return rc;

    list->initialized = 1;
    list->error = PDF_OK;
    pdf_device_init(&list->device, &s_display_list_ops, list);
    return PDF_OK;
}

void pdf_display_list_reset(pdf_display_list *list)
{
    if (list == NULL || !list->initialized)
        return;

    pdf_arena_reset(&list->arena);
    list->first = NULL;
    list->last = NULL;
    list->count = 0;
    list->error = PDF_OK;
}

void pdf_display_list_destroy(pdf_display_list *list)
{
    if (list == NULL || !list->initialized)
        return;

    pdf_device_reset(&list->device);
    pdf_arena_destroy(&list->arena);
    memset(list, 0, sizeof(*list));
}

pdf_device *pdf_display_list_get_device(pdf_display_list *list)
{
    if (list == NULL || !list->initialized)
        return NULL;
    return &list->device;
}

int pdf_display_list_run(const pdf_display_list *list, pdf_device *device)
{
    const pdf_display_list_record *rec;

    if (list == NULL || device == NULL || !list->initialized ||
        device->ops == NULL || device->ops->op == NULL)
        return PDF_ERR_BADARG;

    if (list->error != PDF_OK)
        return list->error;

    rec = list->first;
    while (rec != NULL)
    {
        if (rec->is_inline_image)
            pdf_device_emit_inline_image(device, rec->inline_dict,
                                         rec->inline_data, rec->inline_len);
        else
            pdf_device_emit(device, rec->opname, rec->args, rec->nargs);
        rec = rec->next;
    }
    return PDF_OK;
}

unsigned long pdf_display_list_count(const pdf_display_list *list)
{
    if (list == NULL || !list->initialized)
        return 0;
    return list->count;
}

int pdf_display_list_error(const pdf_display_list *list)
{
    if (list == NULL || !list->initialized)
        return PDF_ERR_BADARG;
    return list->error;
}
