/* pdf_content.h
 *
 * Interprete de content streams (la secuencia de operadores PDF: "cm",
 * "re", "f", "Tj", "Do", etc.) ya decodificados en memoria (el filtro
 * FlateDecode/DCTDecode/etc. ya corrio -- este modulo NO toca el
 * archivo ni conoce streams comprimidos).
 *
 * Diseño deliberado: pdf_content.c SOLO tokeniza y arma la pila de
 * operandos; no sabe dibujar nada. Cada operador reconocido se entrega
 * via un callback generico a quien llama (graphics/, text/, image/ o,
 * por ahora, el device de diagnostico en examples/). Esto separa "leer
 * el lenguaje de una pagina PDF" de "que hacer con cada instruccion",
 * que son responsabilidades distintas y van a evolucionar a ritmos
 * distintos (el parser de operadores casi no cambia; el render si).
 */

#ifndef PDF_CONTENT_H
#define PDF_CONTENT_H

#include "pdf_object.h"
#include "pdf_mem.h"
#include "pdf_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximo de operandos acumulados antes de un operador. El operador con
 * mas operandos del PDF estandar es "d0"/"d1" (2) o el color con
 * componentes CMYK+patron (hasta 5); 16 sobra con margen. Si aparece un
 * operador con mas, los operandos mas viejos se descartan (no crashea). */
#define PDF_CONTENT_MAX_ARGS 16

/* Callback invocado por cada operador reconocido. 'args'/'nargs' son
 * los operandos apilados desde el ultimo operador (en orden de
 * aparicion). 'opname' es el texto exacto del operador ("Tj","cm",...).
 * El callback NO debe quedarse con punteros a 'args' mas alla de la
 * llamada: la arena de operandos se recicla despues de cada operador. */
typedef pdf_device_op_fn pdf_content_op_fn;
typedef pdf_device_inline_image_fn pdf_content_inline_image_fn;

typedef struct pdf_content_ops_s
{
    pdf_content_op_fn            op;             /* obligatorio */
    pdf_content_inline_image_fn  inline_image;   /* puede ser NULL */
    void                         *user;
} pdf_content_ops;

/* Ejecuta el interprete sobre 'data'[0..len). 'arena' es donde se alocan
 * los pdf_obj operando (tipicamente page->decode_arena o una arena chica
 * dedicada que el llamador resetea entre paginas -- ver DESIGN.md 2.1).
 * Devuelve PDF_OK, o PDF_ERR_BADARG si la sintaxis es irrecuperable. Los
 * errores de operadores individuales NO abortan la interpretacion (se
 * ignoran, tolerante, como hacen todos los visores PDF de produccion). */
int pdf_content_run_device(const unsigned char *data, long len,
                            pdf_arena *arena, pdf_device *device);

/* API de compatibilidad heredada. Internamente construye un pdf_device
 * adaptador y utiliza exactamente el mismo interprete que
 * pdf_content_run_device(). */
int pdf_content_run(const unsigned char *data, long len,
                    pdf_arena *arena, const pdf_content_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* PDF_CONTENT_H */
