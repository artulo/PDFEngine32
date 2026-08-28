/* pdf_render.h
 *
 * Device de render: implementa pdf_content_op_fn traduciendo operadores
 * de graficos Y de texto a pixeles sobre un pdf_bitmap.
 *
 * Grafico (sin cambios respecto de la version anterior):
 *   q, Q, cm, m, l, c, v, y, h, re, S, s, f, F, f*, B, B*, b, b*, n, rg,
 *   RG, g, G
 *
 * Texto (nuevo -- ver DESIGN.md seccion 11):
 *   BT, ET, Tf, Td, TD, Tm, T*, Tc, Tw, Tz, TL, Ts, Tr, Tj, TJ, ' , "
 *
 *   Requiere que el llamador pase el /Resources de la pagina (para
 *   resolver /Font por nombre en 'Tf') y el stream/xref/arena para
 *   cargar objetos de fuente bajo demanda -- ver pdf_render_device_init.
 *
 *   El texto se dibuja con el CONTORNO REAL de cada glyph (ver
 *   pdf_ttf.h): fuente TrueType embebida (/FontFile2) si esta
 *   presente, si no un sustituto real leido directo de
 *   C:\Windows\Fonts (sin GDI, ver pdf_ttf_find_system_font) -- solo
 *   cuando NINGUNO de los dos resuelve un glyph dibujable se cae a una
 *   caja rellena aproximando la caja de tinta (usando el ancho real de
 *   /Widths). El gancho pdf_glyph_draw_fn de mas abajo sigue
 *   existiendo, opt-in, con prioridad sobre lo anterior -- usado por
 *   harbour/pdf_hbfunc.c para dibujar con GDI real en vez del
 *   rasterizador propio.
 *
 * Todavia NO soportado:
 *   - Imagenes inline BI/ID/EI.
 *   - Type1 (/FontFile) y CFF/OpenType (/FontFile3) embebidos -- solo
 *     TrueType clasico ('glyf'), ver src/text/README.txt.
 *   - Patrones de mosaico (PatternType 1), shadings tipo mesh (4-7) y
 *     function-based (Type 1), PDF Functions Type 4 (PostScript
 *     calculator) -- ver pdf_shading.h/pdf_function.h para lo que SI
 *     esta soportado (shading axial/radial via 'sh' y patrones de
 *     shading PatternType 2, Functions Type 0/2/3).
 *   - Grosor de linea real, color spaces Separation/DeviceN/Indexed.
 */

#ifndef PDF_RENDER_H
#define PDF_RENDER_H

#include "pdf_content.h"
#include "pdf_image.h"
#include "pdf_bitmap.h"
#include "pdf_raster.h"
#include "pdf_path.h"
#include "pdf_types.h"
#include "pdf_font.h"
#include "pdf_parser.h"
#include "pdf_stream.h"
#include "pdf_filter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_RENDER_MAX_GSTATE_DEPTH 32
#define PDF_RENDER_FONT_CACHE_SIZE  16

typedef struct pdf_gstate_s
{
    pdf_matrix ctm;
    pdf_color  fill_color;
    pdf_color  stroke_color;
    double     line_width; /* 'w', en espacio de usuario -- se escala a pixeles al trazar */

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 51): el operador 'gs'
     * (ExtGState) no se procesaba en absoluto -- ni /ca (alpha de
     * relleno) ni /CA (alpha de trazo) ni blend modes. Esto no
     * importaba mientras los PDFs de prueba solo tuvieran rellenos
     * 100% opacos, pero en PDFs reales es comun usar /ca<1 para
     * efectos de sombra/resaltado (un rectangulo semitransparente con
     * blend mode Multiply/Screen sobre un soft mask, tipico de
     * "sombra suave" detras de un titulo). Sin soporte de alpha/blend,
     * este motor pintaba esos rellenos 100% opacos -- un rectangulo
     * que deberia verse como una sombra sutil terminaba como un bloque
     * solido que tapaba TODO lo que estaba debajo (incluido texto real
     * ya dibujado antes en el mismo Form XObject). Ver pdf_render.c,
     * uso de 'fill_alpha' en los operadores de relleno: no se
     * implementa blending real (fuera de alcance para este motor
     * minimo, ver pdf_bitmap.h), pero al menos se evita el caso mucho
     * peor de tapar contenido real con un color opaco equivocado --
     * mismo espiritu tolerante que el resto del motor (filtros de
     * imagen no soportados, fuentes no encontradas, etc. se ignoran en
     * vez de romper el render). */
    double     fill_alpha;   /* 'ca' via 'gs', 1.0 = opaco (default) */
    double     stroke_alpha; /* 'CA' via 'gs', 1.0 = opaco (default) */
    int        blend_mode;   /* PDF_BLEND_*, via '/BM' de 'gs' (ver pdf_bitmap.h) */

    /* clip rectangular activo, en pixel space (aproximacion por bounding
     * box del path de clip real -- ver pdf_bitmap.h). Se sincroniza con
     * dev->bitmap->clip_* cada vez que cambia via q/Q o W/W*+pintado. */
    int        clip_x0, clip_y0, clip_x1, clip_y1;

    /* mascara de clip por pixel (NULL = solo el rectangulo de arriba).
     * Se rasteriza en finish_path cuando el path de clip no es un
     * rectangulo alineado a ejes -- ver pdf_raster_rasterize_clip_mask.
     * Se salva/restaura por valor con el resto del gstate en q/Q (el
     * puntero sobrevive porque esta alocado en dev->arena, de vida
     * igual o mayor que el gstate stack). */
    unsigned char *clip_mask;
    int            clip_mask_stride;

    /* soft mask de luminosidad activa via '/SMask' de 'gs' (NULL =
     * ninguna). Mismo criterio de vida que clip_mask. */
    unsigned char *smask_mask;
    int            smask_stride;

    /* patron de shading (PatternType 2) resuelto por 'scn' cuando el
     * colorspace de relleno es /Pattern -- NULL = usar fill_color plano
     * (comportamiento actual, sin cambios). fill_pattern apunta al dict
     * del patron ya resuelto (vive en dev->arena via el parser), no se
     * duplica aca. */
    pdf_obj   *fill_pattern;
    pdf_matrix fill_pattern_matrix;
    int        fill_cs_is_pattern; /* seteado por 'cs' con /Pattern, limpiado por rg/g/k */

    /* estado de texto que SI es parte del graphics state (se salva/
     * restaura con q/Q) -- la matriz de texto (Tm) NO, ver mas abajo. */
    pdf_font  *font;        /* NULL si no se selecciono con Tf todavia */
    double     font_size;
    double     char_space;  /* Tc */
    double     word_space;  /* Tw */
    double     h_scale;     /* Tz, ya normalizado a 1.0 = 100% */
    double     leading;     /* TL */
    double     rise;        /* Ts */
    int        render_mode; /* Tr */
} pdf_gstate;

typedef struct pdf_font_cache_entry_s
{
    char      name[PDF_FONT_NAME_MAX];
    pdf_font *font; /* puntero, no valor -- ver pdf_render_device_set_font_cache_hook:
                      * cuando el gancho de cache de documento esta activo, este
                      * puntero puede venir de ESE cache (vive mas alla de este
                      * render), no de una alocacion local del device. */
} pdf_font_cache_entry;

/* Gancho opcional de cache de fuentes A NIVEL DOCUMENTO (ver DESIGN.md,
 * seccion de rendimiento) -- 'pdf_render_device.font_cache' de mas
 * abajo es por-DEVICE (se tira al terminar cada render), asi que sin
 * este gancho cada llamada a Pdf_RenderToHBitmap volvia a parsear el
 * programa TrueType/CFF embebido de cada fuente usada, aunque fuera la
 * MISMA fuente que ya se cargo para una pagina anterior o un zoom
 * anterior. 'fdict' es el dict de fuente ya resuelto (puntero ESTABLE
 * entre llamadas gracias al cache de resolucion de objetos de
 * pdf_xref_load_object -- sin eso, cachear por puntero de dict no
 * serviria de nada, cada resolucion daria un puntero distinto). La
 * funcion debe devolver un 'pdf_font*' ya cargado (buscando en su
 * cache si existe, o llamando pdf_font_load() y guardando el
 * resultado si no) -- NULL si no se pudo cargar. 'user' se pasa tal
 * cual (mismo patron que pdf_glyph_draw_fn/pdf_text_extract_fn). */
typedef pdf_font *(*pdf_font_cache_fn)(void *user, pdf_obj *fdict,
                                        pdf_stream *st, const pdf_xref_table *xref,
                                        pdf_arena *arena);

/* Gancho opcional para reemplazar el dibujo de glyphs (por defecto: caja
 * placeholder, ver DESIGN.md seccion 11). Si esta seteado, se llama en
 * vez de dibujar la caja -- pensado para que el binding de Windows
 * dibuje el glyph de VERDAD via GDI (CreateFont+TextOut), reusando toda
 * la matematica de posicion/avance/kerning ya verificada aca.
 *
 * 'origin_x_px'/'origin_y_px': posicion del baseline del caracter, en
 * pixeles de destino (mismo espacio que pdf_bitmap, ya con el flip de
 * eje Y aplicado).
 * 'pixel_height': tamanio aproximado de la fuente en pixeles (no exacto
 * si el CTM tiene escala no uniforme o rotacion -- ver limitacion en
 * pdf_hbfunc.c).
 * 'advance_width_px': ancho que ESTE caracter debe ocupar segun /Widths
 * del PDF, en pixeles -- puede no coincidir con el ancho natural del
 * glyph en la fuente sustituta (si el PDF fue generado con una fuente
 * distinta a la instalada ahora, cosa muy comun). El backend deberia
 * usar esto para escalar horizontalmente el glyph y que la separacion
 * entre caracteres/palabras quede fiel al documento original, no al
 * ancho que "le parezca" a la fuente sustituta.
 * 'base_font_name': tal cual viene de /BaseFont (p.ej. "Arial-BoldMT"),
 * para que el backend elija la fuente del sistema a sustituir.
 * 'unicode': BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion
 * 55) -- 'code' es el codigo de caracter CRUDO del PDF (0-255), que
 * para fuentes con /Encoding estandar (WinAnsi, MacRoman, etc. -- el
 * caso normal) coincide con el codepoint ANSI real. Pero para fuentes
 * con subconjunto embebido SIN /Encoding (comun en PDFs generados por
 * PowerPoint, nombre con prefijo "AAAAAA+"), 'code' es un indice
 * arbitrario elegido por el generador para ESE subconjunto especifico
 * -- pasarlo directo a una API de texto ANSI busca glyphs en
 * posiciones que no tienen relacion con la letra real, resultando en
 * "tofu" (glyph faltante). 'unicode' es el codepoint Unicode REAL
 * para este caracter (resuelto via /ToUnicode si esta presente, o
 * simplemente igual a 'code' si no hay mapeo -- ver
 * pdf_font_get_unicode) -- el backend deberia preferir 'unicode' con
 * una API de texto Unicode (p.ej. TextOutW en vez de TextOutA) cuando
 * este disponible, para que el texto salga legible sin importar que
 * codificacion arbitraria use la fuente subconjunto original.
 * 'rotation_deg': BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md
 * seccion 61) -- antes este gancho NO llevaba ningun angulo, asi que
 * texto con una matriz Tm rotada (comun: el "lomo" vertical de
 * portadas de tesis/informes, con la matriz de texto tipo
 * "0 Fs -Fs 0 x y Tm") se posicionaba y escalaba bien (magnitud
 * correcta, ver 'pixel_height') pero se DIBUJABA SIN ROTAR -- el
 * backend GDI simplemente no tenia forma de saber que rotar. Angulo
 * en GRADOS, sentido ANTIHORARIO tal como se ve en pantalla (mismo
 * convenio que espera GDI en lfEscapement/lfOrientation), medido
 * sobre la direccion de AVANCE del texto (el eje X del espacio de
 * texto) proyectada a pixeles -- 0.0 para el caso normal (texto
 * horizontal, sin cambio de comportamiento).
 * 'is_serif': BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion
 * 65) -- cuando 'base_font_name' es un nombre generico sin ninguna
 * pista reconocible (p.ej. "CIDFont+F1", confirmado en un PDF real),
 * el backend no tiene forma de elegir una fuente sustituta razonable
 * por NOMBRE -- este flag (desde /FontDescriptor /Flags bit 2) da
 * una pista de respaldo: al menos elegir entre una familia serif
 * (Times New Roman) o sans-serif (Arial), en vez de un nombre
 * literal que casi seguro no existe como fuente instalada.
 */
typedef void (*pdf_glyph_draw_fn)(void *user, unsigned char code, int unicode,
                                   double origin_x_px, double origin_y_px,
                                   double pixel_height, double advance_width_px,
                                   double rotation_deg,
                                   int is_bold, int is_italic, int is_serif,
                                   pdf_color color, const char *base_font_name);

/* Gancho de extraccion de texto (fase 2 del roadmap de potencialidad
 * MuPDF -- extraccion/busqueda/copia, ver DESIGN.md seccion 70). A
 * diferencia de 'pdf_glyph_draw_fn' (que reemplaza el PINTADO y solo
 * se invoca cuando el caracter es visible), este gancho se llama
 * SIEMPRE, una vez por caracter mostrado via Tj/TJ/'/" -- incluido
 * texto invisible (Tr 3/7, comun en capas OCR sobre un escaneo) y
 * espacios -- porque la extraccion necesita "que hay ahi" sin
 * importar si se pinta o no. Puede coexistir con 'glyph_draw': un
 * mismo render puede pintar Y extraer texto en la misma pasada.
 * 'render_mode' es el Tr vigente al momento de este caracter, para que
 * el consumidor decida si le importa el texto invisible o no. */
typedef void (*pdf_text_extract_fn)(void *user, int unicode, int raw_code,
                                     double origin_x_px, double origin_y_px,
                                     double advance_width_px, double pixel_height,
                                     double rotation_deg, int render_mode);

typedef struct pdf_render_device_s
{
    pdf_bitmap  *bitmap;
    double       page_height_pt;
    double       page_width_pt; /* ver pdf_render_device_set_rotation -- solo
                          * hace falta para /Rotate 90/270 (necesita el
                          * ancho ademas del alto para invertir el eje X);
                          * 0 = "no seteado", los rotate 0/180 no lo usan. */
    double       scale; /* factor de puntos-PDF a pixeles (1.0 = 72 DPI,
                          * el tamanio "nativo" del PDF; 2.0-4.0 da mas
                          * resolucion real para fotos de imprenta -- ver
                          * pdf_render_device_init). El bitmap destino
                          * debe crearse ya con las dimensiones finales
                          * (page_w*scale x page_h*scale, ver ejemplo en
                          * examples/render_pdf_page.c). */
    int          rotate; /* /Rotate de la pagina (0/90/180/270, sentido
                          * horario "como se ve en pantalla" -- estandar
                          * PDF 32000-1 7.7.3.3), ver
                          * pdf_render_device_set_rotation. 0 = sin rotar
                          * (default, comportamiento de siempre). */
    double       crop_x0, crop_y0; /* origen de /CropBox (esquina inferior
                          * izquierda), en el MISMO espacio absoluto que
                          * usa el content stream -- BUG REAL ENCONTRADO
                          * (Arturo, PDF real con /CropBox bien distinto
                          * de /MediaBox: 842x595 contra 842x1190): el
                          * motor ignoraba /CropBox por completo,
                          * asumiendo SIEMPRE que la pagina visible
                          * arranca en (0,0) del content stream -- para
                          * paginas donde /CropBox no coincide con
                          * /MediaBox (recorte de imprenta, marcas de
                          * corte, etc.) esto corria/recortaba mal todo
                          * el contenido. 'page_height_pt'/'page_width_pt'
                          * arriba deben ser el tamanio del CROP (no del
                          * media), y crop_x0/crop_y0 el offset a restar
                          * ANTES de escalar/invertir Y -- default 0,0
                          * (si no hay /CropBox o coincide con /MediaBox
                          * en el origen, comportamiento identico a
                          * siempre). */
    pdf_gstate   gstate_stack[PDF_RENDER_MAX_GSTATE_DEPTH];
    int          gstate_top;

    /* CTM "de base" del stream de contenido actual (identidad para la
     * pagina; para dentro de un Form XObject, el CTM justo despues de
     * aplicar su /Matrix) -- los patrones de shading (PatternType 2,
     * ver DESIGN.md seccion 68) se definen en ESTE espacio, no en el
     * CTM vigente al momento de pintar (trampa clasica: un patron NO
     * se mueve si el path que lo usa se traslada con 'cm'). Se
     * salva/restaura alrededor de cada 'Do' de Form XObject en vez de
     * vivir en el gstate (no es parte del graphics state que se
     * salva/restaura con q/Q). */
    pdf_matrix   base_ctm;

    pdf_path     cur_path;

    /* matriz de texto y de linea (Tm/Tlm) -- estado de "objeto de
     * texto", NO forma parte del graphics state: se resetean en BT y no
     * se salvan/restauran con q/Q (asi lo exige el estandar). */
    pdf_matrix   text_matrix;
    pdf_matrix   line_matrix;

    /* para resolver /Font por nombre desde Tf */
    pdf_obj           *resources;
    pdf_stream         *st;
    const pdf_xref_table *xref;
    pdf_arena          *arena; /* tipicamente page->page_arena: vive mientras la pagina esta activa */

    pdf_font_cache_entry font_cache[PDF_RENDER_FONT_CACHE_SIZE];
    int                   n_fonts;

    pdf_glyph_draw_fn glyph_draw;      /* NULL = caja placeholder (default) */
    void              *glyph_draw_user;

    pdf_text_extract_fn text_extract;  /* NULL = no se extrae texto (default) */
    void                *text_extract_user;

    pdf_font_cache_fn font_cache_hook; /* NULL = sin cache de documento (default,
                                         * comportamiento identico a siempre: cada
                                         * fuente se parsea de cero en este render) */
    void              *font_cache_hook_user;

    int form_depth; /* guarda contra recursion infinita en Form XObjects
                      * (un form que se referencia a si mismo, o cadenas
                      * muy anidadas) -- ver 'Do' en pdf_render.c */

    int pending_clip;   /* 1 si hubo W/W* desde el ultimo path-paint: hay
                          * que intersectar gs->clip con el bbox del path
                          * actual ANTES de resetearlo (ver finish_path
                          * en pdf_render.c) */
    int pending_clip_evenodd; /* W* vs W -- no afecta el bbox pero se
                          * guarda por si se necesita en el futuro para
                          * clip no rectangular */
} pdf_render_device;

/* page_height_pt: alto de pagina en puntos PDF (para invertir el eje Y).
 * scale: factor puntos-PDF -> pixeles del bitmap destino (1.0 = tamanio
 * nativo/72 DPI; usar 2.0-4.0 para resolucion real de imprenta -- el
 * bitmap DEBE crearse ya del tamanio final escalado, este parametro
 * solo ajusta como pdf_render_op traduce coordenadas). 'resources' es
 * el /Resources de la pagina (ya resuelto, sin ref indirecta
 * pendiente) -- puede ser NULL si la pagina no tiene texto, en cuyo
 * caso Tf no encuentra fuentes y se usa el ancho generico. */
void pdf_render_device_init(pdf_render_device *dev, pdf_bitmap *bitmap,
                             double page_height_pt, double scale,
                             pdf_obj *resources,
                             pdf_stream *st, const pdf_xref_table *xref,
                             pdf_arena *arena);

/* Registra el gancho de dibujo de glyph real (opcional, llamar despues
 * de pdf_render_device_init). 'user' se pasa tal cual a cada llamada
 * de 'fn'. */
void pdf_render_device_set_glyph_hook(pdf_render_device *dev,
                                       pdf_glyph_draw_fn fn, void *user);

/* BUG REAL ENCONTRADO (Arturo: "las letras estan volteadas" -- pagina de
 * titulo de un PDF real con /Rotate 90): el motor NUNCA soporto /Rotate,
 * el atributo de pagina (heredable, ver pdf_page_get_inherited) que le
 * dice al visor que rote la pagina 0/90/180/270 grados en sentido
 * horario al MOSTRARLA -- se ignoraba por completo, asi que contenido
 * autoria do asumiendo esa rotacion (patron MUY comun: dibujar el
 * contenido en las coordenadas "naturales" y dejar que /Rotate lo gire,
 * en vez de rotar las coordenadas a mano en el content stream) salia
 * de costado/al reves.
 *
 * Llamar DESPUES de pdf_render_device_init, ANTES de correr el content
 * stream -- normaliza 'rotate_deg' al multiplo de 90 mas cercano modulo
 * 360 (0/90/180/270). El LLAMADOR sigue siendo responsable de: (1) leer
 * /Rotate como atributo heredable (pdf_page_get_inherited, no
 * pdf_dict_get directo -- normalmente vive en un nodo /Pages padre, no
 * en la pagina hoja), (2) crear el 'bitmap' de pdf_render_device_init
 * con ANCHO/ALTO ya INTERCAMBIADOS para 90/270 (el "canvas de salida"
 * cambia de orientacion), ya que esta funcion solo ajusta COMO
 * pdf_render_op traduce coordenadas, no el tamanio del bitmap en si
 * (ver pdf_hbfunc.c HB_FUNC(PDF_RENDERTOHBITMAP) para el patron
 * completo). Para 90/270 tambien hace falta pdf_render_device.page_width_pt
 * (setear directo, `dev->page_width_pt = ...`, ANTES de llamar aca) --
 * page_height_pt solo no alcanza para invertir el eje X. */
void pdf_render_device_set_rotation(pdf_render_device *dev, int rotate_deg);

/* Normaliza un valor crudo de /Rotate (puede venir negativo, mayor a
 * 360, o no ser exactamente multiplo de 90 en un PDF malformado) al
 * multiplo de 90 mas cercano modulo 360 (0/90/180/270) -- misma logica
 * que usa pdf_render_device_set_rotation internamente, expuesta aca
 * para que el LLAMADOR (pdf_hbfunc.c, pdf_text_extract.c) decida con el
 * mismo criterio si el bitmap de salida necesita ancho/alto
 * intercambiados (90/270) ANTES de crear el bitmap -- si el llamador
 * usara el valor crudo sin normalizar para esa decision y
 * pdf_render_device_set_rotation normalizara distinto, el bitmap
 * quedaria con las dimensiones equivocadas para la rotacion que
 * realmente se aplica (salida recortada/corrupta). */
int pdf_normalize_rotation(int rotate_deg);

/* Registra el gancho de extraccion de texto (opcional, llamar despues
 * de pdf_render_device_init, puede coexistir con el de dibujo de
 * glyph). 'user' se pasa tal cual a cada llamada de 'fn'. */
void pdf_render_device_set_text_extract_hook(pdf_render_device *dev,
                                              pdf_text_extract_fn fn, void *user);

/* Registra el gancho de cache de fuentes a nivel documento (opcional,
 * llamar despues de pdf_render_device_init) -- ver pdf_font_cache_fn
 * arriba. Sin esto, cada fuente usada en la pagina se vuelve a
 * parsear (pdf_font_load) en cada render, sin persistir entre
 * llamadas -- el comportamiento de siempre. */
void pdf_render_device_set_font_cache_hook(pdf_render_device *dev,
                                            pdf_font_cache_fn fn, void *user);

/* Compatible con pdf_content_op_fn -- pasar como ops.op y dev como
 * ops.user al llamar pdf_content_run. */
void pdf_render_op(void *user, const char *opname, pdf_obj **args, int nargs);

/* AcroForm: dibuja la apariencia actual (/AP/N) de cada campo Widget
 * de 'page_obj' -- llamar DESPUES de correr el content stream de la
 * pagina (pdf_content_run), nunca antes (la norma exige que las
 * anotaciones se pinten encima del contenido de pagina). 'dev' debe
 * estar ya inicializado (pdf_render_device_init) con el mismo
 * st/xref/arena/bitmap que se uso para la pagina -- esta funcion
 * resetea dev->gstate_top a 0 y arma un gstate limpio por cada campo,
 * asi que no importa en que estado haya quedado el gstate stack tras
 * el content stream. Ver DESIGN.md (fase AcroForm) y pdf_form.h. */
void pdf_render_draw_annotations(pdf_render_device *dev, pdf_obj *page_obj);

/* Igual que pdf_render_draw_annotations() pero para anotaciones
 * /Subtype /Highlight (resaltado de texto, ver pdf_annot.h/DESIGN.md)
 * -- recorre /Annots directo (no via pdf_form_list_fields, que es
 * especifico de AcroForm), dibuja la apariencia (/AP/N) de cada
 * Highlight que la tenga (se saltan, sin error, los que no -- ver
 * limitacion de alcance en DESIGN.md), respetando /F Hidden/NoView
 * igual que el camino de Widgets. Llamar junto con
 * pdf_render_draw_annotations(), mismo momento (despues del content
 * stream de la pagina). */
void pdf_render_draw_highlight_annotations(pdf_render_device *dev, pdf_obj *page_obj);

/* Ver comentario grande junto a la implementacion, pdf_render.c --
 * convierte un punto del espacio "topdown" que usa la extraccion de
 * texto/seleccion (pdf_text_extract_page(), aSelRanges en
 * pdf_viewer.prg) de vuelta al espacio de usuario PDF NATIVO
 * (bottom-up, sin rotar) que exige la norma para /Rect y /QuadPoints.
 * 'rotate_deg' es el /Rotate NATIVO heredado de la pagina -- NUNCA
 * ::nUserRotate. Devuelve 0 (sin tocar las salidas) si la matriz
 * resultara degenerada. */
int pdf_render_topdown_to_native(int rotate_deg,
                                  double crop_x0, double crop_y0,
                                  double page_w, double page_h,
                                  double topdown_x, double topdown_y,
                                  double *out_x, double *out_y);

#ifdef __cplusplus
}
#endif

#endif /* PDF_RENDER_H */
