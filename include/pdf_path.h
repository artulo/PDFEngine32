/* pdf_path.h
 *
 * Construccion de paths vectoriales (m/l/c/v/y/h/re) ya transformados a
 * espacio de dispositivo -- las curvas Bezier cubicas se "aplanan" a
 * segmentos de recta en el momento de agregarlas (PDF_BEZIER_SEGMENTS
 * segmentos por curva, suficiente para el detalle de un plano tecnico a
 * escala de pantalla/impresora tipica).
 *
 * Capacidad FIJA (sin arena, vive en la pila o embebido en el device):
 * PDF_PATH_MAX_POINTS puntos totales, PDF_PATH_MAX_SUBPATHS subpaths.
 * Si un path real excede esto, los puntos/subpaths de mas se descartan
 * silenciosamente (documentado, no crashea) -- suficiente para planos
 * tipicos; si hace falta mas, subir las constantes es trivial.
 *
 * BUG REAL ENCONTRADO (Arturo: "se introducen lineas que no
 * corresponden" en D-6025IC1r0.pdf, un P&ID de AutoCAD) --
 * PDF_PATH_MAX_SUBPATHS estaba en 256, muy por debajo de lo que un
 * plano CAD real necesita (confirmado: un solo path de este archivo,
 * antes de su 'S', hace 361 'm' -- "texto de una sola linea" estilo
 * AutoCAD dibuja cada letra como su propio subpath chico, y un plano
 * con muchas etiquetas los acumula rapido). Subido a 2048 -- ver
 * ademas el fix en pdf_path.c (subpath_overflow) para el caso
 * residual en que incluso esto no alcance: sin ese fix, un 'm' que no
 * entra por el cupo no arrancaba nada, pero el 'l' que le seguia SI
 * se agregaba -- al subpath ANTERIOR valido, prolongandolo con un
 * punto que no le pertenece. El rasterizador dibuja cada subpath como
 * una polilinea continua, asi que ese punto huerfano aparecia como
 * una linea larga conectando dos partes del dibujo sin ninguna
 * relacion real entre si. */

#ifndef PDF_PATH_H
#define PDF_PATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_PATH_MAX_POINTS    8192
#define PDF_PATH_MAX_SUBPATHS  2048
#define PDF_BEZIER_SEGMENTS    12

typedef struct pdf_path_point_s
{
    double x, y;
} pdf_path_point;

typedef struct pdf_path_s
{
    pdf_path_point points[PDF_PATH_MAX_POINTS];
    int n_points;

    int subpath_start[PDF_PATH_MAX_SUBPATHS];  /* indice en points[] */
    int subpath_len[PDF_PATH_MAX_SUBPATHS];
    int subpath_closed[PDF_PATH_MAX_SUBPATHS];
    int n_subpaths;

    /* punto actual y punto de inicio del subpath actual, en espacio de
     * dispositivo (ya transformados) -- necesarios para 'v'/'y'/'h'. */
    double cur_x, cur_y;
    double start_x, start_y;
    int    has_current;

    /* .T. si el ULTIMO 'm' se descarto por cupo agotado (ver comentario
     * grande de PDF_PATH_MAX_SUBPATHS arriba) -- mientras esta en .T.,
     * cualquier 'l'/'c'/'v'/'y' que siga se descarta TAMBIEN (en vez de
     * agregarse por error al subpath valido anterior), hasta el proximo
     * 'm' que si entre por cupo. */
    int    subpath_overflow;
} pdf_path;

void pdf_path_reset(pdf_path *path);

/* Todas las coordenadas que reciben estas funciones ya deben venir
 * transformadas por el CTM vigente (el llamador -- pdf_render.c -- es
 * quien conoce y aplica la matriz). */
void pdf_path_moveto(pdf_path *path, double x, double y);
void pdf_path_lineto(pdf_path *path, double x, double y);
void pdf_path_curveto(pdf_path *path, double x1, double y1,
                       double x2, double y2, double x3, double y3);
void pdf_path_close(pdf_path *path);

/* Atajo para 're': equivale a moveto+lineto*3+close, pero recibe las 4
 * esquinas ya transformadas individualmente (un 'cm' con rotacion hace
 * que un rectangulo en espacio de usuario deje de ser axis-aligned en
 * espacio de dispositivo). */
void pdf_path_rect_corners(pdf_path *path,
                            double x0, double y0, double x1, double y1,
                            double x2, double y2, double x3, double y3);

#ifdef __cplusplus
}
#endif

#endif /* PDF_PATH_H */
