/* pdf_error.h
 *
 * Codigos de error comunes a todo PDFEngine32. Centralizados aca (en vez
 * de repetidos en cada modulo) para que agregar un codigo nuevo no
 * implique tocar N headers.
 */

#ifndef PDF_ERROR_H
#define PDF_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_OK              0
#define PDF_ERR_NOMEM       1  /* no hay presupuesto ni tras degradar/podar   */
#define PDF_ERR_BADARG      2  /* argumento invalido o sintaxis PDF invalida  */
#define PDF_ERR_OVERFLOW    3  /* overflow de tamanio (count * size, etc.)    */
#define PDF_ERR_NOTFOUND    4  /* objeto/clave/recurso no encontrado          */
#define PDF_ERR_IO          5  /* fallo de lectura de archivo                 */
#define PDF_ERR_UNSUPPORTED 6  /* feature reconocida pero no implementada aun */

#ifdef __cplusplus
}
#endif

#endif /* PDF_ERROR_H */
