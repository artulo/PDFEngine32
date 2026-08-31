/* pdf_jbig2.h
 *
 * Decodificador JBIG2 (JBIG2Decode) minimo -- ver DESIGN.md seccion
 * 91-92 para el contexto completo. Implementa lo que se confirmo
 * necesario contra un archivo real (tests/PRINCIPLES_OF_MINERAL_
 * PROCESSINGFuerstan.pdf, un libro escaneado de 586 paginas donde el
 * 83% salia en blanco por este filtro faltante): el subconjunto
 * "generic region" -- una pagina entera (o una region de ella)
 * codificada DIRECTO con el codificador aritmetico MQ (el mismo
 * algoritmo, tabla e implementacion que ya usa pdf_jpx.c para
 * JPEG2000 -- JBIG2 Annex E y JPEG2000 Annex C son el MISMO
 * codificador, norma publica) y una plantilla de contexto de pixeles
 * vecinos ya decodificados (GBTEMPLATE 0-3, con posiciones
 * adaptativas AT).
 *
 * Metodologia (misma que memoria [[feedback_jpx_debugging_method]] --
 * "conseguir una referencia real, no solo releer el spec"): las
 * plantillas de contexto (CodingTemplates/ReusedContexts) y el orden
 * exacto de armado del entero de contexto se tomaron de pdf.js
 * (jbig2.js de Mozilla, MPL 2.0, implementacion publica ampliamente
 * usada) y se verificaron -- ANTES de escribir una sola linea de C --
 * con un prototipo en Python contra la imagen real decodificada por
 * PyMuPDF/MuPDF para el archivo de arriba: 0 diferencias en los
 * 7 779 968 pixeles de la pagina completa.
 *
 * NO soportado (fuera de alcance esta ronda, se detecta y devuelve
 * PDF_ERR_UNSUPPORTED en vez de fallar silenciosamente o crashear):
 * diccionarios de simbolos, regiones de texto, regiones de
 * refinamiento generico, diccionarios de patrones/regiones de
 * halftone, codificacion MMR (K<0 estilo CCITT) para regiones
 * genericas, tablas de codigo a medida, extensiones. Estos son los
 * segmentos que un generador JBIG2 mas sofisticado (con deteccion de
 * simbolos de texto repetidos) usaria para comprimir mejor -- el caso
 * "toda la pagina como una sola region generica" (SIN diccionario de
 * simbolos) es mas simple y comun en muchos pipelines de escaneo. Si
 * un segmento no soportado aparece, se devuelve PDF_ERR_UNSUPPORTED
 * limpio (el llamador ya degrada con tolerancia, ver pdf_image.c). */

#ifndef PDF_JBIG2_H
#define PDF_JBIG2_H

#include "pdf_mem.h"
#include "pdf_filter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decodifica el stream JBIG2 embebido de PDF (formato "sin encabezado
 * de archivo", una secuencia de segmentos, ver Annex D de T.88) hacia
 * bits empaquetados 1bpp (MSB primero, filas alineadas a byte, bit=1
 * significa "primer plano/negro" -- convencion FIJA de JBIG2, a
 * diferencia de CCITT no hay una bandera equivalente a /BlackIs1)
 * -- mismo formato de salida que pdf_filter_ccitt_g4, para que
 * pdf_image.c lo desempaquete con la MISMA logica generica que ya usa
 * para CCITT/Flate/crudo, sin caminos nuevos.
 *
 * 'globals'/'globals_len' (puede ser NULL/0): el stream de
 * /JBIG2Globals si el diccionario de imagen lo declara -- sus
 * segmentos se procesan ANTES que los de 'src', como si estuvieran
 * concatenados (asi lo exige la norma, pensado para compartir
 * diccionarios de simbolos entre paginas -- aunque esta implementacion
 * no soporta diccionarios de simbolos, procesar 'globals' primero
 * sigue siendo necesario para no perder informacion de PAGINA si
 * alguna vez viniera ahi).
 *
 * 'width'/'height': tamanio declarado por /Width y /Height del
 * diccionario de imagen del PDF -- se usa para dimensionar el bitmap
 * de pagina si el segmento de informacion de pagina (tipo 48) no
 * aparece o declara un tamanio distinto (no deberia pasar en un
 * archivo bien formado, pero mejor no confiar ciegamente). */
int pdf_filter_jbig2(pdf_arena *arena,
                      const unsigned char *src, long src_len,
                      const unsigned char *globals, long globals_len,
                      int width, int height, pdf_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* PDF_JBIG2_H */
