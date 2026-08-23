/* pdf_jpx.h
 *
 * Decodificador JPEG2000 (JPXDecode) minimo -- ver DESIGN.md seccion
 * 60 para el contexto completo. Implementa lo necesario para el caso
 * REAL encontrado en PDFs de uso comun (confirmado inspeccionando el
 * codestream de Conveyor_Handbook.pdf a mano): contenedor JP2 (boxes)
 * o codestream J2K crudo, un tile o varios, transformada wavelet 9/7
 * irreversible O 5/3 reversible, MCT (ICT/RCT), UNA capa de calidad,
 * code-blocks sin estilos especiales (sin BYPASS/RESET/TERMALL/etc.).
 *
 * NO soportado (fuera de alcance, se detecta y devuelve
 * PDF_ERR_UNSUPPORTED en vez de fallar silenciosamente o crashear):
 * multiples capas de calidad (progressive-by-quality), Region of
 * Interest (RGN), estilos de code-block especiales, componentes con
 * submuestreo de croma (xr/yr != 1), mas de 4 componentes.
 */

#ifndef PDF_JPX_H
#define PDF_JPX_H

#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_jpx_image_s
{
    int            width, height;
    unsigned char *rgb;  /* width*height*3, alocado en la arena pasada al filtro */
} pdf_jpx_image;

/* JPXDecode: decodifica 'src'/'src_len' (el box jp2c completo, O un
 * codestream J2K crudo empezando directo en SOC 0xFF4F -- ambos casos
 * se detectan automaticamente) hacia 'out', alocado en 'arena'.
 * Devuelve PDF_OK o error. */
int pdf_filter_jpx(pdf_arena *arena, const unsigned char *src, long src_len,
                    pdf_jpx_image *out);

#ifdef __cplusplus
}
#endif

#endif /* PDF_JPX_H */
