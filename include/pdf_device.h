/* pdf_device.h
 *
 * Interfaz generica de salida para el interprete de content streams.
 * El interprete conoce PDF; el device decide que hacer con cada operador.
 *
 * Primera capa de la arquitectura PDFEngine32 inspirada en el modelo
 * Document -> Page -> Interpreter -> Device. No contiene codigo de
 * rasterizacion y no depende de Win32.
 */
#ifndef PDF_DEVICE_H
#define PDF_DEVICE_H

#include "pdf_object.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pdf_device_op_fn)(void *user, const char *opname,
                                  pdf_obj **args, int nargs);

typedef void (*pdf_device_inline_image_fn)(void *user, pdf_obj *dict_obj,
                                            const unsigned char *data, long len);

typedef struct pdf_device_ops_s
{
    pdf_device_op_fn op;
    pdf_device_inline_image_fn inline_image;
} pdf_device_ops;

typedef struct pdf_device_s
{
    const pdf_device_ops *ops;
    void *user;
} pdf_device;

void pdf_device_init(pdf_device *dev, const pdf_device_ops *ops, void *user);
void pdf_device_reset(pdf_device *dev);
void pdf_device_emit(pdf_device *dev, const char *opname,
                     pdf_obj **args, int nargs);
void pdf_device_emit_inline_image(pdf_device *dev, pdf_obj *dict_obj,
                                  const unsigned char *data, long len);

#ifdef __cplusplus
}
#endif

#endif /* PDF_DEVICE_H */
