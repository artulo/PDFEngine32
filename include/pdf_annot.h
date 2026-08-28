/* pdf_annot.h
 *
 * Anotaciones de marcado (Markup Annotations) -- primera pieza:
 * /Subtype /Highlight (resaltado de texto), ver DESIGN.md. A
 * diferencia de pdf_form.h (AcroForm: campos interactivos, /FT/DA/
 * herencia de /Parent), este modulo no necesita nada de eso -- una
 * anotacion de marcado es solo un dict con /Rect, /QuadPoints, /C,
 * /AP, sin ningun concepto de "campo" ni de arbol de formulario. Deja
 * lugar natural a /Underline, /StrikeOut, etc mas adelante (mismo
 * mecanismo de apariencia generada + /QuadPoints).
 *
 * Ambas funciones trabajan SIEMPRE en espacio de usuario PDF NATIVO
 * (bottom-up, sin ninguna rotacion aplicada) -- la conversion desde el
 * espacio "topdown" que usa la seleccion de texto de la UI
 * (pdf_viewer.prg) se hace ANTES de llamar aca, en
 * HB_FUNC(PDF_ANNOT_ADDHIGHLIGHT) (harbour/pdf_hbfunc.c), via
 * pdf_render_topdown_to_native() (pdf_render.h). Este modulo no sabe
 * nada de "topdown" ni de /Rotate -- solo PDF puro.
 */

#ifndef PDF_ANNOT_H
#define PDF_ANNOT_H

#include "pdf_object.h"
#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cantidad maxima de quads (lineas de texto) que puede llevar UN solo
 * resaltado -- limite defensivo (un arrastre de seleccion pathologico
 * o un llamador con un bug no puede volar el presupuesto de memoria),
 * no una limitacion de la norma. Una seleccion mas larga que esto
 * simplemente falla (PDF_ANNOT_ADDHIGHLIGHT devuelve .F.); en la
 * practica ninguna seleccion de usuario real se acerca a este numero
 * de LINEAS. */
#define PDF_ANNOT_MAX_QUADS 512

/* Genera una apariencia (/AP/N) para un resaltado -- un rectangulo
 * relleno por cada quad de 'quads' (n_quads*8 doubles: 4 puntos por
 * quad, orden TL,TR,BL,BR -- MISMO orden que /QuadPoints, ver
 * pdf_annot_new_highlight), color (r,g,b) con blend Multiply y
 * opacidad 'alpha', TODO en espacio de pagina PDF NATIVO (bottom-up).
 *
 * OJO -- el orden TL,TR,BL,BR es el de ALMACENAMIENTO (QuadPoints),
 * NO un orden de perimetro valido para rellenar un poligono: conectar
 * TL->TR->BL->BR->cerrar dibuja un "moño" autointersectado. El PATH de
 * relleno que arma esta funcion recorre el perimetro real
 * (TL->TR->BR->BL->cerrar) -- si se toca este codigo, no "simplificar"
 * usando el orden de QuadPoints directo.
 *
 * 'out_bbox' (4 doubles: x0,y0,x1,y1) recibe el bounding box union de
 * todos los quads -- se usa como /BBox del Form generado (identico a
 * como el llamador arma /Rect de la anotacion con el MISMO valor, ver
 * pdf_annot_new_highlight) para que la matriz de posicionamiento de
 * Appearance Streams (norma 12.5.5, ver draw_annot_appearance en
 * pdf_render.c) resulte identidad -- el contenido ya esta en
 * coordenadas absolutas de pagina, no relativas a una caja local.
 *
 * Devuelve un stream SINTETICO (pdf_obj_new_synthetic_stream) con
 * 'new_obj_num' como numero de objeto -- el llamador debe haber
 * elegido un numero de objeto libre y agregarlo a la lista de objetos
 * a guardar (ver pdf_write.h). NULL si los argumentos son invalidos o
 * 'n_quads' excede PDF_ANNOT_MAX_QUADS. */
pdf_obj *pdf_annot_generate_highlight_appearance(pdf_arena *arena,
    const double *quads, int n_quads,
    double r, double g, double b, double alpha,
    long new_obj_num, double *out_bbox);

/* Arma el diccionario de la anotacion: /Type /Annot /Subtype /Highlight
 * /Rect=rect (4 doubles, espacio nativo) /QuadPoints (8*n_quads
 * doubles, TL,TR,BL,BR por quad -- T.32000-1 8.4.5 dice "orden
 * antihorario" en el texto, pero Acrobat -- y por lo tanto todo lector
 * real que necesite interoperar -- siempre uso TL,TR,BL,BR; este
 * modulo sigue esa convencion de facto, no el texto literal del spec)
 * /C=[r g b] /CA=1.0 /F=4 (Print, sobrevive impresion)
 * /AP<</N=ap_stream_obj>> (puntero de stream DIRECTO como valor de
 * dict -- pdf_write.c ya lo serializa como referencia "N G R"
 * automaticamente, no hace falta envolverlo en pdf_obj_new_ref).
 *
 * Devuelve el dict nuevo (arena-allocated, sin obj_num propio -- un
 * PDF_DICT no lo guarda; el llamador lo agrega a la lista de objetos a
 * guardar usando el numero de objeto que el mismo eligio para esta
 * anotacion). NULL si los argumentos son invalidos. */
pdf_obj *pdf_annot_new_highlight(pdf_arena *arena,
    const double *rect, const double *quads, int n_quads,
    double r, double g, double b,
    pdf_obj *ap_stream_obj);

#ifdef __cplusplus
}
#endif

#endif /* PDF_ANNOT_H */
