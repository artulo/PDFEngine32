/* pdf_types.h
 *
 * Tipos y constantes compartidas por varios modulos, para no crear
 * dependencias cruzadas entre headers que no la necesitan. Los tipos
 * "grandes" (pdf_obj, pdf_document, pdf_page, etc.) se definen en sus
 * propios headers -- aca solo van cosas chicas y transversales.
 */

#ifndef PDF_TYPES_H
#define PDF_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Punto 2D en espacio de usuario (unidades PDF, 1/72 pulgada). Usado por
 * el interprete de content stream y, mas adelante, por graphics/. */
typedef struct pdf_point_s
{
    double x, y;
} pdf_point;

/* Matriz de transformacion 2x3 (como el operador PDF "cm"):
 *   [ a b 0 ]
 *   [ c d 0 ]
 *   [ e f 1 ]
 */
typedef struct pdf_matrix_s
{
    double a, b, c, d, e, f;
} pdf_matrix;

/* Rectangulo (como /MediaBox, /CropBox, etc.) */
typedef struct pdf_rect_s
{
    double x0, y0, x1, y1;
} pdf_rect;

/* Color RGB en [0,1], resultado ya normalizado sin importar el color
 * space de origen (DeviceGray/DeviceRGB/DeviceCMYK/ICC...) -- la
 * conversion de color space vive en graphics/ (no implementada aun). */
typedef struct pdf_color_s
{
    double r, g, b;
} pdf_color;

#define PDF_MATRIX_IDENTITY_INIT { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 }

#ifdef __cplusplus
}
#endif

#endif /* PDF_TYPES_H */
