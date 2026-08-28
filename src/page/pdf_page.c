/* pdf_page.c
 *
 * Ver pdf_page.h.
 */

#include "pdf_page.h"
#include <stddef.h>
#include <string.h>
#include "pdf_content.h"

/* BUG REAL ENCONTRADO (transparencia/shadings, fase 1 -- misma familia
 * que los bugs ya documentados en pdf_object.c/pdf_xref.c/pdf_filter.c):
 * bcc32 7.70 miscompila escrituras directas consecutivas a campos
 * adyacentes de un struct (aca 'pdf_buf.data'/'pdf_buf.len', el
 * resultado del content stream de CADA pagina que se renderiza) --
 * mismo fix en todo este archivo: memcpy() desde una variable local en
 * vez de asignacion directa. */
static void pdf_buf_set(pdf_buf *out, unsigned char *data, long len)
{
    memcpy(&out->data, &data, sizeof(data));
    memcpy(&out->len, &len, sizeof(len));
}

int pdf_page_open(pdf_page *page, pdf_document *doc, int page_number)
{
    int rc;

    if (page == NULL || doc == NULL)
        return PDF_ERR_BADARG;

    page->doc         = doc;
    page->page_number = page_number;

    rc = pdf_arena_init(&page->page_arena, pdf_document_ledger_mut(doc),
                         (size_t)PDF_BLOCK_SIZE_PAGE, "page");
    if (rc != PDF_OK)
        return rc;

    rc = pdf_arena_init(&page->decode_arena, pdf_document_ledger_mut(doc),
                         (size_t)PDF_BLOCK_SIZE_DECODE, "decode");
    if (rc != PDF_OK)
    {
        pdf_arena_destroy(&page->page_arena);
        return rc;
    }

    return PDF_OK;
}

void pdf_page_close(pdf_page *page)
{
    if (page == NULL)
        return;

    pdf_arena_destroy(&page->decode_arena);
    pdf_arena_destroy(&page->page_arena);
}

void pdf_page_recycle_decode_arena(pdf_page *page)
{
    if (page == NULL)
        return;
    pdf_arena_reset(&page->decode_arena);
}

/* --- contenido de pagina (soporta /Contents ref unica o array) --------- */

#define PDF_PAGE_MAX_CONTENT_STREAMS 64

static int pdf_decode_one_content_stream(pdf_stream *st, const pdf_xref_table *xref, pdf_obj *stm,
                                          pdf_arena *arena, pdf_buf *out)
{
    unsigned char *raw;
    long got;
    pdf_obj *filter_obj;
    const char *chain[4];
    int nfilters, fi;
    unsigned char *cur_data;
    long cur_len;

    pdf_buf_set(out, NULL, 0);

    if (stm == NULL || stm->type != PDF_STREAM || stm->u.stm.raw_length <= 0)
        return PDF_ERR_BADARG;

    raw = (unsigned char *)pdf_arena_alloc(arena, (size_t)stm->u.stm.raw_length);
    if (raw == NULL)
        return PDF_ERR_NOMEM;

    if (pdf_stream_seek(st, stm->u.stm.raw_offset) != PDF_OK)
        return PDF_ERR_IO;

    got = pdf_stream_read(st, raw, stm->u.stm.raw_length);

    if (xref != NULL && xref->crypt.active)
        got = pdf_crypt_decrypt(&xref->crypt, stm->u.stm.obj_num, stm->u.stm.obj_gen, raw, got);

    /* /Filter puede ser un nombre simple O un array de nombres (incluso
     * de UN solo elemento, "[/FlateDecode]" -- algunos generadores lo
     * escriben asi y es igual de valido que "/FlateDecode" a secas;
     * confirmado que quickstart.pdf real lo hace y sin este manejo el
     * stream se pasaba SIN decodificar, quedando el tokenizer de
     * content stream tratando de interpretar bytes Flate crudos como
     * sintaxis PDF -- eso es lo que colgaba el programa). */
    nfilters = 0;
    filter_obj = pdf_dict_get(stm, "Filter");
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

    if (nfilters == 0)
    {
        /* sin filtro: los datos crudos ya son el content stream */
        pdf_buf_set(out, raw, got);
        return PDF_OK;
    }

    cur_data = raw;
    cur_len  = got;
    for (fi = 0; fi < nfilters; fi++)
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
            /* DCTDecode/CCITTFaxDecode no aplican a content streams (son
             * para imagenes) -- cualquier otro filtro no soportado corta
             * aca, sin dejar pasar bytes a medio decodificar. */
            return PDF_ERR_UNSUPPORTED;
        }
        cur_data = dec.data;
        cur_len  = dec.len;
    }

    pdf_buf_set(out, cur_data, cur_len);
    return PDF_OK;
}

int pdf_page_get_content(pdf_stream *st, const pdf_xref_table *xref,
                          pdf_obj *page_obj, pdf_arena *arena, pdf_buf *out)
{
    pdf_obj *contents;
    pdf_buf pieces[PDF_PAGE_MAX_CONTENT_STREAMS];
    int n_pieces;
    long total;
    int i;
    unsigned char *combined;
    long pos;

    if (out != NULL) pdf_buf_set(out, NULL, 0);

    if (st == NULL || xref == NULL || page_obj == NULL || arena == NULL || out == NULL)
        return PDF_ERR_BADARG;

    contents = pdf_dict_get(page_obj, "Contents");
    if (contents == NULL)
        return PDF_ERR_NOTFOUND;

    n_pieces = 0;

    if (contents->type == PDF_REF)
    {
        pdf_obj *stm = pdf_parser_load_object(st, xref, contents->u.ref.num, arena);
        pdf_buf piece;

        if (pdf_decode_one_content_stream(st, xref, stm, arena, &piece) == PDF_OK && piece.len > 0)
            pieces[n_pieces++] = piece;
    }
    else if (contents->type == PDF_ARRAY)
    {
        /* /Contents como array: el estandar PDF exige tratar los streams
         * como si estuvieran concatenados con un separador de linea entre
         * ellos -- patron muy comun en PDFs reales (confirmado contra
         * D-6025IC1r0.pdf y Verificacion de verticalidad.pdf). */
        int count = contents->u.arr.count;
        if (count > PDF_PAGE_MAX_CONTENT_STREAMS)
            count = PDF_PAGE_MAX_CONTENT_STREAMS;

        for (i = 0; i < count; i++)
        {
            pdf_obj *ref = contents->u.arr.items[i];
            pdf_obj *stm;
            pdf_buf piece;

            if (ref == NULL || ref->type != PDF_REF)
                continue;

            stm = pdf_parser_load_object(st, xref, ref->u.ref.num, arena);
            if (pdf_decode_one_content_stream(st, xref, stm, arena, &piece) == PDF_OK && piece.len > 0)
                pieces[n_pieces++] = piece;
        }
    }
    else
    {
        return PDF_ERR_BADARG;
    }

    if (n_pieces == 0)
        return PDF_ERR_NOTFOUND;

    if (n_pieces == 1)
    {
        *out = pieces[0];
        return PDF_OK;
    }

    total = 0;
    for (i = 0; i < n_pieces; i++)
        total += pieces[i].len + 1; /* +1 por el separador de linea */

    combined = (unsigned char *)pdf_arena_alloc(arena, (size_t)total);
    if (combined == NULL)
        return PDF_ERR_NOMEM;

    pos = 0;
    for (i = 0; i < n_pieces; i++)
    {
        memcpy(combined + pos, pieces[i].data, (size_t)pieces[i].len);
        pos += pieces[i].len;
        if (i < n_pieces - 1)
            combined[pos++] = '\n';
    }

    pdf_buf_set(out, combined, pos);
    return PDF_OK;
}

/* --- resolucion recursiva de la N-esima pagina -------------------------- */

/* 'node_ref_num' es el numero de objeto de la referencia /Kids[i] que
 * llevo hasta ESTE 'node' (o -1 para el nodo raiz /Pages, que no llega
 * via ninguna entrada de /Kids sino de /Root/Pages directo) -- se
 * actualiza en cada nivel de la recursion antes de bajar, asi que
 * cuando el nodo hoja (la pagina buscada) hace match, 'node_ref_num'
 * es exactamente SU propio numero de objeto real (ver
 * pdf_document_get_page_ex, agregado en la fase de resaltado de texto
 * para poder marcar una pagina como "tocada" al escribir /Annots
 * nuevo -- un PDF_DICT, a diferencia de PDF_STREAM, no guarda su
 * propio obj_num/obj_gen en el struct pdf_obj). */
static pdf_obj *pdf_walk_page_tree(pdf_stream *st, const pdf_xref_table *xref,
                                    pdf_arena *arena, pdf_obj *node,
                                    int *counter, int target,
                                    long node_ref_num, long *out_num)
{
    pdf_obj *kids = pdf_dict_get(node, "Kids");

    if (kids == NULL || kids->type != PDF_ARRAY)
    {
        /* Nodo sin /Kids: es una hoja (pagina real). Los nodos /Pages
         * intermedios siempre tienen /Kids: si no lo tiene, no hay forma
         * de que sea un nodo intermedio, sin importar si /Type dice
         * "Page" explicitamente o falta (tolerante, como el resto del
         * parser). */
        if (*counter == target)
        {
            if (out_num != NULL) *out_num = node_ref_num;
            return node;
        }
        (*counter)++;
        return NULL;
    }

    {
        int i;
        for (i = 0; i < kids->u.arr.count; i++)
        {
            pdf_obj *ref = kids->u.arr.items[i];
            pdf_obj *child;
            pdf_obj *found;

            if (ref == NULL || ref->type != PDF_REF)
                continue;

            child = pdf_parser_load_object(st, xref, ref->u.ref.num, arena);
            if (child == NULL)
                continue;

            found = pdf_walk_page_tree(st, xref, arena, child, counter, target,
                                        ref->u.ref.num, out_num);
            if (found != NULL)
                return found;
        }
    }

    return NULL;
}

pdf_obj *pdf_document_get_page_ex(pdf_stream *st, const pdf_xref_table *xref,
                                   pdf_arena *arena, int page_index,
                                   long *out_obj_num)
{
    pdf_obj *pages;
    int counter;

    if (out_obj_num != NULL) *out_obj_num = -1;

    if (st == NULL || xref == NULL || arena == NULL || page_index < 0)
        return NULL;

    pages = pdf_document_get_pages_root(st, xref, arena);
    if (pages == NULL)
        return NULL;

    counter = 0;
    return pdf_walk_page_tree(st, xref, arena, pages, &counter, page_index, -1, out_obj_num);
}

pdf_obj *pdf_document_get_page(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, int page_index)
{
    return pdf_document_get_page_ex(st, xref, arena, page_index, NULL);
}


/* -------------------------------------------------------------------------
 * Herencia de atributos del Page Tree
 * ------------------------------------------------------------------------- */

static pdf_obj *pdf_page_resolve_ref_local(pdf_stream *st,
                                            const pdf_xref_table *xref,
                                            pdf_arena *arena,
                                            pdf_obj *obj)
{
    if (obj == NULL)
        return NULL;
    if (obj->type != PDF_REF)
        return obj;
    return pdf_xref_load_object(st, xref, obj->u.ref.num, arena);
}

static int pdf_page_find_target(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, pdf_obj *node,
                                pdf_obj *target, const char *key,
                                pdf_obj **inherited, pdf_obj **result)
{
    pdf_obj *local;
    pdf_obj *kids;
    int i;

    if (node == NULL || node->type != PDF_DICT)
        return 0;

    /* En un nodo /Pages, un valor local se vuelve el valor heredado de todos
     * sus descendientes. El recorrido es depth-first y nunca confunde un
     * nodo intermedio con una hoja. */
    local = pdf_dict_get(node, key);
    if (local != NULL)
    {
        *inherited = local;
    }

    if (node == target)
    {
        *result = local != NULL ? local : *inherited;
        return 1;
    }

    kids = pdf_dict_get(node, "Kids");
    if (kids == NULL || kids->type != PDF_ARRAY)
        return 0;

    for (i = 0; i < kids->u.arr.count; i++)
    {
        pdf_obj *child = pdf_page_resolve_ref_local(st, xref, arena,
                                                     kids->u.arr.items[i]);
        pdf_obj *child_inherited = *inherited;

        if (child == NULL || child->type != PDF_DICT)
            continue;

        if (pdf_page_find_target(st, xref, arena, child, target, key,
                                 &child_inherited, result))
            return 1;
    }

    return 0;
}

pdf_obj *pdf_page_get_inherited(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, pdf_obj *page_obj,
                                const char *key)
{
    pdf_obj *pages_root;
    pdf_obj *inherited;
    pdf_obj *result;

    if (st == NULL || xref == NULL || arena == NULL ||
        page_obj == NULL || page_obj->type != PDF_DICT || key == NULL)
        return NULL;

    /* La clave local gana inmediatamente y evita recorrer todo el arbol. */
    result = pdf_dict_get(page_obj, key);
    if (result != NULL)
        return result;

    pages_root = pdf_document_get_pages_root(st, xref, arena);
    if (pages_root == NULL)
        return NULL;

    inherited = NULL;
    result = NULL;
    if (!pdf_page_find_target(st, xref, arena, pages_root, page_obj, key,
                              &inherited, &result))
        return NULL;

    return result;
}

pdf_obj *pdf_page_get_resources(pdf_stream *st, const pdf_xref_table *xref,
                                pdf_arena *arena, pdf_obj *page_obj)
{
    pdf_obj *resources;

    resources = pdf_page_get_inherited(st, xref, arena, page_obj, "Resources");
    if (resources == NULL)
        return NULL;

    if (resources->type == PDF_REF)
        resources = pdf_xref_load_object(st, xref, resources->u.ref.num, arena);

    if (resources == NULL || resources->type != PDF_DICT)
        return NULL;

    return resources;
}


/* -------------------------------------------------------------------------
 * Ejecucion de pagina sobre un device
 * ------------------------------------------------------------------------- */

int pdf_page_run_device(pdf_stream *st, const pdf_xref_table *xref,
                        pdf_page *page, pdf_obj *page_obj,
                        pdf_device *device)
{
    pdf_buf decoded;
    int rc;

    if (st == NULL || xref == NULL || page == NULL ||
        page_obj == NULL || device == NULL)
        return PDF_ERR_BADARG;

    if (page_obj->type != PDF_DICT)
        return PDF_ERR_BADARG;

    /* Cada ejecucion empieza con una arena de decode limpia. Los objetos
     * de larga vida de la pagina siguen viviendo en page_arena/doc_arena. */
    pdf_arena_reset(&page->decode_arena);

    rc = pdf_page_get_content(st, xref, page_obj,
                              &page->decode_arena, &decoded);
    if (rc != PDF_OK)
        return rc;

    return pdf_content_run_device(decoded.data, decoded.len,
                                  &page->decode_arena, device);
}

int pdf_page_create_display_list(pdf_stream *st, const pdf_xref_table *xref,
                                 pdf_page *page, pdf_obj *page_obj,
                                 pdf_display_list *list)
{
    int rc;
    const pdf_ledger *ledger;

    if (st == NULL || xref == NULL || page == NULL ||
        page_obj == NULL || list == NULL)
        return PDF_ERR_BADARG;

    ledger = pdf_document_ledger(page->doc);
    if (ledger == NULL)
        return PDF_ERR_BADARG;

    /* La display list necesita una arena persistente distinta de decode.
     * Si el llamador reutiliza la misma lista, se limpia primero. */
    if (!list->initialized)
    {
        rc = pdf_display_list_init(list, pdf_document_ledger_mut(page->doc),
                                   64UL * 1024UL);
        if (rc != PDF_OK)
            return rc;
    }
    else
    {
        pdf_display_list_reset(list);
    }

    rc = pdf_page_run_device(st, xref, page, page_obj,
                             pdf_display_list_get_device(list));
    if (rc != PDF_OK)
        return rc;

    rc = pdf_display_list_error(list);
    if (rc != PDF_OK)
        return rc;

    /* Todo lo que el device de display list necesita ya fue clonado en su
     * propia arena. La arena temporal queda disponible para la siguiente
     * pagina/stream. */
    pdf_arena_reset(&page->decode_arena);
    return PDF_OK;
}
