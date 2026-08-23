/* pdf_display_list.h
 *
 * Display list persistente para PDFEngine32.
 *
 * El interprete PDF escribe operaciones en esta lista a traves de un
 * pdf_device. Los operandos se clonan en una arena propia, de modo que la
 * lista no depende de la arena temporal usada para decodificar el content
 * stream. La lista puede reproducirse posteriormente sobre cualquier
 * pdf_device compatible.
 */
#ifndef PDF_DISPLAY_LIST_H
#define PDF_DISPLAY_LIST_H

#include "pdf_device.h"
#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_display_list_record_s pdf_display_list_record;

typedef struct pdf_display_list_s
{
    pdf_arena arena;
    pdf_display_list_record *first;
    pdf_display_list_record *last;
    unsigned long count;
    int initialized;
    int error;
    pdf_device device;
} pdf_display_list;

/* Inicializa una display list con una arena independiente. */
int pdf_display_list_init(pdf_display_list *list, pdf_ledger *ledger,
                          size_t block_size);

/* Borra todos los registros pero conserva la arena lista para reutilizarse. */
void pdf_display_list_reset(pdf_display_list *list);

/* Destruye la lista y libera su arena. */
void pdf_display_list_destroy(pdf_display_list *list);

/* Device al que debe entregarse el content stream para grabarlo. */
pdf_device *pdf_display_list_get_device(pdf_display_list *list);

/* Reproduce todos los registros sobre el device indicado. */
int pdf_display_list_run(const pdf_display_list *list, pdf_device *device);

/* Numero de operaciones grabadas, incluyendo imagenes inline. */
unsigned long pdf_display_list_count(const pdf_display_list *list);

/* Devuelve el primer error de grabacion, o PDF_OK. */
int pdf_display_list_error(const pdf_display_list *list);

#ifdef __cplusplus
}
#endif

#endif /* PDF_DISPLAY_LIST_H */
