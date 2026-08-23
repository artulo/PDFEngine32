/* pdf_raster.h
 *
 * Rasteriza un pdf_path sobre un pdf_bitmap: trazo (lineas via Bresenham,
 * SIN grosor variable -- limitacion conocida, ver DESIGN.md) y relleno
 * (scanline con regla nonzero o evenodd, soporta multiples subpaths a
 * la vez para paths con "agujeros").
 */

#ifndef PDF_RASTER_H
#define PDF_RASTER_H

#include "pdf_bitmap.h"
#include "pdf_path.h"
#include "pdf_shading.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_fill_rule_e
{
    PDF_FILL_NONZERO,
    PDF_FILL_EVENODD
} pdf_fill_rule;

/* Traza los segmentos de todos los subpaths (cierra automaticamente los
 * marcados como closed). No soporta grosor de linea variable: siempre
 * dibuja 1 pixel de ancho, sin importar el 'w' vigente en el content
 * stream. Atajo de pdf_raster_stroke_path_w(bmp,path,c,1.0). */
void pdf_raster_stroke_path(pdf_bitmap *bmp, const pdf_path *path, pdf_color c);

/* Igual que pdf_raster_stroke_path, pero con grosor real (en pixeles,
 * ya escalado por el llamador segun el CTM vigente -- ver ctm_scale()
 * en pdf_render.c). Cada segmento se dibuja como un rectangulo relleno
 * (scanline fill) perpendicular a su direccion, con un cuadrado en
 * cada vertice para evitar huecos en las uniones (aproximacion simple
 * de join/cap, no distingue miter/round/bevel ni cap styles). Si
 * width_px <= 1, usa el trazo Bresenham de 1px (mas rapido, resultado
 * identico para el caso mas comun de lineas finas). */
void pdf_raster_stroke_path_w(pdf_bitmap *bmp, const pdf_path *path, pdf_color c, double width_px);

/* Trazo PDF completo: grosor, cap (0 butt/1 round/2 square),
 * join (0 miter/1 round/2 bevel), limite de miter y patron dash. */
void pdf_raster_stroke_path_style(pdf_bitmap *bmp, const pdf_path *path,
                                  pdf_color c, double width_px,
                                  int line_cap, int line_join,
                                  double miter_limit,
                                  const double *dash, int dash_count,
                                  double dash_phase);

/* Rellena todos los subpaths juntos segun 'rule'. Los subpaths NO
 * cerrados se cierran implicitamente para el relleno (asi lo exige el
 * estandar PDF: un path abierto igual se rellena como si estuviera
 * cerrado, aunque el trazo del contorno no dibuje el segmento de cierre
 * salvo que se haya usado 's'/'b' en vez de 'f'/'B'). */
void pdf_raster_fill_path(pdf_bitmap *bmp, const pdf_path *path,
                           pdf_color c, pdf_fill_rule rule);

/* Igual que pdf_raster_fill_path, pero con ANTIALIASING (supersampling
 * vertical + cobertura horizontal exacta por pixel, via
 * pdf_bitmap_set_pixel_coverage -- ver DESIGN.md, ronda "render de
 * fuentes real"). Funcion NUEVA y separada de pdf_raster_fill_path a
 * proposito: esta ultima tiene manejo especial ya afinado (con bugs
 * reales encontrados y corregidos) para formas mas angostas que 1px
 * (lineas finas de tablas en planos tecnicos) que NO hace falta ni
 * conviene tocar para el caso que motiva esta version -- texto
 * (glyphs), donde no hay formas sub-pixel que proteger y SI importa
 * mucho la calidad visual a tamanio chico (sin AA, texto pequenio se
 * ve tosco/con bordes dentados comparado con un visor PDF real). */
void pdf_raster_fill_path_aa(pdf_bitmap *bmp, const pdf_path *path,
                              pdf_color c, pdf_fill_rule rule);

/* Rasteriza 'path' a una mascara de clip de 8 bits (0=fuera, 255=
 * dentro, stride == bmp_width) para instalar via
 * pdf_bitmap_set_clip_mask -- ver DESIGN.md seccion 68 (fase de
 * transparencia/shadings). 'out_mask' debe tener bmp_width*bmp_height
 * bytes, alocados por el llamador (tipicamente en dev->arena, con
 * vida igual a la pagina).
 *
 * Devuelve 1 si escribio una mascara real (path no es un rectangulo
 * simple alineado a ejes) o 0 si detecto que 'path' ES un rectangulo
 * alineado a ejes (el caso 're W n', la inmensa mayoria de los clips
 * reales) -- en ese caso NO toca 'out_mask' y el llamador debe seguir
 * usando el clip rectangular barato existente (bbox), evitando pagar
 * el costo de una mascara de pagina completa para el caso comun. */
int pdf_raster_rasterize_clip_mask(const pdf_path *path, pdf_fill_rule rule,
                                    int bmp_width, int bmp_height,
                                    unsigned char *out_mask);

/* Mismo chequeo "es un rectangulo simple alineado a ejes" que usa
 * pdf_raster_rasterize_clip_mask internamente, pero expuesto para que
 * el LLAMADOR (finish_path en pdf_render.c) pueda decidir ANTES de
 * pedir los bmp_width*bmp_height bytes de out_mask -- si no, el
 * "ahorro" documentado arriba nunca se cobra de verdad: el buffer de
 * pagina completa ya se alloco (y quedo permanentemente pegado en la
 * arena) para cuando esta funcion recien se entera de que no hacia
 * falta. Bug real encontrado con un mapa aeronautico denso
 * (tests/mupdf_bug.cgiid=701945-slow.rendering.pdf) que hace 're W n'
 * miles de veces -- cada uno tiraba una mascara de pagina entera a la
 * basura, agotando el presupuesto de memoria antes de terminar de
 * dibujar el mapa. */
int pdf_raster_path_is_rect(const pdf_path *path);

/* Igual que pdf_raster_fill_path_aa, pero el color de cada pixel lo
 * decide 'color_fn' (patrones de shading, ver DESIGN.md seccion 68 --
 * PatternType 2 via scn/SCN) en vez de un pdf_color fijo. Si
 * 'color_fn' devuelve 0 para un pixel, ese pixel queda SIN pintar (no
 * se pinta negro/vacio) -- mismo criterio "no pintar" que
 * pdf_shading_eval. */
void pdf_raster_fill_path_shaded(pdf_bitmap *bmp, const pdf_path *path,
                                 pdf_fill_rule rule,
                                 pdf_shading_pixel_fn color_fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* PDF_RASTER_H */
