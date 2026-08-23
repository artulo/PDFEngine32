/* pdf_form.c
 *
 * Ver pdf_form.h.
 */

#include "pdf_form.h"
#include <string.h>
#include <stdio.h>

/* Resuelve un pdf_obj que puede ser una referencia indirecta o el
 * objeto directo -- patron repetido en todo el motor (pdf_render.c,
 * pdf_hbfunc.c, etc.), aca centralizado para este archivo. */
static pdf_obj *resolve(pdf_stream *st, const pdf_xref_table *xref,
                         pdf_arena *arena, pdf_obj *obj)
{
    if (obj != NULL && obj->type == PDF_REF)
        return pdf_xref_load_object(st, xref, obj->u.ref.num, arena);
    return obj;
}

/* Info de "donde" se encontro un atributo heredado -- el dict en si
 * (ya resuelto) y su numero/generacion de objeto (0/0 si no se pudo
 * determinar, p.ej. un /Parent embebido directo en vez de referencia
 * indirecta -- caso raro en la practica). Necesario porque al guardar
 * hay que reescribir el objeto DONDE VIVE /V, que puede ser el widget
 * mismo o un campo padre -- no alcanza con el valor solo. */
typedef struct
{
    pdf_obj *dict;
    long     obj_num, obj_gen;
} pdf_form_owner;

/* Busca 'key' en 'widget_dict' y, si no esta, sube la cadena /Parent
 * (hasta 32 niveles, contra ciclos malformados) -- mismo espiritu que
 * pdf_page_get_inherited pero sobre /Parent de campo, no el arbol
 * /Pages. */
static pdf_obj *field_find_inherited(pdf_stream *st, const pdf_xref_table *xref,
                                      pdf_arena *arena, pdf_obj *widget_dict,
                                      long widget_num, long widget_gen,
                                      const char *key, pdf_form_owner *owner_out)
{
    pdf_obj *cur = widget_dict;
    long cur_num = widget_num, cur_gen = widget_gen;
    int depth;

    for (depth = 0; depth < 32 && cur != NULL && cur->type == PDF_DICT; depth++)
    {
        pdf_obj *v = pdf_dict_get(cur, key);
        if (v != NULL)
        {
            if (owner_out != NULL)
            {
                owner_out->dict = cur;
                owner_out->obj_num = cur_num;
                owner_out->obj_gen = cur_gen;
            }
            return v;
        }

        {
            pdf_obj *parent = pdf_dict_get(cur, "Parent");
            if (parent == NULL)
                break;
            if (parent->type == PDF_REF)
            {
                cur_num = parent->u.ref.num;
                cur_gen = parent->u.ref.gen;
                cur = pdf_xref_load_object(st, xref, parent->u.ref.num, arena);
            }
            else
            {
                /* dict embebido directo (no referencia indirecta) --
                 * raro, pero no hay forma de reescribirlo por separado
                 * al guardar; se sigue subiendo igual para clasificar/
                 * mostrar, con num/gen quedando en el ultimo nivel
                 * referenciado (el escritor lo tratara como no
                 * guardable si owner_out->obj_num queda en 0). */
                cur = parent;
            }
        }
    }

    if (owner_out != NULL)
    {
        owner_out->dict = NULL;
        owner_out->obj_num = 0;
        owner_out->obj_gen = 0;
    }
    return NULL;
}

/* /AP /N de un checkbox puede ser un dict de nombre-de-estado -> stream
 * (p.ej. {"Off": ..., "Yes": ...} -- el nombre "on" NO siempre es
 * "Yes"). Devuelve el primer nombre distinto de "Off" encontrado, o
 * cadena vacia si no se pudo resolver. */
static void find_checkbox_on_state(pdf_obj *ap_n_dict, char *out, int out_max)
{
    pdf_dict_entry *e;

    out[0] = 0;
    if (ap_n_dict == NULL || ap_n_dict->type != PDF_DICT)
        return;

    for (e = ap_n_dict->u.dict.first; e != NULL; e = e->next)
    {
        if (e->key != NULL && strcmp(e->key, "Off") != 0)
        {
            int i;
            for (i = 0; i < out_max - 1 && e->key[i] != 0; i++)
                out[i] = e->key[i];
            out[i] = 0;
            return;
        }
    }
}

static void copy_bounded(char *dst, int dst_max, const char *src, long src_len)
{
    long n = src_len;
    if (n > (long)(dst_max - 1)) n = dst_max - 1;
    if (n > 0) memcpy(dst, src, (size_t)n);
    dst[n < 0 ? 0 : n] = 0;
}

int pdf_form_list_fields(pdf_stream *st, const pdf_xref_table *xref,
                          pdf_arena *arena, pdf_obj *page_obj,
                          pdf_form_field *out, int max_fields)
{
    pdf_obj *annots;
    int n = 0;
    int i;

    if (st == NULL || xref == NULL || arena == NULL ||
        page_obj == NULL || page_obj->type != PDF_DICT ||
        out == NULL || max_fields <= 0)
        return 0;

    annots = resolve(st, xref, arena, pdf_dict_get(page_obj, "Annots"));
    if (annots == NULL || annots->type != PDF_ARRAY)
        return 0;

    for (i = 0; i < annots->u.arr.count && n < max_fields; i++)
    {
        pdf_obj *annot_ref = annots->u.arr.items[i];
        pdf_obj *widget;
        const char *subtype;
        long widget_num = 0, widget_gen = 0;
        pdf_form_field *f;
        pdf_obj *ft_obj, *v_obj, *rect_obj, *t_obj, *ff_obj;
        pdf_form_owner v_owner;
        long ff;

        if (annot_ref == NULL) continue;
        if (annot_ref->type == PDF_REF)
        {
            widget_num = annot_ref->u.ref.num;
            widget_gen = annot_ref->u.ref.gen;
        }
        widget = resolve(st, xref, arena, annot_ref);
        if (widget == NULL || widget->type != PDF_DICT) continue;

        subtype = pdf_dict_get_name(widget, "Subtype");
        if (subtype == NULL || strcmp(subtype, "Widget") != 0) continue;

        f = &out[n];
        memset(f, 0, sizeof(*f));
        f->widget_obj = widget;
        f->obj_num = widget_num;
        f->obj_gen = widget_gen;

        rect_obj = pdf_dict_get(widget, "Rect");
        if (rect_obj != NULL && rect_obj->type == PDF_ARRAY && rect_obj->u.arr.count == 4)
        {
            f->rect.x0 = pdf_obj_num(rect_obj->u.arr.items[0], 0.0);
            f->rect.y0 = pdf_obj_num(rect_obj->u.arr.items[1], 0.0);
            f->rect.x1 = pdf_obj_num(rect_obj->u.arr.items[2], 0.0);
            f->rect.y1 = pdf_obj_num(rect_obj->u.arr.items[3], 0.0);
            if (f->rect.x1 < f->rect.x0) { double t = f->rect.x0; f->rect.x0 = f->rect.x1; f->rect.x1 = t; }
            if (f->rect.y1 < f->rect.y0) { double t = f->rect.y0; f->rect.y0 = f->rect.y1; f->rect.y1 = t; }
        }

        t_obj = field_find_inherited(st, xref, arena, widget, widget_num, widget_gen, "T", NULL);
        if (t_obj != NULL && t_obj->type == PDF_STRING)
            copy_bounded(f->name, PDF_FORM_NAME_MAX, t_obj->u.str.data, t_obj->u.str.len);

        ff_obj = field_find_inherited(st, xref, arena, widget, widget_num, widget_gen, "Ff", NULL);
        ff = (ff_obj != NULL) ? (long)pdf_obj_num(ff_obj, 0.0) : 0;
        f->read_only = (ff & 0x1) ? 1 : 0;

        ft_obj = field_find_inherited(st, xref, arena, widget, widget_num, widget_gen, "FT", NULL);
        if (ft_obj != NULL && ft_obj->type == PDF_NAME && strcmp(ft_obj->u.name, "Tx") == 0)
        {
            f->type = PDF_FORM_FIELD_TEXT;
        }
        else if (ft_obj != NULL && ft_obj->type == PDF_NAME && strcmp(ft_obj->u.name, "Btn") == 0)
        {
            /* bit 15 (0x8000) = Radio, bit 16 (0x10000) = Pushbutton --
             * ninguno de los dos es un checkbox simple de esta etapa. */
            if (ff & 0x8000L) f->type = PDF_FORM_FIELD_OTHER;
            else if (ff & 0x10000L) f->type = PDF_FORM_FIELD_OTHER;
            else f->type = PDF_FORM_FIELD_CHECKBOX;
        }
        else
        {
            f->type = PDF_FORM_FIELD_OTHER;
        }

        v_owner.dict = NULL; v_owner.obj_num = 0; v_owner.obj_gen = 0;
        v_obj = field_find_inherited(st, xref, arena, widget, widget_num, widget_gen, "V", &v_owner);

        if (v_obj != NULL)
        {
            /* /V se encontro en 'v_owner.dict' (el widget mismo o un
             * /Parent) -- ESE es el objeto a reescribir al guardar.
             * v_owner.obj_num puede quedar en 0 si esa cadena de
             * /Parent no era referencia indirecta (raro) -- el
             * escritor trata obj_num==0 como "no guardable". */
            f->obj_num = v_owner.obj_num;
            f->obj_gen = v_owner.obj_gen;
        }
        /* si /V no se encontro en ningun lado (campo vacio, nunca
         * llenado), f->obj_num/obj_gen quedan apuntando al widget
         * mismo (el default seteado arriba) -- es donde se va a crear
         * /V la primera vez que se edite. */

        if (f->type == PDF_FORM_FIELD_TEXT)
        {
            if (v_obj != NULL && v_obj->type == PDF_STRING)
                copy_bounded(f->value, PDF_FORM_VALUE_MAX, v_obj->u.str.data, v_obj->u.str.len);
        }
        else if (f->type == PDF_FORM_FIELD_CHECKBOX)
        {
            pdf_obj *ap, *ap_n;

            if (v_obj != NULL && v_obj->type == PDF_NAME)
                copy_bounded(f->value, PDF_FORM_VALUE_MAX, v_obj->u.name, (long)strlen(v_obj->u.name));
            else
                copy_bounded(f->value, PDF_FORM_VALUE_MAX, "Off", 3);

            ap = resolve(st, xref, arena, pdf_dict_get(widget, "AP"));
            ap_n = (ap != NULL) ? resolve(st, xref, arena, pdf_dict_get(ap, "N")) : NULL;
            find_checkbox_on_state(ap_n, f->on_state_name, PDF_FORM_STATE_NAME_MAX);
        }

        n++;
    }

    return n;
}

/* ====================================================================
 * Generacion de apariencia minima para un campo de texto editado (ver
 * pdf_form.h) -- alcance deliberadamente chico: una sola linea,
 * alineada a la izquierda, recortada al /BBox, con la fuente/tamanio
 * de /DA (o un default razonable si no se puede leer). Sin multilinea,
 * sin autotamanio real, sin rich text.
 * ==================================================================== */

/* Extrae "/FontName size Tf" de un /DA simple (el caso comun -- casi
 * todos los /DA reales son "0 g /Helv 10 Tf" o similar). Escaneo
 * manual chico, NO un parser de content stream completo -- alcanza
 * con encontrar el primer "/Nombre" y el numero que le sigue. */
static void parse_da(const char *da, long da_len, char *font_name_out,
                      int font_name_max, double *size_out)
{
    long i;
    font_name_out[0] = 0;
    *size_out = 0.0;
    if (da == NULL) return;

    for (i = 0; i < da_len; i++)
    {
        if (da[i] != '/') continue;

        {
            long j = i + 1;
            int k = 0;
            while (j < da_len && da[j] != ' ' && da[j] != '\t' &&
                   da[j] != '\r' && da[j] != '\n' && k < font_name_max - 1)
            {
                font_name_out[k++] = da[j];
                j++;
            }
            font_name_out[k] = 0;

            while (j < da_len && (da[j] == ' ' || da[j] == '\t' ||
                                   da[j] == '\r' || da[j] == '\n'))
                j++;

            {
                int neg = 0;
                double val = 0.0, frac = 0.0, frac_div = 1.0;
                int has_digit = 0;
                if (j < da_len && da[j] == '-') { neg = 1; j++; }
                while (j < da_len && da[j] >= '0' && da[j] <= '9')
                {
                    val = val * 10.0 + (double)(da[j] - '0');
                    j++; has_digit = 1;
                }
                if (j < da_len && da[j] == '.')
                {
                    j++;
                    while (j < da_len && da[j] >= '0' && da[j] <= '9')
                    {
                        frac_div *= 10.0;
                        frac += (double)(da[j] - '0') / frac_div;
                        j++; has_digit = 1;
                    }
                }
                if (has_digit)
                    *size_out = (neg ? -1.0 : 1.0) * (val + frac);
            }
        }
        return; /* primer /Nombre alcanza */
    }
}

pdf_obj *pdf_form_generate_text_appearance(pdf_stream *st, const pdf_xref_table *xref,
                                            pdf_arena *arena, pdf_obj *widget,
                                            const char *new_text, long new_obj_num)
{
    pdf_obj *da_obj, *rect_obj;
    char font_name[64];
    double font_size;
    double rx0 = 0.0, ry0 = 0.0, rx1 = 0.0, ry1 = 0.0, w, h;
    pdf_obj *resources;
    pdf_obj *bbox_arr, *ap_dict;
    char esc[600];
    char content[2048];
    long content_len;

    if (st == NULL || xref == NULL || arena == NULL || widget == NULL || new_text == NULL)
        return NULL;

    rect_obj = pdf_dict_get(widget, "Rect");
    if (rect_obj != NULL && rect_obj->type == PDF_ARRAY && rect_obj->u.arr.count == 4)
    {
        rx0 = pdf_obj_num(rect_obj->u.arr.items[0], 0.0);
        ry0 = pdf_obj_num(rect_obj->u.arr.items[1], 0.0);
        rx1 = pdf_obj_num(rect_obj->u.arr.items[2], 0.0);
        ry1 = pdf_obj_num(rect_obj->u.arr.items[3], 0.0);
    }
    w = rx1 - rx0; if (w < 1.0) w = 1.0;
    h = ry1 - ry0; if (h < 1.0) h = 1.0;

    font_name[0] = 0;
    font_size = 0.0;
    da_obj = field_find_inherited(st, xref, arena, widget, 0, 0, "DA", NULL);
    if (da_obj != NULL && da_obj->type == PDF_STRING)
        parse_da(da_obj->u.str.data, da_obj->u.str.len, font_name, sizeof(font_name), &font_size);
    if (font_name[0] == 0)
        copy_bounded(font_name, sizeof(font_name), "Helv", 4);
    if (font_size <= 0.0)
    {
        font_size = h * 0.65;
        if (font_size > 12.0) font_size = 12.0;
        if (font_size < 4.0)  font_size = 4.0;
    }

    /* /DR (Default Resources) de /Root/AcroForm -- de ahi sale el
     * dict de fuente real que /DA referencia por nombre (lo normal en
     * cualquier PDF con AcroForm bien formado). */
    resources = NULL;
    {
        pdf_obj *root = pdf_dict_get(xref->trailer, "Root");
        if (root != NULL && root->type == PDF_REF)
            root = pdf_xref_load_object(st, xref, root->u.ref.num, arena);
        if (root != NULL)
        {
            pdf_obj *acroform = pdf_dict_get(root, "AcroForm");
            if (acroform != NULL && acroform->type == PDF_REF)
                acroform = pdf_xref_load_object(st, xref, acroform->u.ref.num, arena);
            if (acroform != NULL)
            {
                pdf_obj *dr = pdf_dict_get(acroform, "DR");
                if (dr != NULL && dr->type == PDF_REF)
                    dr = pdf_xref_load_object(st, xref, dr->u.ref.num, arena);
                if (dr != NULL && dr->type == PDF_DICT)
                    resources = dr;
            }
        }
    }
    if (resources == NULL)
    {
        /* Sin /DR -- armar un /Resources minimo con /Helvetica bajo el
         * nombre que haya dado /DA. Si el nombre no coincide con nada
         * reconocible el renderer cae a su sustituto generico (no
         * crashea, mismo criterio tolerante del resto del motor). */
        pdf_obj *font_dict = pdf_obj_new_dict(arena);
        pdf_obj *font_entry = pdf_obj_new_dict(arena);
        pdf_dict_set(arena, font_entry, "Type", pdf_obj_new_name(arena, "Font"));
        pdf_dict_set(arena, font_entry, "Subtype", pdf_obj_new_name(arena, "Type1"));
        pdf_dict_set(arena, font_entry, "BaseFont", pdf_obj_new_name(arena, "Helvetica"));
        pdf_dict_set(arena, font_dict, font_name, font_entry);
        resources = pdf_obj_new_dict(arena);
        pdf_dict_set(arena, resources, "Font", font_dict);
    }

    {
        int ei = 0, i;
        for (i = 0; new_text[i] != 0 && ei < (int)sizeof(esc) - 2; i++)
        {
            unsigned char c = (unsigned char)new_text[i];
            if (c == '(' || c == ')' || c == '\\')
                esc[ei++] = '\\';
            esc[ei++] = (char)c;
        }
        esc[ei] = 0;
    }

    {
        double pad_w = (w > 2.0) ? w - 2.0 : w;
        double pad_h = (h > 2.0) ? h - 2.0 : h;
        double ty = (h - font_size) / 2.0;
        if (ty < 1.0) ty = 1.0;

        content_len = (long)sprintf(content,
            "/Tx BMC\nq\n1 1 %.2f %.2f re\nW n\nBT\n/%s %.2f Tf\n0 g\n2 %.2f Td\n(%s) Tj\nET\nQ\nEMC",
            pad_w, pad_h, font_name, font_size, ty, esc);
    }

    bbox_arr = pdf_obj_new_array(arena, 4);
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, 0.0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, 0.0));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, w));
    pdf_array_push(arena, bbox_arr, pdf_obj_new_real(arena, h));

    ap_dict = pdf_obj_new_dict(arena);
    pdf_dict_set(arena, ap_dict, "Type", pdf_obj_new_name(arena, "XObject"));
    pdf_dict_set(arena, ap_dict, "Subtype", pdf_obj_new_name(arena, "Form"));
    pdf_dict_set(arena, ap_dict, "BBox", bbox_arr);
    pdf_dict_set(arena, ap_dict, "Resources", resources);

    return pdf_obj_new_synthetic_stream(arena, ap_dict, (const unsigned char *)content,
                                         content_len, new_obj_num, 0);
}
