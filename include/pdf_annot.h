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

/* --- Formas libres: /Line, /Square, /Circle, /Ink ---------------------
 *
 * Mismo patron que Highlight arriba: una funcion "generate_*_appearance"
 * que arma el stream sintetico (/AP/N), y una funcion "new_*" que arma
 * el dict de la anotacion en si (siempre con el stream de apariencia
 * como PUNTERO DIRECTO en /AP/N, nunca pdf_obj_new_ref -- mismo motivo
 * documentado arriba para Highlight: un item de array que referencia un
 * numero de objeto sintetico de esta sesion no resolveria hasta guardar
 * y reabrir). Todo en espacio de pagina PDF NATIVO -- igual que
 * Highlight, la conversion desde "topdown" se hace ANTES de llamar aca,
 * en HB_FUNC(PDF_ANNOTADDSHAPE) (harbour/pdf_hbfunc.c).
 *
 * A diferencia de Highlight (un RELLENO que cabe exacto dentro de sus
 * quads), estas 4 formas son TRAZOS -- tienen ancho real que se sale de
 * la linea/curva matematica. 'out_bbox' de cada generador ya viene
 * expandido con el margen necesario (ancho de linea, mas el ancho de
 * las alas de flecha para Line) para que el trazo no quede recortado en
 * el borde del /BBox del Form. */

/* Cantidad maxima de puntos que puede llevar UN solo trazo de Ink --
 * limite defensivo (protege contra un arrastre patologico, p.ej. si el
 * mouse-up nunca llega a liberar la captura), no una limitacion real de
 * uso: un trazo real de mouse, muestreado a una distancia minima
 * razonable entre puntos, nunca se acerca a este numero. */
#define PDF_ANNOT_MAX_INK_POINTS 4096

/* Linea recta de (x1,y1) a (x2,y2), con una punta de flecha triangular
 * rellena en el extremo (x2,y2) -- el /AP generado es autocontenido
 * (dibuja la flecha el mismo, no depende de que el lector interprete
 * /LE). 'line_width' es el ancho de trazo del eje (no de la flecha).
 * NULL si (x1,y1)==(x2,y2) (linea degenerada, sin direccion para la
 * flecha) o 'out_bbox'/'arena' invalidos. */
pdf_obj *pdf_annot_generate_line_appearance(pdf_arena *arena,
    double x1, double y1, double x2, double y2,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox);

/* /Type /Annot /Subtype /Line /Rect=rect /L=[x1 y1 x2 y2] /C=[r g b]
 * /CA=1.0 /F=4 /LE=[/None /OpenArrow] (metadata de interop -- NO
 * determina el dibujo real, eso lo hace /AP/N generado arriba)
 * /AP<</N=ap_stream_obj>>. */
pdf_obj *pdf_annot_new_line(pdf_arena *arena, const double *rect,
    double x1, double y1, double x2, double y2,
    double r, double g, double b, pdf_obj *ap_stream_obj);

/* Rectangulo SIN relleno (solo contorno, sin /IC) inscripto en
 * [x0,y0,x1,y1] (ya normalizado: x0<x1, y0<y1). NULL si el rect es
 * degenerado (ancho o alto <= 0) o argumentos invalidos. */
pdf_obj *pdf_annot_generate_square_appearance(pdf_arena *arena,
    double x0, double y0, double x1, double y1,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox);

/* /Type /Annot /Subtype /Square /Rect=rect /C=[r g b] /CA=1.0 /F=4
 * /AP<</N=ap_stream_obj>>. Sin /IC (sin color de relleno). */
pdf_obj *pdf_annot_new_square(pdf_arena *arena, const double *rect,
    double r, double g, double b, pdf_obj *ap_stream_obj);

/* Elipse SIN relleno inscripta en [x0,y0,x1,y1], aproximada con 4
 * curvas Bezier cubicas (kappa=0.5522847498307936, tecnica estandar)
 * -- el interprete de content-streams de este motor soporta el
 * operador 'c' sin limite de cantidad (ver pdf_render.c). NULL si el
 * rect es degenerado o argumentos invalidos. */
pdf_obj *pdf_annot_generate_circle_appearance(pdf_arena *arena,
    double x0, double y0, double x1, double y1,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox);

/* /Type /Annot /Subtype /Circle /Rect=rect /C=[r g b] /CA=1.0 /F=4
 * /AP<</N=ap_stream_obj>>. Sin /IC. */
pdf_obj *pdf_annot_new_circle(pdf_arena *arena, const double *rect,
    double r, double g, double b, pdf_obj *ap_stream_obj);

/* Trazo libre (freehand): 'points' trae n_points*2 doubles (x,y
 * intercalados) de UN solo trazo continuo -- se dibuja como una
 * polilinea abierta (sin 'h', un trazo libre no se autocierra).
 * 'n_points' debe estar en [2, PDF_ANNOT_MAX_INK_POINTS]. NULL fuera de
 * ese rango o argumentos invalidos. */
pdf_obj *pdf_annot_generate_ink_appearance(pdf_arena *arena,
    const double *points, int n_points,
    double r, double g, double b, double line_width,
    long new_obj_num, double *out_bbox);

/* /Type /Annot /Subtype /Ink /Rect=rect /InkList=[[x1 y1 x2 y2 ...]]
 * (un solo sub-array = un solo trazo por anotacion, alcance v1)
 * /C=[r g b] /CA=1.0 /F=4 /AP<</N=ap_stream_obj>>. */
pdf_obj *pdf_annot_new_ink(pdf_arena *arena, const double *rect,
    const double *points, int n_points,
    double r, double g, double b, pdf_obj *ap_stream_obj);

/* --- Globo de tip: /FreeText (clic + texto corto, siempre visible) ---
 *
 * Un solo punto de click (no un arrastre) con un mensaje corto tipeado
 * por el usuario -- se dibuja como un rectangulo con relleno amarillo
 * palido (estilo post-it) + borde, una colita triangular que apunta
 * exactamente al punto clickeado, y el texto envuelto en lineas dentro
 * del rectangulo. A diferencia de Highlight/Formas (geometria pura),
 * esta es la primera anotacion de este modulo que dibuja TEXTO real
 * dentro de su propia apariencia generada. */

/* Bytes del mensaje -- guarda defensiva (protege contra un TGet con
 * un buffer inusualmente grande o un llamador con un bug), no una
 * limitacion real de uso: un "tip" real nunca se acerca a esto. */
#define PDF_ANNOT_TIP_MAX_TEXT_LEN 300
/* Lineas tras el wrap por palabra -- guarda defensiva analoga (acota
 * el tamanio del buffer de contenido sin necesidad de medirlo antes). */
#define PDF_ANNOT_TIP_MAX_LINES    20

/* Envuelve 'text' en lineas (wrap "greedy" por palabra, midiendo cada
 * una con pdf_afm_width("Helvetica", code) -- ver pdf_afm.h; codigos
 * fuera del rango ASCII imprimible 32-126 caen al generico 500/1000 em,
 * mismo criterio tolerante que el resto del motor) y genera el
 * rectangulo+colita+texto completos, en espacio de pagina PDF NATIVO.
 * El globo SIEMPRE crece hacia arriba y a la derecha desde
 * (anchor_x,anchor_y) -- sin clampeo contra los bordes de la pagina en
 * esta primera version (ver DESIGN.md).
 *
 * 'out_bbox' = union del punto ancla y el rectangulo del globo -- se
 * usa TANTO como /BBox del Form como /Rect de la anotacion (mismo
 * criterio que Highlight/Formas: si no coinciden exactamente, la
 * matriz de Appearance Streams deja de ser identidad y el globo sale
 * distorsionado).
 *
 * Devuelve NULL si 'text' es NULL, vacio, o mas largo que
 * PDF_ANNOT_TIP_MAX_TEXT_LEN, o argumentos invalidos. */
pdf_obj *pdf_annot_generate_tip_appearance(pdf_arena *arena,
    double anchor_x, double anchor_y, const char *text,
    long new_obj_num, double *out_bbox);

/* /Type /Annot /Subtype /FreeText /Rect=rect /Contents=text (texto
 * plano -- para cualquier lector/herramienta que lea el contenido sin
 * pasar por /AP) /DA="/Helv 9 Tf 0 g" (requerido por la norma para
 * /FreeText) /IC=[r g b] (mismo amarillo palido que ya pinta el /AP --
 * interop: un lector que no respete /AP tiene mejor chance de
 * aproximar el look real) /CA=1.0 /F=4 /AP<</N=ap_stream_obj>>
 * (puntero DIRECTO, mismo motivo ya documentado arriba para
 * Highlight/Formas). */
pdf_obj *pdf_annot_new_freetext(pdf_arena *arena, const double *rect,
    const char *text, pdf_obj *ap_stream_obj);

#ifdef __cplusplus
}
#endif

#endif /* PDF_ANNOT_H */
