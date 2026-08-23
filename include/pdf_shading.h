/* pdf_shading.h
 *
 * Shadings (ISO 32000-1 8.7.4.5): gradientes evaluados analiticamente
 * pixel a pixel (no un bitmap pre-renderizado). Soportado: Type 2
 * (axial/lineal) y Type 3 (radial). Mesh shadings (Type 4-7) y
 * function-based (Type 1) quedan fuera de alcance -- ver
 * pdf_shading_load.
 */

#ifndef PDF_SHADING_H
#define PDF_SHADING_H

#include "pdf_function.h"
#include "pdf_colorspace.h"
#include "pdf_bitmap.h"
#include "pdf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_shading_kind_e
{
    PDF_SHADING_AXIAL       = 2,
    PDF_SHADING_RADIAL      = 3,
    PDF_SHADING_UNSUPPORTED = -1
} pdf_shading_kind;

typedef struct pdf_shading_s
{
    pdf_shading_kind kind;
    pdf_colorspace   cs;
    pdf_function     fn;

    /* axial: [x0,y0,x1,y1,_,_] ; radial: [x0,y0,r0,x1,y1,r1] -- ambos
     * en espacio de coordenadas del shading (el llamador transforma). */
    double coords[6];
    double domain[2]; /* t0,t1 -- default 0,1 */
    int    extend0, extend1;

    int       has_background;
    pdf_color background;
} pdf_shading;

/* Carga un shading desde 'sh_obj' (dict o stream -- un shading puede
 * ser un stream si trae /BackgroundColor con datos de mesh, pero para
 * axial/radial casi siempre es un dict simple). Devuelve PDF_OK si se
 * pudo describir (incluso si /ShadingType no es 2 o 3, en cuyo caso
 * 'out->kind' queda en PDF_SHADING_UNSUPPORTED para que el llamador
 * degrade con gracia -- no pintar nada), o un error si 'sh_obj' esta
 * demasiado malformado para describirlo en absoluto. */
int pdf_shading_load(pdf_stream *st, const pdf_xref_table *xref,
                     pdf_obj *sh_obj, pdf_arena *arena, pdf_shading *out);

/* Evalua el color del shading en el punto (x,y), YA EN ESPACIO DE
 * COORDENADAS DEL SHADING (el llamador debe transformar device->
 * shading antes de llamar). Devuelve 1 y llena '*out' si el punto cae
 * dentro del dominio pintado (t en [domain0,domain1] extendido segun
 * Extend[]), o 0 si el punto NO debe pintarse (fuera de dominio sin
 * extension, o circulos radiales sin interseccion valida) -- el
 * llamador debe dejar ese pixel intacto, nunca pintar un color
 * inventado. */
int pdf_shading_eval(const pdf_shading *sh, double x, double y, pdf_color *out);

/* Pinta el shading dentro del clip ACTUALMENTE INSTALADO en 'bmp'
 * (bmp->clip_x0..y1 + bmp->clip_mask, ambos ya aplicados gratis por
 * pdf_bitmap_set_pixel_coverage). 'device_to_shading' es la inversa,
 * ya calculada por el llamador, de (Matrix del shading/patron * CTM *
 * transformacion a espacio de pixel) -- ver pdf_render.c. Respeta
 * bmp->opacity/blend_mode/soft_mask igual que cualquier otro pintado
 * (el llamador debe sincronizarlos antes, ver sync_paint_state en
 * pdf_render.c). */
void pdf_shading_paint_clip(pdf_bitmap *bmp, const pdf_shading *sh,
                            pdf_matrix device_to_shading);

/* Callback de color por pixel para rellenos de path con patron de
 * shading (ver pdf_raster_fill_path_shaded en pdf_raster.h) -- declarado
 * aca porque es especifico de shading aunque el bucle de relleno viva
 * en pdf_raster.c. Devuelve 1 si pinto (escribe '*out'), 0 para no
 * pintar ese pixel (mismo criterio que pdf_shading_eval). */
typedef int (*pdf_shading_pixel_fn)(void *user, int x, int y, pdf_color *out);

#ifdef __cplusplus
}
#endif

#endif /* PDF_SHADING_H */
