/* pdf_image.h
 *
 * Decodificacion y composicion de imagenes PDF (XObjects /Subtype
 * /Image, referenciadas por el operador 'Do').
 *
 * Soporta:
 *   - ColorSpace: DeviceGray, DeviceRGB, DeviceCMYK, Indexed (con base
 *     DeviceRGB o DeviceGray).
 *   - BitsPerComponent: 1, 4 y 8 (2 y 16 no soportados -- muy raros en la practica).
 *   - Filtros: sin filtro (samples crudos), FlateDecode, DCTDecode
 *     (JPEG baseline), CCITTFaxDecode (Group 4).
 *   - ImageMask (stencil de 1 bit, pinta con el color de relleno vigente).
 *
 * NO soportado: Decode arrays custom (se asume el default para cada
 * color space), JPXDecode (JPEG2000), color spaces
 * ICCBased/Separation/DeviceN (se tratan como su /N componentes vía
 * la misma heuristica gris/RGB/CMYK que sc/scn), Form XObjects
 * (/Subtype /Form -- son content streams anidados con su propio
 * /Resources, fuera de alcance de este modulo).
 *
 * SMask (transparencia real por-pixel, ver DESIGN.md seccion 59): SI
 * soportado -- se decodifica como un canal alpha aparte y se compone
 * con blending real contra el fondo, en vez de pintar opaco.
 *
 * La composicion usa muestreo por transformacion inversa (para cada
 * pixel de destino, se calcula que pixel de origen le corresponde via
 * la inversa de la matriz CTM+flip-Y) -- a diferencia del resto del
 * motor, esto SI soporta rotacion/shear del CTM correctamente, porque
 * es casi gratis con este enfoque.
 */

#ifndef PDF_IMAGE_H
#define PDF_IMAGE_H

#include "pdf_object.h"
#include "pdf_mem.h"
#include "pdf_bitmap.h"
#include "pdf_types.h"
#include "pdf_stream.h"
#include "pdf_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_image_s
{
    int             width, height;
    int             is_mask;       /* 1 = ImageMask (stencil de 1 bit) */
    unsigned char  *rgb;           /* width*height*3 (RGB24), NULL si is_mask */
    unsigned char  *mask_bits;     /* width*height bytes, 1=pintar/0=transparente, valido solo si is_mask */
    /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 59):
     * antes NO se leia /SMask en absoluto -- una imagen con mascara de
     * transparencia por-pixel (comun: sombras suaves, logos con bordes
     * antialiasados, fotos recortadas) se dibujaba 100% opaca, tapando
     * lo que hubiera debajo con su fondo real (a veces negro/blanco
     * solido en vez del contenido real de la imagen). 'alpha' es
     * width*height bytes (0=transparente, 255=opaco), ya remuestreado
     * al tamanio de 'rgb' si el /SMask tenia dimensiones distintas
     * (el estandar lo permite) -- NULL si la imagen no tiene /SMask
     * (el caso normal, sin cambio de comportamiento). */
    unsigned char  *alpha;
} pdf_image;

/* Decodifica el XObject de imagen 'img_dict' (dict ya resuelto, con
 * /Width /Height /BitsPerComponent /ColorSpace /Filter, y el stream
 * crudo accesible via 'raw_offset'/'raw_length' que trae el propio
 * img_dict si es PDF_STREAM). Escribe el resultado en 'arena'
 * (tipicamente page->decode_arena). Devuelve PDF_OK o error. */
int pdf_image_decode(pdf_stream *st, const pdf_xref_table *xref, pdf_obj *img_dict, pdf_arena *arena,
                      pdf_image *out);

/* Compone 'img' sobre 'bmp', mapeando el cuadrado unitario [0,1]x[0,1]
 * de espacio de imagen a traves de 'unit_to_pixel' (la matriz completa
 * que ya incluye CTM + flip de eje Y -- ver pdf_render.c). Si img->is_mask,
 * pinta con 'fill_color' donde corresponda; si no, usa los pixeles RGB
 * de la imagen directamente. */
void pdf_image_draw(pdf_bitmap *bmp, const pdf_image *img,
                     pdf_matrix unit_to_pixel, pdf_color fill_color);

#ifdef __cplusplus
}
#endif

#endif /* PDF_IMAGE_H */
