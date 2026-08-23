/* pdf_form.h
 *
 * Parseo de campos de formulario AcroForm (anotaciones /Widget de
 * /Annots) -- primer paso de la fase "AcroForm" del roadmap de
 * potencialidad MuPDF (ver DESIGN.md). Alcance de esta etapa: SOLO
 * campos de texto (/FT /Tx) y checkbox (/FT /Btn sin el bit "radio"
 * de /Ff) se listan como editables; radio buttons, combo/listbox y
 * firmas digitales se reportan (tipo PDF_FORM_FIELD_OTHER) pero no son
 * clickeables/editables en la UI todavia.
 *
 * Mismo patron que pdf_text_extract.h: el llamador provee el arreglo
 * de salida (arena_alloc, NO una variable local -- con
 * PDF_FORM_MAX_FIELDS*sizeof(pdf_form_field) puede pesar varios KB, no
 * conviene en el stack), esta funcion lo llena hasta 'max_fields' sin
 * crashear si hay mas.
 */

#ifndef PDF_FORM_H
#define PDF_FORM_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_parser.h"
#include "pdf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_FORM_MAX_FIELDS      256
#define PDF_FORM_VALUE_MAX       256
#define PDF_FORM_NAME_MAX        128
#define PDF_FORM_STATE_NAME_MAX  64

typedef enum pdf_form_field_type_e
{
    PDF_FORM_FIELD_TEXT = 1,
    PDF_FORM_FIELD_CHECKBOX,
    PDF_FORM_FIELD_OTHER  /* radio/combo/list/firma -- listado, no editable aun */
} pdf_form_field_type;

typedef struct pdf_form_field_s
{
    pdf_form_field_type type;
    pdf_rect  rect;       /* /Rect del widget, espacio de pagina (puntos PDF) */
    char      value[PDF_FORM_VALUE_MAX];      /* texto: bytes crudos del string PDF
                                                * (ASCII/Latin1 practico -- si viene
                                                * con BOM UTF-16BE se copia tal cual,
                                                * sin transliterar, ver pdf_form.h);
                                                * checkbox: nombre de estado actual
                                                * ("Off" o el nombre "on") */
    char      on_state_name[PDF_FORM_STATE_NAME_MAX]; /* checkbox: nombre del estado
                                                * "on" en /AP/N (NO siempre "Yes"),
                                                * vacio si no se pudo resolver */
    char      name[PDF_FORM_NAME_MAX];  /* /T (nombre parcial), solo para diagnostico */
    int       read_only;  /* /Ff bit 1 (ReadOnly) */
    long      obj_num, obj_gen; /* objeto a reescribir al guardar -- el propio
                                 * widget, o su /Parent si /V vive ahi */
    pdf_obj  *widget_obj; /* dict del widget ya resuelto, para que el render
                           * pueda leer /AP sin volver a buscarlo */
} pdf_form_field;

/* Lista los campos Widget de 'page_obj' (ya resuelto, ver
 * pdf_document_get_page) hacia 'out' (capacidad 'max_fields',
 * provista por el llamador). Recorre /Annots filtrando /Subtype
 * /Widget; resuelve /FT, /V, /DA subiendo la cadena /Parent cuando el
 * widget no los tiene directo (atributos heredables de campo, mismo
 * espiritu que pdf_page_get_inherited pero sobre /Parent en vez del
 * arbol /Pages). Devuelve la cantidad de campos escritos en 'out'
 * (0..max_fields, nunca escribe fuera de rango aunque la pagina tenga
 * mas -- los de mas se descartan sin crashear, mismo criterio que
 * pdf_text_search). 0 si la pagina no tiene /Annots o no tiene
 * ningun Widget (no es un error). */
int pdf_form_list_fields(pdf_stream *st, const pdf_xref_table *xref,
                          pdf_arena *arena, pdf_obj *page_obj,
                          pdf_form_field *out, int max_fields);

/* Genera una apariencia (/AP/N) minima para un campo de texto con
 * 'new_text' como valor nuevo -- una sola linea, alineada a la
 * izquierda, recortada a un /BBox del tamanio de /Rect, con la
 * fuente/tamanio de /DA (heredado si hace falta) o un default
 * razonable si no se puede leer. Usa /Root/AcroForm/DR para resolver
 * el nombre de fuente de /DA a un dict de fuente real; si no hay /DR,
 * arma un /Resources minimo con Helvetica bajo ese nombre.
 *
 * Devuelve un stream SINTETICO (ver pdf_obj_new_synthetic_stream,
 * pdf_object.h) con 'new_obj_num' como numero de objeto -- el
 * llamador es responsable de haber elegido un numero de objeto libre
 * (no usado por ningun objeto existente del documento) y de agregarlo
 * a la lista de objetos a guardar (ver pdf_write.h). NULL si
 * 'widget'/'new_text' son invalidos. */
pdf_obj *pdf_form_generate_text_appearance(pdf_stream *st, const pdf_xref_table *xref,
                                            pdf_arena *arena, pdf_obj *widget,
                                            const char *new_text, long new_obj_num);

#ifdef __cplusplus
}
#endif

#endif /* PDF_FORM_H */
