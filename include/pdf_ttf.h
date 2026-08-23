/* pdf_ttf.h
 *
 * Parser TrueType (sfnt) minimo + rasterizador de contornos, para
 * reemplazar la caja placeholder de texto (ver pdf_render.h) por
 * glyphs reales -- similar en PROPOSITO a como MuPDF usa FreeType, pero
 * sin esa (ni ninguna otra) dependencia externa: parser y aplanado de
 * curvas propios, en el mismo estilo C89 minimalista del resto del
 * motor. Ver DESIGN.md, ronda "render de fuentes real".
 *
 * Este modulo NO sabe nada de PDF ni de pdf_render/pdf_matrix -- solo
 * entiende el formato de archivo TrueType (.ttf) y expone contornos ya
 * aplanados (curvas cuadraticas subdivididas a segmentos de recta) via
 * callbacks moveto/lineto, en espacio de glyph NORMALIZADO a em
 * (coordenadas originales / unitsPerEm, o sea 1.0 = un em) -- el
 * llamador (pdf_render.c) es quien conoce la matriz de render de texto
 * y transforma cada punto a espacio de dispositivo.
 *
 * Alcance: solo TrueType 'glyf' clasico (glyphs simples y compuestos),
 * NO CFF/OpenType ('CFF ' table) ni Type1. Tolerante en cada paso:
 * datos fuera de rango o con forma inesperada hacen que las funciones
 * devuelvan error/0 sin tocar la salida ni crashear -- el llamador cae
 * de vuelta a la caja placeholder (mismo criterio tolerante que el
 * resto del motor: filtros no soportados, fuentes no encontradas,
 * etc. se degradan en vez de romper el render).
 */

#ifndef PDF_TTF_H
#define PDF_TTF_H

#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Datos parseados de un archivo .ttf -- NO copia 'data' (debe
 * permanecer vivo, tipicamente en una arena, mientras se use este
 * struct). Sin destructor: son solo offsets/enteros hacia 'data'. */
typedef struct pdf_ttf_font_s
{
    const unsigned char *data;
    long                  data_len;

    int  units_per_em;      /* de 'head', tipicamente 1000 o 2048 */
    int  loca_long;          /* 'head'.indexToLocFormat: 0=short(x2), 1=long */
    int  num_glyphs;         /* de 'maxp' */

    long loca_off, loca_len;
    long glyf_off, glyf_len;

    /* subtabla cmap preferida para texto unicode normal (formato 4 o
     * 12) -- 0 si no se encontro ninguna usable. */
    long cmap_unicode_off;
    int  cmap_unicode_format;

    /* subtabla cmap (3,0) "Symbol" (Wingdings/Webdings/Symbol -- ver
     * DESIGN.md seccion 36) -- 0 si no existe. Convencion: se consulta
     * con code+0xF000. */
    long cmap_symbol_off;

    /* 'hmtx' (ancho de avance NATURAL de cada glyph en esta fuente,
     * ver pdf_ttf_glyph_advance_em) -- 0/0 si no se encontro 'hhea'/
     * 'hmtx' (tolerante: el llamador simplemente no reescala). */
    long hmtx_off;
    int  num_h_metrics;
} pdf_ttf_font;

/* Parsea el directorio de tablas sfnt y las cabeceras de head/maxp/
 * loca/cmap (NO decodifica glyphs todavia, eso es bajo demanda via
 * pdf_ttf_glyph_outline). 'data'/'data_len' deben seguir vivos
 * mientras se use 'out'. Devuelve PDF_OK o error. */
int pdf_ttf_load(const unsigned char *data, long data_len, pdf_ttf_font *out);

/* Codepoint Unicode -> glyph index, via la subtabla cmap unicode
 * (formato 4 o 12). Devuelve 0 (glyph .notdef, convencion TrueType
 * estandar) si no hay mapeo o no hay subtabla usable. */
int pdf_ttf_gid_for_unicode(const pdf_ttf_font *font, int unicode);

/* Codigo de caracter crudo (0-255) -> glyph index, via la subtabla
 * cmap (3,0) "Symbol" (convencion code+0xF000, y tambien se prueba el
 * code crudo por si la fuente ya lo indexa asi). Devuelve 0 si no hay
 * subtabla symbol o no hay mapeo. Pensado para fuentes tipo
 * Wingdings/Webdings/Symbol -- ver 'pdf_ttf_find_system_font'. */
int pdf_ttf_gid_for_symbol_code(const pdf_ttf_font *font, int code);

typedef void (*pdf_ttf_moveto_fn)(void *user, double x, double y);
typedef void (*pdf_ttf_lineto_fn)(void *user, double x, double y);

/* Extrae el contorno del glyph 'gid' y lo entrega aplanado (curvas
 * cuadraticas subdivididas) via 'moveto'/'lineto', en espacio de glyph
 * normalizado a em (coordenadas originales / units_per_em). Maneja
 * glyphs compuestos recursivamente -- 'depth' debe pasarse en 0 desde
 * el llamador externo (se usa como guarda contra recursion infinita
 * con fuentes corruptas/circulares, igual espiritu que 'form_depth' en
 * pdf_render_device; profundidad maxima interna: 8).
 *
 * Devuelve PDF_OK si se emitio al menos un contorno, PDF_ERR_NOTFOUND
 * si 'gid' es 0/.notdef o esta fuera de rango (glyph vacio, comun para
 * espacio -- no es un error real, el llamador simplemente no dibuja
 * nada), o PDF_ERR_BADARG/PDF_ERR_UNSUPPORTED si los datos del glyph
 * son invalidos -- en NINGUN caso crashea. */
int pdf_ttf_glyph_outline(const pdf_ttf_font *font, int gid,
                           pdf_ttf_moveto_fn moveto, pdf_ttf_lineto_fn lineto,
                           void *user, int depth);

/* BUG REAL ENCONTRADO (render de fuentes real, confirmado contra
 * Utilization_and_efficiency_of_ground_gra.pdf): dibujar el contorno
 * del glyph SIN reescalarlo horizontalmente al ancho declarado en
 * /Widths del PDF hace que, con una fuente sustituta de sistema (cuyas
 * proporciones no coinciden con las de la fuente original embebida
 * /FontFile3 Type1/CFF -- fuera de alcance, ver pdf_ttf.h), letras
 * angostas como 'f'/'i' se DIBUJEN SUPERPUESTAS unas sobre otras (el
 * avance entre ellas usa el ancho ANGOSTO original, pero el glyph
 * sustituto se dibuja con su ancho NATURAL, mas grande) -- el
 * resultado visual es una mancha que puede confundirse con una letra
 * distinta (p.ej. "efficiency" se veia "efPciency"). El gancho GDI
 * (pdf_render.h, 'advance_width_px') YA documentaba este requisito
 * ("el backend deberia usar esto para escalar horizontalmente el
 * glyph") pero el camino de contornos reales no lo implementaba.
 * Devuelve el ancho de avance NATURAL del glyph 'gid' en esta fuente,
 * normalizado a em (1.0 = un em, mismo criterio que pdf_ttf_glyph_outline)
 * -- el llamador (pdf_render.c) calcula 'w0_declarado / esto' como
 * factor de escala horizontal antes de transformar cada punto del
 * contorno. Devuelve <= 0.0 si no se pudo determinar (sin 'hmtx', gid
 * fuera de rango) -- el llamador no debe reescalar en ese caso. */
double pdf_ttf_glyph_advance_em(const pdf_ttf_font *font, int gid);

/* ---- Sustitucion por fuente de sistema (sin GDI) -------------------
 *
 * Busca un archivo .ttf real en la carpeta de fuentes de Windows
 * (via getenv("SystemRoot") + "\Fonts\", sin ninguna llamada a
 * GDI/Win32 -- solo getenv+fopen, por eso compila igual en el motor
 * portable) que aproxime 'base_font_name' (heuristica de palabras
 * clave: Courier/Consolas/mono, Times/Georgia/Garamond/Cambria,
 * Symbol, Wingdings/Webdings exactos, y arial.ttf como fallback
 * generico sans-serif) combinado con is_bold/is_italic (sufijos
 * "","bd","i","bi" -- convencion estable de Windows desde Win95) y
 * is_serif como ultimo desempate si el nombre no dio ninguna pista.
 *
 * Carga y parsea el archivo (si lo encuentra) UNA sola vez por
 * ejecucion del proceso: mantiene una cache interna de vida de
 * PROCESO (arena+ledger dedicados, ~32MB de presupuesto, lazy-init en
 * el primer uso) -- excepcion deliberada y acotada al patron arena=
 * doc/page/decode del resto del motor (ver pdf_mem.h): un archivo de
 * fuente de sistema tiene sentido reusarlo mas alla de una pagina o
 * incluso de un documento (typicamente la misma app abre muchos PDFs
 * en la misma sesion, todos con las mismas fuentes estandar).
 *
 * Devuelve un puntero a un pdf_ttf_font ya cargado (vive tanto como el
 * proceso, no liberar) o NULL si no se encontro/pudo parsear ningun
 * archivo razonable (el llamador cae a la caja placeholder). */
const pdf_ttf_font *pdf_ttf_find_system_font(const char *base_font_name,
                                              int is_bold, int is_italic,
                                              int is_serif);

/* Igual que pdf_ttf_find_system_font pero fuerza la busqueda de una
 * fuente de simbolos (Symbol/Wingdings/Webdings) -- usar cuando
 * 'base_font_name' sugiere una de estas (ver pdf_render.c) para elegir
 * el archivo exacto en vez de caer al generico sans-serif. */
const pdf_ttf_font *pdf_ttf_find_system_symbol_font(const char *base_font_name);

#ifdef __cplusplus
}
#endif

#endif /* PDF_TTF_H */
