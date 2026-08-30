/* pdf_bitmap.h
 *
 * Buffer de pixeles RGB24 + alpha interno de destino para el render. Vive en una arena
 * (tipicamente page_arena, ya que el bitmap de una pagina es del mismo
 * orden de vida que la pagina misma).
 *
 * Este es un rasterizador MINIMO a proposito: solo rellenos de
 * rectangulos alineados a los ejes. Curvas (operador 'c'/'v'/'y'),
 * trazos con grosor variable, antialiasing y clipping no lineal quedan
 * fuera de este esqueleto -- son trabajo de graphics/ a futuro.
 */

#ifndef PDF_BITMAP_H
#define PDF_BITMAP_H

#include "pdf_mem.h"
#include "pdf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_bitmap_s
{
    int             width;
    int             height;
    int             stride;   /* bytes por fila, = width * 3 (RGB24) */
    unsigned char  *pixels;   /* width*height*3 bytes, en 'arena'      */
    unsigned char  *alpha;    /* width*height bytes; 255=opaco */

    /* Clip rectangular activo (pixel space, [clip_x0,clip_x1) x
     * [clip_y0,clip_y1), semi-abierto). Se aplica automaticamente en
     * pdf_bitmap_set_pixel -- centraliza el clipping en un solo lugar
     * en vez de tener que tocar fill/stroke/texto/imagenes por
     * separado. Solo soporta rectangulos (la forma real del path de
     * clip se aproxima por su bounding box) -- suficiente para el caso
     * comun (recortar a un marco/región rectangular), no pixel-perfect
     * para paths de clip no rectangulares. Default: sin clip (todo el
     * bitmap), ver pdf_bitmap_create. */
    int             clip_x0, clip_y0, clip_x1, clip_y1;

    /* Mascara de clipping por pixel. NULL = solo clipping rectangular. */
    unsigned char  *clip_mask;
    int             clip_mask_stride;

    /* Mascara de transparencia por objeto (soft mask). 0=transparente,
     * 255=opaco. Se combina con clip_mask. */
    unsigned char  *soft_mask;
    int             soft_mask_stride;
    double          opacity; /* alpha de composicion source-over, 0..1 */
    int             blend_mode; /* PDF_BLEND_* */

    /* Knockout state for transparency groups. When enabled, a new paint
     * operation removes the previous group contribution at the covered
     * pixels, while preserving the backdrop of a non-isolated group. */
    int             knockout;
    unsigned char  *knockout_mask;
    int             knockout_mask_stride;
    unsigned char  *knockout_backdrop_pixels;
    unsigned char  *knockout_backdrop_alpha;
    int             knockout_backdrop_stride;
} pdf_bitmap;

/* Crea un bitmap RGB24 de width x height, inicializado en blanco
 * (255,255,255), alocado en 'arena'. Devuelve PDF_OK o error. */
int pdf_bitmap_create(pdf_arena *arena, int width, int height, pdf_bitmap *bmp);

/* Crea un bitmap RGBA transparente. Se usa para transparency groups. */
int pdf_bitmap_create_transparent(pdf_arena *arena, int width, int height, pdf_bitmap *bmp);

/* Compone un bitmap fuente RGBA sobre el destino. */
void pdf_bitmap_composite(pdf_bitmap *dst, const pdf_bitmap *src, double opacity, int blend_mode);

/* Copia completamente la superficie fuente al destino. Se usa para
 * iniciar un transparency group no aislado con su backdrop. */
void pdf_bitmap_copy(pdf_bitmap *dst, const pdf_bitmap *src);

/* Actualiza el clip rectangular activo (ver comentario en el struct).
 * Los valores se clampean contra los bordes reales del bitmap. */
void pdf_bitmap_set_clip(pdf_bitmap *bmp, int x0, int y0, int x1, int y1);

/* Instala una mascara de clipping por pixel. La mascara pertenece al
 * mismo arena que el bitmap y usa 0=fuera, 255=dentro. */
void pdf_bitmap_set_clip_mask(pdf_bitmap *bmp, unsigned char *mask, int stride);

/* Instala una soft mask de transparencia por pixel. */
void pdf_bitmap_set_soft_mask(pdf_bitmap *bmp, unsigned char *mask, int stride);

/* Define la opacidad de pintura del bitmap. 1.0 = opaco, 0.0 = transparente. */
void pdf_bitmap_set_opacity(pdf_bitmap *bmp, double opacity);

/* PDF blend modes soportados por el compositor RGB. */
#define PDF_BLEND_NORMAL   0
#define PDF_BLEND_MULTIPLY 1
#define PDF_BLEND_SCREEN   2
#define PDF_BLEND_OVERLAY  3
#define PDF_BLEND_DARKEN   4
#define PDF_BLEND_LIGHTEN  5
#define PDF_BLEND_COLOR_DODGE 6
#define PDF_BLEND_COLOR_BURN  7
#define PDF_BLEND_HARD_LIGHT  8
#define PDF_BLEND_SOFT_LIGHT  9
#define PDF_BLEND_DIFFERENCE 10
#define PDF_BLEND_EXCLUSION  11
#define PDF_BLEND_HUE        12
#define PDF_BLEND_SATURATION 13
#define PDF_BLEND_COLOR      14
#define PDF_BLEND_LUMINOSITY 15

void pdf_bitmap_set_blend_mode(pdf_bitmap *bmp, int mode);

/* Configura knockout por pixel para un transparency group. Si backdrop es
 * NULL, se considera un backdrop transparente (grupo aislado). */
int pdf_bitmap_enable_knockout(pdf_bitmap *bmp, pdf_arena *arena,
                               const pdf_bitmap *backdrop);
void pdf_bitmap_set_knockout(pdf_bitmap *bmp, int enabled);

/* Pone un pixel (sin chequeo de bounds en release -- el llamador debe
 * clampear antes; en debug se puede agregar assert). Silenciosamente
 * no hace nada si x/y estan fuera de rango (defensivo) o fuera del
 * clip rectangular activo. */
void pdf_bitmap_set_pixel(pdf_bitmap *bmp, int x, int y, pdf_color c);

/* Igual que pdf_bitmap_set_pixel(), pero con cobertura alfa adicional
 * (0..1). Se usa para máscaras de glyphs antialiased. */
void pdf_bitmap_set_pixel_coverage(pdf_bitmap *bmp, int x, int y,
                                   pdf_color c, double coverage);

/* BUG REAL DE RENDIMIENTO (Arturo: "comparando con Acrobat/MuPDF el
 * nuestro es extremadamente lento", 3240-3241-2.pdf): pdf_image_draw
 * dibuja imagenes ya decodificadas como bytes RGB de 0-255, pero
 * pdf_bitmap_set_pixel() solo acepta un pdf_color de doubles en
 * [0,1] -- forzaba a CADA pixel de imagen (hasta 2.5 millones por
 * "Do") a hacer 3 divisiones por 255.0 para entrar, que
 * pdf_bitmap_set_pixel_coverage() luego multiplicaba por 255.0 de
 * nuevo para volver a bytes (clamp_u8), un ida-y-vuelta sin sentido
 * para datos que ya eran bytes. Esta variante hace exactamente el
 * mismo camino/chequeos que pdf_bitmap_set_pixel_coverage (bounds,
 * clip, opacity, clip_mask, soft_mask) pero opera en bytes: si el
 * resultado da opaco/Normal/sin-knockout (el caso comun, sin
 * mascaras activas) escribe los bytes tal cual sin ninguna
 * conversion; si no, cae al camino general convirtiendo a
 * pdf_color recien ahi (caso raro: imagen con soft mask o dentro de
 * un grupo con blend mode no-Normal). */
void pdf_bitmap_set_pixel_rgb_u8(pdf_bitmap *bmp, int x, int y,
                                 unsigned char r, unsigned char g, unsigned char b);

/* Rellena el rectangulo [x0,y0]-[x1,y1] (en coordenadas de pixel, ya
 * convertidas desde espacio PDF por el llamador) con el color dado.
 * Recorta contra los bordes del bitmap. */
void pdf_bitmap_fill_rect(pdf_bitmap *bmp, int x0, int y0, int x1, int y1, pdf_color c);

/* Escribe el bitmap como PPM (P6) crudo -- util para verificar
 * visualmente el resultado sin depender de Windows/GDI durante el
 * desarrollo multiplataforma de este esqueleto. El backend real para
 * FiveWin (HDC/HBITMAP) va en win32/. */
int pdf_bitmap_write_ppm(const pdf_bitmap *bmp, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PDF_BITMAP_H */
