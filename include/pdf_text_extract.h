/* pdf_text_extract.h
 *
 * Extraccion y busqueda de texto (fase 2 del roadmap de potencialidad
 * MuPDF -- ver DESIGN.md seccion 70). Corre el content stream de una
 * pagina igual que un render normal (mismo pdf_render_device/
 * pdf_content_run que pdf_page_run_device), pero en vez de -- o ademas
 * de -- pintar pixeles, recolecta CADA caracter mostrado via el gancho
 * pdf_text_extract_fn (pdf_render.h), incluido texto invisible (Tr 3/7,
 * comun en capas OCR sobre un escaneo) y espacios, en un arreglo plano
 * pdf_text_glyph por pagina.
 *
 * Coordenadas SIEMPRE a scale=1.0 (puntos PDF, mismo espacio que
 * TPdfViewer:nPageWidthPt/nPageHeightPt) -- la extraccion es
 * independiente del zoom vigente; el llamador (UI) convierte a pixels
 * de pantalla multiplicando por su factor de zoom al momento de
 * dibujar/consultar. Esto significa que un pdf_text_page extraido una
 * vez sirve para cualquier nivel de zoom sin volver a extraer.
 */

#ifndef PDF_TEXT_EXTRACT_H
#define PDF_TEXT_EXTRACT_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_parser.h"
#include "pdf_page.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_text_glyph_s
{
    int    unicode;      /* codepoint Unicode resuelto (ver pdf_font_get_unicode) */
    int    raw_code;     /* codigo crudo del PDF (byte o CID), diagnostico */
    double x0_px, y0_px; /* origen del baseline, espacio de puntos PDF (scale=1.0) */
    double advance_px;   /* ancho de avance de ESTE caracter */
    double height_px;    /* tamanio aproximado de fuente */
    double rotation_deg; /* ver pdf_glyph_draw_fn en pdf_render.h */
    int    render_mode;  /* Tr vigente -- 3/7 = invisible, se incluye igual */
    int    is_space;     /* unicode == ' ' (32) -- limite de palabra */
} pdf_text_glyph;

typedef struct pdf_text_page_s
{
    pdf_text_glyph *glyphs;      /* provisto por el llamador (arena_alloc), capacidad glyph_cap */
    int             glyph_cap;
    int             n_glyphs;    /* trunca en glyph_cap, no crashea si la pagina tiene mas */
    double          page_width_pt;
    double          page_height_pt;
} pdf_text_page;

/* Extrae todos los caracteres mostrados de 'page_obj' hacia 'out'.
 * 'out->glyphs'/'out->glyph_cap' deben estar seteados por el llamador
 * ANTES de llamar (arena_alloc de un arreglo pdf_text_glyph); esta
 * funcion llena 'out->n_glyphs' (0..glyph_cap) y
 * 'out->page_width_pt'/'page_height_pt'. Usa page->page_arena/
 * decode_arena para el bitmap throwaway y el content stream
 * decodificado -- no persiste nada de eso, solo lo que queda copiado
 * en 'out->glyphs'.
 *
 * Devuelve PDF_OK (incluso si la pagina no tiene /Contents -- en ese
 * caso 'out->n_glyphs' queda en 0, no es un error) o un codigo de
 * error si no se pudo crear el bitmap throwaway. */
int pdf_text_extract_page(pdf_stream *st, const pdf_xref_table *xref,
                           pdf_page *page, pdf_obj *page_obj,
                           pdf_text_page *out);

/* El motor no trackea metricas reales de fuente (ascent/descent de las
 * tablas hhea/OS2) por glyph, solo 'height_px' (aprox. el alto del
 * em-square completo, ver pdf_render.c). Un glyph tipico ocupa ~80% de
 * eso ARRIBA del baseline (ascenso: mayusculas, altura-x, ascendentes
 * como 'l'/'h') y ~20% ABAJO (descenso: 'g'/'p'/'y'/'j'/'q'). BUG REAL
 * ENCONTRADO (Arturo: "esta marcado sin encuadre a la seleccion en el
 * sentido vertical") -- pdf_text_search()/PDF_GLYPHSINRECT ponian el
 * 100% de height_px ARRIBA del baseline y 0% abajo, así que el
 * recuadro de seleccion/resaltado quedaba pegado al baseline (cortando
 * descendentes) y con un hueco vacio de mas arriba (el ascenso real es
 * bastante menor al alto completo del em-square). Constantes
 * compartidas para no repetir el numero mágico en cada sitio que arma
 * un bbox vertical de texto. */
#define PDF_TEXT_ASCENT_FRAC  0.80
#define PDF_TEXT_DESCENT_FRAC 0.20

typedef struct pdf_text_match_s
{
    int    start_glyph_idx, end_glyph_idx; /* inclusive, indices en tp->glyphs */
    double x0, y0, x1, y1;                 /* bbox aproximado, espacio de puntos PDF */
} pdf_text_match;

/* Busca 'needle_utf8' (string UTF-8, terminado en NUL) dentro de
 * 'tp->glyphs' -- incluye texto invisible (Tr 3/7, capas OCR) a
 * proposito, ver comentario del archivo. Busqueda simple de
 * subsecuencia consecutiva de codepoints (sin normalizacion Unicode
 * mas alla de mayus/minus ASCII cuando !case_sensitive). Escribe hasta
 * 'max_matches' resultados en 'out_matches' y la cantidad real
 * encontrada (puede ser mayor a max_matches, los de mas se descartan
 * sin crashear) en '*out_count'. Devuelve PDF_OK, o PDF_ERR_BADARG si
 * 'needle_utf8' es NULL/vacio. */
int pdf_text_search(const pdf_text_page *tp, const char *needle_utf8,
                     int case_sensitive, pdf_text_match *out_matches,
                     int max_matches, int *out_count);

/* Encuentra el indice (0-based) del glyph mas cercano al punto (x,y)
 * (puntos PDF), en el sentido de "posicion de insercion de cursor de
 * texto" -- prioriza fuertemente coincidir la LINEA (diferencia en Y)
 * por sobre la posicion horizontal (X): dos glyphs en la misma linea
 * pero en extremos opuestos son "mas cercanos" en este sentido que dos
 * glyphs en lineas distintas aunque esten a la misma distancia en X.
 * Pensado para mapear el punto de inicio/fin de un arrastre de mouse a
 * una posicion en la SECUENCIA de texto (ver pdf_text_glyph -- el
 * arreglo ya esta en orden de aparicion del content stream, que para
 * un documento bien formado coincide con el orden de lectura), no a
 * una interseccion geometrica de rectangulo -- asi una seleccion que
 * cruza varias lineas sigue letra-palabra-parrafo en vez de recortar
 * por columna de X en cada linea (BUG REAL ENCONTRADO: Arturo reporto
 * que el texto copiado salia "al azar" en vez de seguir la secuencia
 * de letras/palabras -- la seleccion por rectangulo puro es geometrica,
 * no de flujo de texto). Devuelve -1 si tp->n_glyphs == 0. */
int pdf_text_nearest_glyph(const pdf_text_page *tp, double x, double y);

#ifdef __cplusplus
}
#endif

#endif /* PDF_TEXT_EXTRACT_H */
