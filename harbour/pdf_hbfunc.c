/* pdf_hbfunc.c
 *
 * ============================================================================
 * ADVERTENCIA IMPORTANTE
 * ============================================================================
 * Este archivo NO pudo compilarse ni probarse en el entorno donde se generó
 * (no hay hbapi.h, hbapiitm.h, FiveWin.ch ni windows.h disponibles aca --
 * es un sandbox Linux sin Harbour/FiveWin/Windows instalados). A diferencia
 * del resto del motor (pdf_mem, pdf_parser, pdf_filter, pdf_content,
 * pdf_render), que SI se compilo y se corrio contra PDFs reales, esta capa
 * de binding esta escrita de memoria siguiendo los patrones estandar de la
 * API C de Harbour y de GDI de Windows. Es MUY probable que haga falta
 * ajustar algo al compilar por primera vez (nombres exactos de funciones
 * hb_arraySetPtr/hb_arraySetNL segun tu version de Harbour, por ejemplo) --
 * avisame los errores tal cual salen y los corrijo, como hicimos con el
 * warning de precedencia en pdf_filter.c.
 * ============================================================================
 *
 * Expone PDFEngine32 a Harbour/FiveWin:
 *
 *   PDF_OPEN( cArchivo, nBudgetMB )        -> pDoc (puntero opaco) o NIL
 *   PDF_CLOSE( pDoc )                       -> NIL
 *   PDF_PAGECOUNT( pDoc )                   -> nPaginas
 *   PDF_RENDERTOHBITMAP( pDoc, nPagina, nScale, nExtraRotate )    -> { hBitmap, nWidth, nHeight } o NIL
 *      (nScale y nExtraRotate opcionales, default 0 -- nExtraRotate se SUMA
 *      al /Rotate propio del PDF, en grados multiplo de 90)
 *   PDF_GETPAGESIZEPT( pDoc, nPagina, nExtraRotate ) -> { anchoPt, altoPt } o NIL
 *      (tamanio de pagina en puntos PDF, MISMO calculo que
 *      PDF_RENDERTOHBITMAP pero SIN decodificar/rasterizar nada --
 *      para cuando solo hace falta saber el tamanio, ver
 *      METHOD RenderCurrentPage() en pdf_viewer.prg)
 *
 * PDF_RENDERTOHBITMAP hace TODO el trabajo (abrir pagina, decodificar el
 * content stream, interpretar, rasterizar) y devuelve un HBITMAP de Windows
 * ya listo para asignar a un TBitmap/TImage de FiveWin -- la memoria de
 * trabajo (page_arena/decode_arena) se libera antes de retornar; el HBITMAP
 * resultante es independiente (GDI tiene su propia copia de los pixeles).
 *
 * El texto se dibuja con GDI de verdad (CreateFont+TextOut, sustituyendo
 * por la fuente del sistema segun /BaseFont), no como caja placeholder --
 * ver pdf_gdi_draw_glyph() mas abajo. Esto reemplaza el fallback de
 * pdf_render.c via el gancho pdf_render_device_set_glyph_hook().
 *
 * La resolucion de pagina usa pdf_document_get_page() del motor, que
 * recorre el arbol /Pages completo de forma recursiva (arreglado tras
 * probar contra planos reales con arbol anidado -- ver DESIGN.md).
 */

#include "hbapi.h"
#include "hbapiitm.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pdf.h"
#include "pdf_text_extract.h"
#include "pdf_form.h"
#include "pdf_write.h"
#include "pdf_annot.h"

/* Cache de texto extraido (fase 2 del roadmap de potencialidad MuPDF --
 * ver DESIGN.md seccion 70): PDF_EXTRACTTEXT/PDF_FINDTEXT/
 * PDF_GLYPHSINRECT/PDF_GETGLYPHTEXT abajo pueden dispararse muy seguido
 * (cada MouseMove de un arrastre de seleccion, cada "buscar siguiente")
 * -- volver a correr el content stream completo de la pagina en cada
 * llamada seria caro. Slots fijos (round-robin, sin LRU real) indexados
 * por numero de pagina 0-based. Cada slot tiene su propio arreglo de
 * glyphs de capacidad fija (PDF_HB_TEXT_MAX_GLYPHS) -- igual criterio
 * que pdf_path.h: si una pagina tiene mas texto que eso, se trunca
 * silenciosamente (ver pdf_text_extract_page), no crashea.
 *
 * Subido de 4 a 12 (fase de vista continua, DESIGN.md seccion 70.1):
 * un arrastre de seleccion que cruza varias paginas del compuesto
 * (TPdfBitmap:MouseMove, pdf_viewer.prg) puede tocar mas de 4 paginas
 * distintas en un solo gesto -- con 4 slots, desalojaba y volvia a
 * extraer el content stream de las mismas paginas en cada pixel de
 * movimiento del mouse (stutter visible). 12 cubre arrastres largos sin
 * gastar memoria relevante (cada slot es un arreglo fijo, no crece con
 * el contenido real de la pagina). */
#define PDF_HB_TEXT_CACHE_SLOTS 12
#define PDF_HB_TEXT_MAX_GLYPHS  8192

typedef struct
{
    int             page_index; /* -1 = slot vacio/invalido */
    int             n_glyphs;
    double          page_width_pt, page_height_pt;
    pdf_text_glyph  glyphs[PDF_HB_TEXT_MAX_GLYPHS];
} pdf_hb_text_slot;

/* AcroForm (ver pdf_form.h/DESIGN.md): lista chica de objetos "tocados"
 * (widgets con /V o /AP mutados en memoria, mas cualquier stream de
 * apariencia nuevo generado por una edicion de texto) -- el escritor
 * (pdf_write.c) recorre esta lista al guardar, sin necesidad de que el
 * llamador Harbour retenga nada mas que obj_num/obj_gen. Dedup simple
 * por obj_num (lista chica, un scan lineal no importa). */
#define PDF_HB_MAX_TOUCHED_OBJS 128

typedef struct
{
    long obj_num, obj_gen;
    pdf_obj *obj_ptr; /* apunta al objeto YA MUTADO en doc_arena -- necesario
                        * para los streams sinteticos (apariencias generadas),
                        * que NO estan registrados en la tabla xref y por lo
                        * tanto no se pueden re-resolver por numero via
                        * pdf_xref_load_object al guardar (ver pdf_write.c). */
} pdf_hb_touched_obj;

/* Cache de fuentes ya parseadas, a nivel DOCUMENTO (ver DESIGN.md,
 * seccion de rendimiento) -- clave: el puntero del dict de fuente
 * (ESTABLE entre llamadas gracias al cache de resolucion de objetos
 * de pdf_xref_load_object, ver pdf_xref.c/.h -- sin eso, cachear por
 * puntero no serviria: cada resolucion daria un puntero nuevo). Sin
 * este cache, cada Pdf_RenderToHBitmap volvia a parsear el programa
 * TrueType/CFF embebido de cada fuente usada en la pagina, aunque
 * fuera la MISMA fuente que ya se cargo para una pagina/zoom anterior
 * -- el costo dominante detras de "el zoom es lento" en documentos con
 * texto. Arreglo fijo + scan lineal, mismo estilo que
 * pdf_hb_text_slot/PDF_HB_TEXT_CACHE_SLOTS de arriba (no la
 * maquinaria LRU mas compleja de pdf_cache.c, que ademas necesitaria
 * una free_fn para la estructura interna de pdf_font). */
#define PDF_HB_MAX_CACHED_FONTS 64

typedef struct
{
    pdf_obj  *fdict; /* NULL = slot vacio */
    pdf_font *font;
} pdf_hb_font_slot;

typedef struct
{
    pdf_stream      st;
    pdf_document    doc;
    pdf_xref_table  xref;
    int             open;

    pdf_hb_text_slot text_cache[PDF_HB_TEXT_CACHE_SLOTS];
    int               text_cache_next;

    long                next_new_obj_num; /* primer numero de objeto libre,
                                            * para apariencias regeneradas */
    pdf_hb_touched_obj  touched[PDF_HB_MAX_TOUCHED_OBJS];
    int                 n_touched;

    pdf_hb_font_slot    font_cache[PDF_HB_MAX_CACHED_FONTS];
    int                 n_cached_fonts;
} pdf_hb_doc;

/* pdf_font_cache_fn (ver pdf_render.h) -- 'user' es el pdf_hb_doc*.
 * Busca por puntero de dict; si no esta, carga con pdf_font_load()
 * (arena SIEMPRE doc.doc_arena, nunca la 'arena' de vida corta que
 * pasa el llamador -- el resultado tiene que sobrevivir mas alla de
 * ESTE render) y lo agrega al cache. Si el cache esta lleno, sigue
 * funcionando (carga sin persistir), solo no gana la aceleracion para
 * esa fuente puntual -- no crashea ni se degrada de otra forma. */
static pdf_font *hb_doc_font_cache_fn(void *user, pdf_obj *fdict,
                                       pdf_stream *st, const pdf_xref_table *xref,
                                       pdf_arena *arena)
{
    pdf_hb_doc *h = (pdf_hb_doc *)user;
    int i;
    pdf_font *font;

    (void)arena; /* se ignora a proposito -- ver comentario arriba */

    if (h == NULL || fdict == NULL) return NULL;

    for (i = 0; i < h->n_cached_fonts; i++)
        if (h->font_cache[i].fdict == fdict)
            return h->font_cache[i].font;

    font = (pdf_font *)pdf_arena_alloc(&h->doc.doc_arena, sizeof(pdf_font));
    if (font == NULL) return NULL;
    if (pdf_font_load(fdict, font, st, xref, &h->doc.doc_arena) != PDF_OK)
        return NULL;

    if (h->n_cached_fonts < PDF_HB_MAX_CACHED_FONTS)
    {
        h->font_cache[h->n_cached_fonts].fdict = fdict;
        h->font_cache[h->n_cached_fonts].font  = font;
        h->n_cached_fonts++;
    }

    return font;
}

static void mark_touched(pdf_hb_doc *h, long obj_num, long obj_gen, pdf_obj *obj_ptr)
{
    int i;
    if (h == NULL || obj_num <= 0 || obj_ptr == NULL) return;
    for (i = 0; i < h->n_touched; i++)
        if (h->touched[i].obj_num == obj_num)
        {
            h->touched[i].obj_ptr = obj_ptr; /* refrescar -- re-editar el mismo campo */
            return;
        }
    if (h->n_touched < PDF_HB_MAX_TOUCHED_OBJS)
    {
        h->touched[h->n_touched].obj_num = obj_num;
        h->touched[h->n_touched].obj_gen = obj_gen;
        h->touched[h->n_touched].obj_ptr = obj_ptr;
        h->n_touched++;
    }
}

static double obj_num(pdf_obj *o)
{
    if (o == NULL) return 0.0;
    if (o->type == PDF_INT)  return (double)o->u.integer;
    if (o->type == PDF_REAL) return o->u.real;
    return 0.0;
}

/* La resolucion de pagina ahora usa pdf_document_get_page() del motor
 * (src/page/pdf_page.c), que recorre el arbol /Pages COMPLETO de forma
 * recursiva -- la version anterior de esta funcion (un solo nivel de
 * /Kids) fallaba en PDFs reales con arbol de paginas anidado, como se
 * confirmo probando contra planos reales (ver DESIGN.md seccion 10). */

/* Convierte un pdf_bitmap (RGB24, sin padding de fila) a un HBITMAP de
 * Windows de 24bpp top-down (BGR, con padding de fila a multiplo de 4,
 * como exige GDI). Copia los pixeles: el HBITMAP resultante es
 * independiente de la arena de origen. Devuelve NULL si CreateDIBSection
 * falla. */
static HBITMAP pdf_bitmap_to_hbitmap(const pdf_bitmap *bmp)
{
    BITMAPINFO bmi;
    HDC hdc;
    HBITMAP hBmp;
    void *bits;
    int x, y;
    int dst_stride;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = bmp->width;
    bmi.bmiHeader.biHeight      = -bmp->height; /* negativo = top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc  = GetDC(NULL);
    hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);

    if (hBmp == NULL || bits == NULL)
        return NULL;

    dst_stride = ((bmp->width * 3 + 3) / 4) * 4; /* alineacion de fila a 4 bytes */

    for (y = 0; y < bmp->height; y++)
    {
        const unsigned char *srow = bmp->pixels + (long)y * bmp->stride;
        unsigned char *drow = (unsigned char *)bits + (long)y * dst_stride;
        for (x = 0; x < bmp->width; x++)
        {
            drow[x * 3 + 0] = srow[x * 3 + 2]; /* B */
            drow[x * 3 + 1] = srow[x * 3 + 1]; /* G */
            drow[x * 3 + 2] = srow[x * 3 + 0]; /* R */
        }
    }

    return hBmp;
}

/* ============================================================================
 * Texto real via GDI (reemplaza la caja placeholder de pdf_render.c)
 * ============================================================================
 * NO PROBADO EN ESTE ENTORNO (mismo motivo que el resto del binding --
 * ver advertencia al inicio del archivo). Sigue el patron estandar de
 * GDI: un DC de memoria chico (256x256, ver PDF_GDI_GLYPH_BMP_SIZE)
 * reutilizado para cada glyph, texto dibujado con
 * SetBkMode(TRANSPARENT) sobre fondo blanco conocido, y despues se
 * copian de vuelta a pdf_bitmap solo los pixeles que dejaron de ser
 * blancos -- asi se preserva el antialiasing de GDI (ClearType/grises)
 * en vez de quedar binario.
 *
 * LIMITACION CONOCIDA: no soporta rotacion/shear del CTM (asume texto
 * horizontal, el caso comun en documentos de ingenieria/oficina). Si el
 * 'cm' vigente tiene componentes b/c no nulos, el texto sale mal
 * orientado -- no hay chequeo defensivo para esto todavia.
 */

#define PDF_GDI_GLYPH_BMP_SIZE 256  /* antes 128 -- se agrando para dar
                                      * margen a factores de escala de
                                      * pagina >1x (ver 'scale' en
                                      * PDF_RENDERTOHBITMAP, resolucion
                                      * real de fotos de imprenta): con
                                      * escala de pagina 3-4x, titulos
                                      * grandes (18-24pt) pueden superar
                                      * los 100px de alto, y con 128 se
                                      * recortaban. No tiene relacion con
                                      * sobremuestreo (se probo y se saco,
                                      * ver DESIGN.md secciones 25-26 --
                                      * producia letras mal formadas). */
#define PDF_GDI_GLYPH_MARGIN   24   /* margen para que el glyph no se corte contra el borde del DC chico */

typedef struct
{
    HDC     hdc;
    HBITMAP hbmp;
    void   *bits;         /* puntero directo a los pixeles del DIB (BGR, top-down, sin padding porque PDF_GDI_GLYPH_BMP_SIZE es multiplo de 4) -- YA NO SE LEE DIRECTO, ver 'readback' */
    unsigned char *readback; /* buffer separado donde se copian los pixeles
                          * via GetDIBits (sincronizado de verdad por GDI,
                          * a diferencia de leer 'bits' directo con un
                          * simple GdiFlush() -- ver DESIGN.md seccion 34).
                          * Mismo tamanio que el DIB: PDF_GDI_GLYPH_BMP_SIZE
                          * al cuadrado x 3 (BGR24), alocado una sola vez. */
    HFONT   cur_font;
    int     cur_height;
    int     cur_bold;
    int     cur_italic;
    int     cur_escapement; /* angulo actual de la fuente seleccionada, en decimas de grado (lfEscapement/lfOrientation) -- ver DESIGN.md seccion 61 */
    int     cur_is_serif;   /* ver DESIGN.md seccion 65 */
    char    cur_name[PDF_FONT_NAME_MAX];
    pdf_bitmap *dst;       /* pdf_bitmap de destino donde se compone el resultado */
} pdf_gdi_text_ctx;

static int pdf_gdi_text_ctx_init(pdf_gdi_text_ctx *ctx, pdf_bitmap *dst)
{
    BITMAPINFO bmi;
    HDC screen_dc;

    memset(ctx, 0, sizeof(*ctx));
    ctx->dst = dst;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = PDF_GDI_GLYPH_BMP_SIZE;
    bmi.bmiHeader.biHeight      = -PDF_GDI_GLYPH_BMP_SIZE; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    screen_dc = GetDC(NULL);
    ctx->hdc  = CreateCompatibleDC(screen_dc);
    ctx->hbmp = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &ctx->bits, NULL, 0);
    ReleaseDC(NULL, screen_dc);

    if (ctx->hdc == NULL || ctx->hbmp == NULL || ctx->bits == NULL)
        return 0;

    ctx->readback = (unsigned char *)malloc((size_t)PDF_GDI_GLYPH_BMP_SIZE * PDF_GDI_GLYPH_BMP_SIZE * 3);
    if (ctx->readback == NULL)
        return 0;

    SelectObject(ctx->hdc, ctx->hbmp);
    SetBkMode(ctx->hdc, TRANSPARENT);

    return 1;
}

static void pdf_gdi_text_ctx_destroy(pdf_gdi_text_ctx *ctx)
{
    if (ctx->cur_font != NULL) DeleteObject(ctx->cur_font);
    if (ctx->hbmp     != NULL) DeleteObject(ctx->hbmp);
    if (ctx->hdc      != NULL) DeleteDC(ctx->hdc);
    if (ctx->readback != NULL) free(ctx->readback);
    memset(ctx, 0, sizeof(*ctx));
}

/* Limpia un BaseFont de PDF (p.ej. "Arial-BoldMT", "TimesNewRomanPSMT",
 * "ABCDEF+Calibri-Bold") a un nombre de familia que Windows realmente
 * reconoce (p.ej. "Arial", "Times New Roman", "Calibri"). Escribe el
 * resultado en 'out' (buffer de al menos 64 bytes).
 *
 * BUG REAL ENCONTRADO (ver DESIGN.md): CreateFontA recibia el BaseFont
 * del PDF tal cual, sin limpiar el sufijo de estilo. "Arial-BoldMT" NO
 * es un nombre de familia de Windows -- Windows conoce la familia como
 * "Arial" nada mas, con negrita/cursiva como ATRIBUTOS DE ESTILO, no
 * parte del nombre de familia. Al pedirle a GDI una fuente con un
 * nombre que no existe exactamente, combinado con DEFAULT_CHARSET y
 * FF_DONTCARE (los criterios de fallback mas laxos posibles), el
 * mapeo de reemplazo de GDI es impredecible -- puede terminar
 * eligiendo cualquier fuente instalada, incluida una decorativa o de
 * simbolos, dando glyphs que no tienen nada que ver con el caracter
 * pedido (confirmado real: "BOLIVIA" salia como "ECLIVIA", cambios de
 * letra reconocibles, no solo mal formadas). Esto es INDEPENDIENTE de
 * los intentos anteriores con MM_ANISOTROPIC (secciones 25-26,
 * revertidos) -- probablemente esta fue la causa real todo este
 * tiempo, y los intentos de ajustar escala nunca iban a arreglarlo
 * porque el problema no era el ancho sino la fuente elegida. */
/* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 65): antes,
 * un nombre no reconocido se devolvia "tal cual" (limpio de prefijo
 * de subset y sufijos comunes), confiando en que GDI encontrara ALGO
 * razonable por substring -- pero si el nombre es genuinamente
 * generico y sin sentido (p.ej. "F1", el resultado de limpiar
 * "CIDFont+F1", confirmado en un PDF real), GDI no encuentra ninguna
 * fuente instalada con ese nombre y cae a lo que sea que tenga por
 * defecto (tipicamente sans-serif) sin importar si el documento
 * necesitaba serif -- un desajuste de proporciones de letra mucho
 * mayor que el esperable entre fuentes similares, causando un
 * escalado horizontal por glyph extremo (confirmado: valores de
 * 0.78 a 1.56 en un caso real, contra 0.84-0.98 tipico cuando la
 * sustitucion es razonable) y el aspecto visual "sucio"/no fino
 * reportado. 'is_serif' (desde /FontDescriptor /Flags, ver
 * DESIGN.md) se usa como red de seguridad: si el nombre NO coincidio
 * con ningun patron conocido, se elige "Times New Roman" o "Arial"
 * como sustituto por defecto segun ese flag, en vez de un nombre que
 * casi seguro no existe como fuente instalada.
 * Devuelve 1 si encontro un nombre conocido por PATRON de texto
 * (mayor confianza), 0 si tuvo que usar el fallback generico/serif. */
static int pdf_gdi_clean_face_name(const char *name, char *out, int out_size, int is_serif)
{
    const char *plus, *p;
    int i, n;
    char tmp[128];

    plus = strchr(name, '+');
    p = (plus != NULL) ? plus + 1 : name; /* quitar prefijo de subset "ABCDEF+" */

    /* nombres conocidos: comparar por prefijo (case-insensitive) contra
     * la familia real de Windows. Cubre la enorme mayoria de fuentes
     * que aparecen en documentos de oficina/ingenieria reales. */
    {
        static const struct { const char *prefix; const char *family; } known[] = {
            { "ArialBlack",      "Arial Black" },
            { "Arial",           "Arial" },
            { "TimesNewRoman",   "Times New Roman" },
            { "Times",           "Times New Roman" },
            { "CourierNew",      "Courier New" },
            { "Courier",         "Courier New" },
            { "Calibri",         "Calibri" },
            { "Cambria",         "Cambria" },
            { "Verdana",         "Verdana" },
            { "Tahoma",          "Tahoma" },
            { "Georgia",         "Georgia" },
            { "ComicSans",       "Comic Sans MS" },
            { "SegoeUI",         "Segoe UI" },
            { "Segoe",           "Segoe UI" }
        };
        int k;
        for (k = 0; k < (int)(sizeof(known) / sizeof(known[0])); k++)
        {
            int plen = (int)strlen(known[k].prefix);
            if (_strnicmp(p, known[k].prefix, plen) == 0)
            {
                strncpy(out, known[k].family, out_size - 1);
                out[out_size - 1] = 0;
                return 1;
            }
        }
    }

    /* nombre desconocido: limpieza generica -- cortar en el primer
     * separador de estilo ('-' o ',', cubre "Xxx-Bold", "Xxx,Italic")
     * y sacar sufijos comunes de fuentes PostScript ("PSMT", "MT", "PS"
     * pegados al final sin separador, comun en fuentes TrueType
     * embebidas exportadas desde Mac/PostScript). */
    n = 0;
    for (i = 0; p[i] != '\0' && n < (int)sizeof(tmp) - 1; i++)
    {
        if (p[i] == '-' || p[i] == ',') break;
        tmp[n++] = p[i];
    }
    tmp[n] = '\0';

    /* sufijos pegados sin separador: revisar de mayor a menor longitud */
    {
        static const char *suffixes[] = { "PSMT", "MT", "PS" };
        int k;
        for (k = 0; k < 3; k++)
        {
            int slen = (int)strlen(suffixes[k]);
            int tlen = (int)strlen(tmp);
            if (tlen > slen && _stricmp(tmp + tlen - slen, suffixes[k]) == 0)
            {
                tmp[tlen - slen] = '\0';
                break;
            }
        }
    }

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 65): un nombre
     * "limpio" resultante MUY corto (1-2 caracteres, como "F1"->"F")
     * es casi con certeza un identificador generico sin sentido
     * (subconjuntos que no preservan el nombre real de la fuente),
     * no el nombre real abreviado de ninguna familia tipografica
     * real -- usar el fallback de is_serif en vez de pasarselo tal
     * cual a GDI. Nombres mas largos (3+ caracteres) SI se dejan
     * pasar tal cual (mejor esfuerzo -- GDI a veces encuentra algo
     * razonable por substring incluso sin estar en la lista
     * 'known[]' de arriba). */
    if ((int)strlen(tmp) < 3)
    {
        strncpy(out, is_serif ? "Times New Roman" : "Arial", out_size - 1);
        out[out_size - 1] = 0;
        return 0;
    }

    strncpy(out, tmp, out_size - 1);
    out[out_size - 1] = 0;
    return 0;
}

static void pdf_gdi_ensure_font(pdf_gdi_text_ctx *ctx, int height,
                                 int bold, int italic, const char *name,
                                 int escapement_deci_deg, int is_serif)
{
    if (ctx->cur_font != NULL && ctx->cur_height == height &&
        ctx->cur_bold == bold && ctx->cur_italic == italic &&
        ctx->cur_escapement == escapement_deci_deg &&
        ctx->cur_is_serif == is_serif &&
        strcmp(ctx->cur_name, name) == 0)
        return; /* ya esta seleccionada la fuente correcta */

    if (ctx->cur_font != NULL)
        DeleteObject(ctx->cur_font);

    {
        char face[64];
        int charset;
        pdf_gdi_clean_face_name(name, face, sizeof(face), is_serif);

        /* Fuentes de simbolos/dingbats (Wingdings, Webdings, Symbol):
         * Windows NECESITA que se pidan con SYMBOL_CHARSET, no
         * DEFAULT_CHARSET -- sin esto, el mapeo de codigo de caracter
         * a glyph de simbolo puede salir mal (el bug real reportado:
         * "Wingdings" no se identifica correctamente). Encontrado real
         * en Flujograma_Propuesto.pdf, que usa
         * "ABCDEF+Wingdings-Regular" (seguramente un icono, viñeta o
         * casilla en algun lado del documento). */
        if (_stricmp(face, "Wingdings") == 0 ||
            _stricmp(face, "Webdings") == 0 ||
            _stricmp(face, "Symbol") == 0)
            charset = SYMBOL_CHARSET;
        else
            charset = DEFAULT_CHARSET;

        /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 61):
         * lfEscapement/lfOrientation (los parametros 5 y 6 de
         * CreateFontA, "0, 0" hardcodeados antes) son los que le dicen
         * a GDI que rote el texto -- sin esto, una matriz Tm rotada
         * (el "lomo" vertical de una portada de tesis, por ejemplo) se
         * posicionaba bien pero se dibujaba siempre horizontal. Ambos
         * campos deben llevar el MISMO angulo (en decimas de grado,
         * antihorario) para que el texto rote Y se oriente junto. */
        ctx->cur_font = CreateFontA(
            -height, 0, escapement_deci_deg, escapement_deci_deg,
            bold ? FW_BOLD : FW_NORMAL,
            italic ? TRUE : FALSE,
            FALSE, FALSE,
            charset, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            (face[0] != '\0') ? face : "Arial");

        SelectObject(ctx->hdc, ctx->cur_font);
    }

    ctx->cur_height = height;
    ctx->cur_bold   = bold;
    ctx->cur_italic = italic;
    ctx->cur_escapement = escapement_deci_deg;
    ctx->cur_is_serif = is_serif;
    strncpy(ctx->cur_name, name, PDF_FONT_NAME_MAX - 1);
    ctx->cur_name[PDF_FONT_NAME_MAX - 1] = 0;
}

/* Callback compatible con pdf_glyph_draw_fn (pdf_render.h).
 *
 * HISTORIAL (ver DESIGN.md secciones 25-26, 30-33 para el detalle
 * completo): se intento dos veces ajustar el ancho de cada glyph via
 * MM_ANISOTROPIC, sin resultado (peor: corrompia la forma de las
 * letras). Se saco esa logica por completo (seccion 30) y el problema
 * persistio identico, lo que probo que el ajuste de ancho NUNCA fue
 * la causa real. Se agrego un log de diagnostico temporal (seccion
 * 32) que confirmo con datos reales: el nombre de fuente que Windows
 * selecciona es exactamente el pedido (Arial/Calibri, sin sustitucion
 * rara), y por separado, trazando el motor central (que SI se puede
 * probar sin Windows), se confirmo que el codigo de caracter que
 * llega a este callback tambien es siempre el correcto ('B','O','L',
 * 'I','V','I','A' en orden para "BOLIVIA", etc). Con la fuente Y el
 * caracter descartados, se probo un GdiFlush() antes de leer los
 * pixeles (hipotesis: GDI puede diferir/batch el dibujo, y leer el
 * puntero del DIB section a mano sin sincronizar podria devolver
 * restos del glyph anterior superpuestos) -- tampoco alcanzo. Se
 * comparo entonces contra HBPDF (otro motor PDF propio, mas maduro en
 * el manejo de fuentes) y se encontro la diferencia clave: acá se
 * leia el puntero del DIB section DIRECTO, mientras que el patron mas
 * robusto es usar GetDIBits (sincroniza de verdad). Arturo confirmo
 * que con GetDIBits las letras ya NO salen deformadas -- eso resolvio
 * la corrupcion de forma, pero el ESPACIADO entre letras seguia
 * inconsistente (huecos tipo "PREPARACI ON", distinto espacio entre
 * letras, sin continuidad).
 *
 * Se habia reintroducido el ajuste de ancho via MM_ANISOTROPIC
 * (manipular el modo de mapeo del DC ANTES de TextOutA) para atacar
 * eso, pero Arturo confirmo que no mejoro nada. Se probo despues
 * estirar en software (seccion 40) con interpolacion lineal (seccion
 * 41) -- mejoro la nitidez pero Arturo siguio reportando problemas de
 * espaciado y calidad.
 *
 * Arturo compartio el codigo fuente real de HBPDF (pdf_truetype.c: un
 * parser TrueType propio con lookup de glyph index via cmap) y el
 * header de su renderer GDI (pdf_render_gdi.h). Aunque no se vio el
 * .c del renderer en si, la combinacion de un parser TrueType con
 * lookup de GID + el patron tipico para este tipo de renderer sugiere
 * fuertemente que HBPDF usa ExtTextOutW con su parametro nativo
 * 'lpDx' (array de anchos exactos por caracter que GDI soporta
 * directamente) -- es decir, NO distorsiona cada glyph individual,
 * solo le dice a GDI donde debe arrancar el siguiente caracter,
 * dejando que cada glyph se dibuje a su forma natural sin ningun
 * estiramiento.
 *
 * Esto es exactamente lo que ya se habia verificado por otro lado
 * (seccion 32): la posicion de arranque de cada caracter
 * (origin_x_px) que le llega a este callback YA es la correcta,
 * calculada por el motor central directo de los anchos reales
 * declarados en el PDF -- nunca hizo falta forzar el ANCHO de cada
 * glyph a un valor exacto, alcanza con dibujarlo a su tamanio
 * NATURAL en la posicion ya correcta. Se saco entonces TODO el
 * mecanismo de estiramiento (MM_ANISOTROPIC y el estiramiento en
 * software) y se volvio a dibujo natural puro -- mismo principio de
 * fondo que HBPDF, aplicado con las herramientas mas simples posibles
 * dado que este renderer sigue dibujando un caracter por vez (no
 * strings completos como ExtTextOutW permitiria) por como esta
 * estructurado el resto del motor (ver seccion 42).
 *
 * Arturo confirmo mejora real con el dibujo 100% natural, pero
 * reporto un problema nuevo: sin NINGUN ajuste de ancho, la
 * diferencia natural entre lo que GDI dibuja y lo que el PDF espera
 * (mas marcada por el kerning agresivo caracter-por-caracter de este
 * documento) hace que algunas letras se toquen/superpongan y otras
 * queden con hueco de mas. Se probaron sucesivamente: un rango
 * angosto simetrico (0.8-1.25, seccion 43), un tope duro contra
 * toques (seccion 44), y un rango asimetrico mas amplio (0.8-3.0,
 * seccion 45) -- Arturo siguio reportando huecos "al azar".
 *
 * Se verifico entonces con DATOS REALES (fontTools + Liberation Sans
 * Bold, fuente metricamente compatible con Arial Bold, comparando
 * contra el /Widths real declarado en Flujograma_Propuesto.pdf y el
 * kerning real extraido de su TJ) que el /Widths del PDF coincide con
 * el ancho NATURAL de la fuente casi exactamente (diferencia de 0.0%
 * a -0.1% para cada letra de "BOLIVIA" probada, ver DESIGN.md seccion
 * 46). Esto parecia revelar el error de fondo de TODOS los intentos
 * 25-45: se estaba comparando el AVANCE TOTAL entre caracteres (que
 * incluye el kerning) contra el ancho NATURAL del glyph. Se elimino
 * el ajuste de ancho por completo en base a esa conclusion.
 *
 * CORRECCION (seccion 47, ver DESIGN.md): esa conclusion resulto
 * INCOMPLETA, no incorrecta -- medir con fontTools a un tamanio de
 * referencia grande confirma que el DISENO de la fuente coincide con
 * /Widths, pero no dice nada sobre como GDI REDONDEA (hintea) el
 * ancho de cada glyph al pixel entero cuando el em real en pantalla
 * es chico (10-20px, el caso real de este documento a la escala de
 * pantalla tipica). Ese redondeo por-glyph es justo lo que producia
 * el patron "al azar" que ningun rango fijo (43-45) podia capturar,
 * porque no es un problema de RANGO sino de que cada glyph necesita
 * SU PROPIA correccion, no una constante ni un clamp compartido. La
 * seccion 47 mide el ancho ya-hinteado real de cada glyph
 * (GetTextExtentPoint32A, DESPUES de rasterizar) y lo escala al
 * ancho declarado por el PDF como remapeo de columnas sobre el
 * resultado ya rasterizado -- no antes, como MM_ANISOTROPIC en las
 * secciones 25-26 (eso arruinaba el hinting); y sin clamp de rango,
 * porque el factor correcto es exactamente advance_width_px dividido
 * el ancho natural medido, no una aproximacion acotada. */
/* CORRECCION FINAL (secciones 49-50 del DESIGN.md): todo el
 * diagnostico de arriba (hinting de GDI por-glyph a tamanio de
 * pantalla) resulto ser una pista falsa, aunque el ajuste en si
 * (interpolacion lineal al reescalar, ver mas abajo) sigue siendo
 * correcto como salvaguarda. La causa raiz real estaba en
 * pdf_font.c: pdf_font_load() no resolvia /Widths cuando venia como
 * referencia indirecta (comun en PDFs reales, confirmado en los 4
 * fonts de Flujograma_Propuesto.pdf) -- el motor central posicionaba
 * TODAS las letras con el mismo ancho generico (500/1000 em) sin
 * importar la letra, y el ajuste de esta funcion estaba estirando
 * cada glyph de GDI para igualar ese objetivo YA MAL CALCULADO (por
 * eso una 'I' angosta de verdad se estiraba a casi el doble de su
 * ancho real, saliendo como bloque solido -- confirmado corriendo
 * este archivo de verdad con GDI real, mingw + Wine, seccion 50).
 * Con /Widths arreglado en el motor central, 'advance_width_px' que
 * llega aca ahora es correcto, y el escalado de esta funcion pasa a
 * ser una salvaguarda para diferencias reales de fuente sustituta
 * (rara pero posible), no una compensacion de un bug en otro lado. */

/* Calcula la cobertura del pixel GDI sin conservar una asignacion
 * intermedia que Embarcadero C++ 7.70 pueda diagnosticar como W8004. */
static double pdf_gdi_pixel_coverage(double pixel_luma,
                                     double text_luma,
                                     double background_luma)
{
    double denom;
    double coverage;

    denom = text_luma - background_luma;

    if (denom > 0.0001 || denom < -0.0001)
        coverage = (pixel_luma - background_luma) / denom;
    else
        coverage = 0.0;

    if (coverage < 0.0)
        coverage = 0.0;

    if (coverage > 1.0)
        coverage = 1.0;

    return coverage;
}

static int pdf_gdi_draw_glyph_gray8(pdf_gdi_text_ctx *ctx,
                                      wchar_t wch,
                                      double origin_x_px,
                                      double origin_y_px,
                                      pdf_color color)
{
    MAT2 mat;
    GLYPHMETRICS gm;
    DWORD bytes;
    DWORD stride;
    unsigned char *gray;
    int x;
    int y;
    int px0;
    int py0;
    unsigned char v;
    double coverage;

    memset(&mat, 0, sizeof(mat));
    mat.eM11.value = 1;
    mat.eM22.value = 1;

    bytes = GetGlyphOutlineW(ctx->hdc, (UINT)wch, GGO_GRAY8_BITMAP,
                             &gm, 0, NULL, &mat);

    if (bytes == GDI_ERROR)
        return 0;

    if (gm.gmBlackBoxX == 0 || gm.gmBlackBoxY == 0)
        return 1;

    stride = ((DWORD)gm.gmBlackBoxX + 3UL) & ~3UL;
    if (stride == 0UL ||
        (DWORD)gm.gmBlackBoxY > 0xFFFFFFFFUL / stride)
        return 0;

    bytes = stride * (DWORD)gm.gmBlackBoxY;
    gray = (unsigned char *)malloc((size_t)bytes);
    if (gray == NULL)
        return 0;

    if (GetGlyphOutlineW(ctx->hdc, (UINT)wch, GGO_GRAY8_BITMAP,
                         &gm, bytes, gray, &mat) == GDI_ERROR)
    {
        free(gray);
        return 0;
    }

    /* gmptGlyphOrigin es el origen del glyph respecto del baseline.
     * El bitmap de GGO_GRAY8 esta ordenado de arriba hacia abajo. */
    px0 = (int)(origin_x_px + (double)gm.gmptGlyphOrigin.x + 0.5);
    py0 = (int)(origin_y_px - (double)gm.gmptGlyphOrigin.y + 0.5);

    for (y = 0; y < (int)gm.gmBlackBoxY; y++)
    {
        const unsigned char *row = gray + (long)y * (long)stride;

        for (x = 0; x < (int)gm.gmBlackBoxX; x++)
        {
            v = row[x];

            if (v == 0)
                continue;

            /* GGO_GRAY8_BITMAP usa 0..64. A diferencia del camino
             * TextOut/GetDIBits no existe un fondo que tengamos que
             * reconstruir: la cobertura ya viene separada de la tinta. */
            coverage = (double)v / 64.0;

            if (coverage > 1.0)
                coverage = 1.0;

            pdf_bitmap_set_pixel_coverage(ctx->dst,
                                          px0 + x,
                                          py0 + y,
                                          color,
                                          coverage);
        }
    }

    free(gray);
    return 1;
}

static void pdf_gdi_draw_glyph(void *user, unsigned char code, int unicode,
                                double origin_x_px, double origin_y_px,
                                double pixel_height, double advance_width_px,
                                double rotation_deg,
                                int is_bold, int is_italic, int is_serif,
                                pdf_color color, const char *base_font_name)
{
    pdf_gdi_text_ctx *ctx;
    int h;
    TEXTMETRICA tm;
    RECT clear_rect;
    wchar_t wch;
    int x, y;
    unsigned char *bits;
    int page_x0, page_y0;
    int escapement_deci_deg;
    int is_rotated;
    int bg_val;
    int natural_w;
    int active_w;
    int active_h;
    SIZE natural_sz;
    int need_h;
    int need_w;
    double luma;
    HBRUSH bg_brush;
    BITMAPINFO bmi;
    unsigned char *row;
    double pr;
    double pg;
    double pb;
    double pl;
    double tl;
    double bl;

    ctx = (pdf_gdi_text_ctx *)user;
    (void)advance_width_px;

    if (ctx == NULL || ctx->hdc == NULL || pixel_height < 1.0)
        return;

    h = (int)(pixel_height + 0.5);
    if (h < 1) h = 1;
    if (h > PDF_GDI_GLYPH_BMP_SIZE - PDF_GDI_GLYPH_MARGIN)
        h = PDF_GDI_GLYPH_BMP_SIZE - PDF_GDI_GLYPH_MARGIN; /* glyph gigante: recortar, mejor que desbordar el DC chico */

    /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 61):
     * antes 'rotation_deg' ni siquiera existia como parametro -- todo
     * texto se dibujaba horizontal sin importar la matriz Tm real del
     * PDF, confirmado con un caso real (TMKOA1de1.pdf): el "lomo"
     * vertical de una portada de tesis ("TESIS DOCTORAL", rotado 90
     * grados) salia gigante, apilado letra-sobre-letra, sin rotar.
     * GDI espera el angulo en DECIMAS de grado -- se redondea, no se
     * trunca, para minimizar el error acumulado en documentos con
     * mucho texto rotado al mismo angulo. */
    escapement_deci_deg = (int)(rotation_deg * 10.0 + (rotation_deg >= 0.0 ? 0.5 : -0.5));
    is_rotated = (escapement_deci_deg != 0);

    pdf_gdi_ensure_font(ctx, h, is_bold, is_italic, base_font_name, escapement_deci_deg, is_serif);

    wch = (wchar_t)unicode;

    /* Camino TrueType limpio para texto horizontal: GGO_GRAY8_BITMAP
     * entrega directamente la cobertura 0..64 del glyph ya rasterizado
     * por GDI. Esto elimina la reconstruccion de alpha a partir de un
     * fondo y evita el halo/doble borde que puede aparecer con
     * TextOutW + GetDIBits. El camino antiguo queda como fallback si
     * GDI no puede obtener el glyph. Para texto rotado conservamos
     * TextOutW porque GGO_GRAY8_BITMAP no aplica el escapement de la
     * misma forma que el DC de texto. */
    if (!is_rotated && pdf_gdi_draw_glyph_gray8(ctx, wch,
                                                origin_x_px, origin_y_px,
                                                color))
        return;

    GetTextMetricsA(ctx->hdc, &tm);
    (void)code; /* ya no se usa directo -- 'unicode' lo reemplaza, ver comentario abajo */
    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 55): antes se usaba
     * 'code' (el byte crudo del PDF) directo como caracter ANSI via
     * TextOutA -- funciona para fuentes con /Encoding estandar, pero
     * para fuentes con subconjunto embebido sin /Encoding (comun,
     * PowerPoint) 'code' es un indice arbitrario sin relacion con
     * ninguna letra real, y GDI no encuentra glyph -> "tofu". Ahora se
     * usa 'unicode' (ya resuelto via /ToUnicode por el motor central,
     * o igual a 'code' si no hace falta mapeo) con las APIs de texto
     * WIDE de GDI (TextOutW/GetTextExtentPoint32W mas abajo) en vez de
     * las ANSI -- funciona igual para el caso normal (unicode==code,
     * texto ASCII/Latin1 tipico) y ademas arregla el caso con
     * subconjunto, sin necesitar dos caminos de codigo distintos. */
    /*
     * El ancho natural se mide solo para limitar el area de composicion.
     * No se usa para deformar el glyph ni para remapear columnas.
     * La geometria del glyph queda determinada por GDI y el avance PDF
     * se conserva como dato de posicionamiento en el motor de texto.
     */
    {

        natural_sz.cx = 0;
        natural_sz.cy = 0;
        /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 61): este
         * escalado horizontal remapea columnas asumiendo que el eje X
         * del canvas auxiliar coincide con la direccion de AVANCE del
         * texto -- valido solo para texto horizontal. Con texto
         * rotado, GDI dibuja el glyph girado DENTRO del mismo canvas
         * (alrededor del punto de anclaje), asi que remapear columnas
         * en X deformaria el glyph en una direccion que ya no tiene
         * relacion con su avance real. No se realiza escalado horizontal
         * para el caso
         * rotado -- el glyph sale con el hinting natural de GDI, sin
         * el ajuste fino de ancho (una perdida aceptable frente a
         * dibujar directamente sin rotar, que es lo que habia antes). */
        /* El glyph conserva su geometria natural. El /W PDF se usa para
         * avanzar el text matrix, no para estirar/deformar el bitmap.
         * El escalado por glyph de versiones anteriores introducia
         * remuestreo y bordes dobles en cabeceras. */
        GetTextExtentPoint32W(ctx->hdc, &wch, 1, &natural_sz);
        natural_w = (int)natural_sz.cx;
    }

    /* BUG DE RENDIMIENTO REAL ENCONTRADO (ver DESIGN.md seccion 53):
     * hasta este punto, TODO glyph -- sin importar su tamanio real --
     * procesaba el canvas auxiliar COMPLETO de 256x256 (65536
     * iteraciones de composicion con interpolacion, que es lo caro --
     * no tanto GetDIBits en si, ver mas abajo) por cada caracter,
     * aunque el glyph real mida apenas 10-40px de alto/ancho en un
     * documento tipico (confirmado corriendo VWAM201103.pdf con GDI
     * real bajo Wine: ~5000 glyphs en ~8s, ~1.6ms cada uno). Se acota
     * el BUCLE DE COMPOSICION (mas abajo) a la region que realmente
     * puede tener tinta: alto = 'h' con margen generoso (cubre
     * ascendentes/descendentes), ancho = el ancho natural ya escalado
     * por el ancho natural medido, con margen generoso -- nunca mas que
     * PDF_GDI_GLYPH_BMP_SIZE.
     *
     * IMPORTANTE -- que NO se acoto, tras probarlo y revertirlo dos
     * veces con GDI real (mingw + Wine): ni el FillRect (seguia
     * limpiando el canvas COMPLETO a 256x256, no solo la region
     * activa) ni la lectura GetDIBits (sigue pidiendo las 256
     * scanlines completas). Acotar CUALQUIERA de esos dos producia
     * marcas de tinta vieja corrompiendo el resultado (confirmado
     * visualmente, glyphs completos saliendo como bloques solidos o
     * barras sueltas) -- no se identifico la causa exacta a nivel de
     * API (sospecha: alguna asuncion de GDI/Wine sobre que un
     * FillRect parcial repetido sobre el mismo DC de memoria deja
     * intacto el resto del bitmap de forma mas confiable de lo que en
     * la practica se observo). Achicar SOLO el bucle de composicion
     * (que no le pide nada distinto a GDI, solo lee menos del buffer
     * ya devuelto completo) da la mayor parte de la ganancia (~2.5x
     * mas rapido en la prueba de arriba) sin ese riesgo. */
    {
        need_h = h + h + PDF_GDI_GLYPH_MARGIN * 2; /* +100% para cubrir descendentes/ascendentes con margen amplio */
        need_w = natural_w + PDF_GDI_GLYPH_MARGIN * 3; /* margen generoso: el glyph real (natural_w es el AVANCE, no siempre el ancho exacto de la tinta -- itálicas/overhang) debe caber comodo */
        if (need_h < 1) need_h = 1;
        if (need_h > PDF_GDI_GLYPH_BMP_SIZE) need_h = PDF_GDI_GLYPH_BMP_SIZE;
        if (need_w < 1) need_w = 1;
        if (need_w > PDF_GDI_GLYPH_BMP_SIZE) need_w = PDF_GDI_GLYPH_BMP_SIZE;
        /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 61): este recorte
         * optimizado del bucle de composicion asume que el glyph se
         * extiende hacia la DERECHA (ancho) y un poco hacia arriba/abajo
         * del baseline (alto) -- valido solo para texto horizontal. Con
         * texto rotado, el glyph puede extenderse en CUALQUIER direccion
         * dentro del canvas de 256x256 (arriba, izquierda, diagonal),
         * asi que este recorte podria cortarlo. Para el caso rotado se
         * usa el canvas COMPLETO -- mas lento, pero el texto rotado es
         * raro en la practica (un "lomo" de portada, alguna etiqueta de
         * grafico), no vale la pena el riesgo de un recorte incorrecto. */
        if (is_rotated)
        {
            active_h = PDF_GDI_GLYPH_BMP_SIZE;
            active_w = PDF_GDI_GLYPH_BMP_SIZE;
        }
        else
        {
            active_h = need_h;
            active_w = need_w;
        }
    }

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 52): el canvas
     * auxiliar se limpiaba SIEMPRE a blanco, y la deteccion de "esto
     * es tinta" era simplemente "el pixel no es blanco". Eso rompe
     * completamente cualquier texto cuyo color sea blanco (o muy
     * claro) -- p.ej. texto blanco sobre una barra de color, un patron
     * MUY comun en titulos/banners (confirmado en VWAM201103.pdf: el
     * texto "Spotlight On..." se dibuja con '1 1 1 scn', blanco puro,
     * sobre una barra azul). GDI lo dibuja perfectamente, pero como el
     * canvas de fondo TAMBIEN es blanco, cada pixel del glyph queda
     * identico al fondo -- "tinta" y "fondo" son indistinguibles, y el
     * glyph entero se descarta silenciosamente al componer, quedando
     * invisible.
     *
     * Fix: elegir el color de fondo del canvas auxiliar dinamicamente,
     * segun el color de texto pedido -- negro si el texto es claro
     * (luminancia > 0.5), blanco si es oscuro. Con eso el contraste
     * entre "tinta" y "fondo" nunca colapsa a cero salvo en el caso
     * degenerado exacto (que no ocurre en la practica: un gris medio
     * exacto igual sigue teniendo ~127 de contraste contra cualquiera
     * de los dos extremos). */
    {
        luma = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
        bg_val = (luma > 0.5) ? 0 : 255;
    }

    clear_rect.left = 0; clear_rect.top = 0;
    clear_rect.right = PDF_GDI_GLYPH_BMP_SIZE; clear_rect.bottom = PDF_GDI_GLYPH_BMP_SIZE;
    {
        bg_brush = (HBRUSH)GetStockObject(bg_val == 0 ? BLACK_BRUSH : WHITE_BRUSH);
        FillRect(ctx->hdc, &clear_rect, bg_brush);
    }

    SetTextColor(ctx->hdc, RGB((int)(color.r * 255.0 + 0.5),
                                (int)(color.g * 255.0 + 0.5),
                                (int)(color.b * 255.0 + 0.5)));

    /* Dibujar a tamanio NATURAL (con el hinting real de GDI para este
     * glyph puntual) -- el ajuste de ancho ya NO se hace distorsionando
     * el DC antes de rasterizar (eso arruinaba la forma, secciones
     * 25-26). Se aplica despues, como remapeo de columnas al componer
     * sobre la pagina.
     *
     * BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 61): con
     * texto rotado, el ajuste manual "-tm.tmAscent" (pensado para
     * convertir el punto TOP-LEFT que espera TextOutW en modo
     * TA_TOP/default hacia el BASELINE, unicamente valido en la
     * direccion vertical del texto SIN rotar) queda aplicado en la
     * direccion EQUIVOCADA una vez que GDI rota el glyph -- confirmado
     * visualmente (mingw+Wine contra TMKOA1de1.pdf): el glyph rotado
     * aparecia case totalmente fuera de su posicion esperada, dejando
     * ver solo un fragmento diminuto en el borde del area de recorte.
     * Se evita el problema de raiz para el caso rotado: en vez de
     * calcular el offset a mano, se usa 'TA_BASELINE' (SetTextAlign)
     * para que GDI mismo interprete el punto pasado como el baseline
     * directamente, sin ambiguedad sobre en que direccion cae "arriba"
     * una vez rotado. El caso normal (la gran mayoria del texto) se
     * deja EXACTAMENTE igual que antes (TA_TOP/default + resta manual
     * de tmAscent, ya extensivamente verificado) para no arriesgar
     * ninguna regresion. */
    if (is_rotated)
    {
        /* BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md seccion 61):
         * el punto de anclaje 'MARGIN/2' (cerca del borde IZQUIERDO
         * del canvas auxiliar) tiene sentido para texto horizontal,
         * que siempre se extiende hacia la DERECHA desde ahi -- pero
         * con rotacion, el glyph puede extenderse en CUALQUIER
         * direccion (para 90 grados especificamente, hacia arriba Y
         * hacia la izquierda del punto de anclaje) -- confirmado
         * visualmente (mingw+Wine): las letras salian visiblemente
         * CORTADAS, mostrando solo una fraccion angosta de cada una,
         * consistente con el glyph saliendose del canvas por el lado
         * izquierdo (fuera de rango, indices negativos que
         * simplemente no existen en el canvas de 0..255). Se centra
         * el punto de anclaje en el MEDIO del canvas de 256x256 para
         * el caso rotado, dando margen amplio en las 4 direcciones. */
        SetTextAlign(ctx->hdc, TA_BASELINE | TA_LEFT);
        TextOutW(ctx->hdc, PDF_GDI_GLYPH_BMP_SIZE / 2,
                 PDF_GDI_GLYPH_BMP_SIZE / 2, &wch, 1);
        SetTextAlign(ctx->hdc, TA_TOP | TA_LEFT); /* restaurar el default para el proximo glyph (puede no ser rotado) */
    }
    else
    {
        TextOutW(ctx->hdc, PDF_GDI_GLYPH_MARGIN / 2,
                 (PDF_GDI_GLYPH_MARGIN / 2) - tm.tmAscent + h, &wch, 1);
    }

    /* Leer los pixeles dibujados via GetDIBits en vez de leer el
     * puntero 'ctx->bits' directo: GetDIBits es la funcion de GDI
     * hecha especificamente para copiar pixeles de un bitmap de forma
     * SINCRONIZADA -- a diferencia de leer el puntero del DIB section
     * a mano. Ver DESIGN.md seccion 34. */
    {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = PDF_GDI_GLYPH_BMP_SIZE;
        bmi.bmiHeader.biHeight      = -PDF_GDI_GLYPH_BMP_SIZE; /* top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        /* BUG REAL ENCONTRADO (confirmado con dos pruebas A/B bajo Wine
         * con GDI real, ver DESIGN.md seccion 53): tanto pedir menos de
         * las PDF_GDI_GLYPH_BMP_SIZE scanlines completas aca como
         * achicar el FillRect de mas arriba a solo la region activa
         * (en vez de siempre el canvas 256x256 completo) corrompen el
         * resultado por separado (glyphs saliendo como bloques solidos
         * o con marcas de tinta vieja sueltas). No se identifico la
         * causa exacta a nivel de API (sospecha: alguna asuncion de
         * GDI/Wine sobre lectura/limpieza parcial repetida del mismo
         * DC de memoria que no se cumple en la practica). Los DOS se
         * dejan siempre a canvas completo -- SOLO el bucle de
         * composicion de mas abajo se acota, que es donde estaba la
         * mayor parte del costo real (65536 iteraciones con
         * interpolacion y llamadas a pdf_bitmap_set_pixel por glyph,
         * contra una simple lectura de memoria a color que es barata
         * en comparacion) y no le pide a GDI nada distinto de lo que
         * ya hacia antes. */
        if (GetDIBits(ctx->hdc, ctx->hbmp, 0, PDF_GDI_GLYPH_BMP_SIZE,
                       ctx->readback, &bmi, DIB_RGB_COLORS) == 0)
            return; /* no se pudo leer: no componer nada mejor que componer basura */
    }

    /* pagina destino: origin_y_px es el baseline; el glyph se dibujo
     * con su baseline en (MARGIN/2 + h) local, asi que el offset entre
     * "local" y "pagina" para el eje Y es: pagina_y = local_y +
     * (origin_y_px - (MARGIN/2 + h)). Para X, origin_x_px coincide con
     * el borde izquierdo local MARGIN/2 -- ese punto queda FIJO (es el
     * origen del glyph, donde el motor central ya calculo el kerning
     * correcto); el resto de las columnas se remapean por
     * No se remapean columnas ni se deforma horizontalmente el glyph.
     * La geometria rasterizada se conserva en su tamano natural.
     * 'advance_width_px' pertenece al posicionamiento del texto PDF. */
    page_x0 = (int)(origin_x_px + 0.5) - (is_rotated ? (PDF_GDI_GLYPH_BMP_SIZE / 2) : (PDF_GDI_GLYPH_MARGIN / 2));
    page_y0 = (int)(origin_y_px + 0.5) - (is_rotated ? (PDF_GDI_GLYPH_BMP_SIZE / 2) : (PDF_GDI_GLYPH_MARGIN / 2) + h);

    bits = ctx->readback;
    for (y = 0; y < active_h; y++)
    {
        row = bits + (long)y * PDF_GDI_GLYPH_BMP_SIZE * 3;

        for (x = 0; x < active_w; x++)
        {
            pr = row[x * 3 + 2] / 255.0;
            pg = row[x * 3 + 1] / 255.0;
            pb = row[x * 3 + 0] / 255.0;
            pl = 0.299 * pr + 0.587 * pg + 0.114 * pb;
            tl = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
            bl = (double)bg_val / 255.0;
            /* GDI entrega un pixel antialiased como mezcla de tinta y
             * fondo: P = alpha*T + (1-alpha)*B. Recuperamos alpha y
             * volvemos a pintar con el COLOR PDF original. */
            if (pdf_gdi_pixel_coverage(pl, tl, bl) > 0.001)
                pdf_bitmap_set_pixel_coverage(
                    ctx->dst, page_x0 + x, page_y0 + y, color,
                    pdf_gdi_pixel_coverage(pl, tl, bl));
        }
    }
}


HB_FUNC(PDF_OPEN)
{
    const char *file;
    long budget_mb;
    pdf_hb_doc *h;

    file = hb_parc(1);
    budget_mb = hb_parnl(2); /* 0 si no se paso -> presupuesto por defecto */

    if (file == NULL)
    {
        hb_ret();
        return;
    }

    h = (pdf_hb_doc *)malloc(sizeof(pdf_hb_doc));
    if (h == NULL)
    {
        hb_ret();
        return;
    }

    if (pdf_stream_open(&h->st, file) != PDF_OK)
    {
        free(h);
        hb_ret();
        return;
    }

    {
        unsigned long budget_bytes = budget_mb > 0
            ? (unsigned long)budget_mb * 1024UL * 1024UL : 0UL;

        if (pdf_document_open(&h->doc, budget_bytes) != PDF_OK)
        {
            pdf_stream_close(&h->st);
            free(h);
            hb_ret();
            return;
        }
    }

    if (pdf_xref_load(&h->st, &h->doc.doc_arena, &h->xref) != PDF_OK)
    {
        pdf_document_close(&h->doc);
        pdf_stream_close(&h->st);
        free(h);
        hb_ret();
        return;
    }

    h->open = 1;
    h->text_cache_next = 0;
    {
        int ti;
        for (ti = 0; ti < PDF_HB_TEXT_CACHE_SLOTS; ti++)
            h->text_cache[ti].page_index = -1;
    }
    /* AcroForm: primer numero de objeto libre para apariencias nuevas
     * -- h->xref.count ya es "un pasado del mayor numero de objeto
     * existente" (indexado por numero de objeto, mismo criterio que
     * /Size del trailer). */
    h->next_new_obj_num = h->xref.count;
    h->n_touched = 0;
    h->n_cached_fonts = 0;
    hb_retptr((void *)h);
}

HB_FUNC(PDF_CLOSE)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);

    if (h != NULL && h->open)
    {
        pdf_document_close(&h->doc);
        pdf_stream_close(&h->st);
        h->open = 0;
        free(h);
    }
}

HB_FUNC(PDF_PAGECOUNT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    long count = 0;

    if (h != NULL && h->open)
    {
        pdf_obj *root = pdf_dict_get(h->xref.trailer, "Root");
        if (root != NULL && root->type == PDF_REF)
        {
            root = pdf_parser_load_object(&h->st, &h->xref, root->u.ref.num, &h->doc.doc_arena);
            if (root != NULL)
            {
                pdf_obj *pages = pdf_dict_get(root, "Pages");
                if (pages != NULL && pages->type == PDF_REF)
                {
                    pages = pdf_parser_load_object(&h->st, &h->xref, pages->u.ref.num, &h->doc.doc_arena);
                    if (pages != NULL)
                        count = pdf_dict_get_int(pages, "Count", 0);
                }
            }
        }
    }

    hb_retnl(count);
}

/* PDF_GETPAGESIZEPT( pDoc, nPagina, nExtraRotate ) -> { anchoPt, altoPt }
 * o NIL. Mismo calculo de tamanio de pagina (CropBox intersectado con
 * MediaBox, con /Rotate + rotacion extra ya swappeando ancho/alto para
 * 90/270) que PDF_RENDERTOHBITMAP, pero SIN decodificar el content
 * stream ni rasterizar nada -- Arturo reporto que abrir un documento
 * con muchas imagenes JPEG2000 pesadas (ver DESIGN.md seccion 75, el
 * fix de multiples capas de calidad que las hace tardar de verdad en
 * vez de fallar rapido y mal) se sentia "colgado": RenderCurrentPage()
 * hacia un primer render A ESCALA 1.0 SOLO para conocer el tamanio de
 * la pagina en puntos (ver pdf_viewer.prg), pagando el costo COMPLETO
 * de decodificar todas las imagenes de la pagina nada mas que para
 * leer dos numeros. Este binding nuevo hace ESO MISMO sin renderizar
 * -- pdf_viewer.prg ahora lo llama en vez de la pasada de "medir"
 * (ver comentario grande en METHOD RenderCurrentPage()). */
HB_FUNC(PDF_GETPAGESIZEPT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    int extra_rotate = hb_parni(3);
    pdf_page page;
    pdf_obj *page_obj, *mediabox, *cropbox, *rotate_obj;
    double media_x0, media_y0, media_x1, media_y1;
    double crop_x0, crop_y0, crop_x1, crop_y1;
    double page_w, page_h;
    int rotate;

    if (h == NULL || !h->open)
    {
        hb_ret();
        return;
    }
    if (pdf_page_open(&page, &h->doc, page_index + 1) != PDF_OK)
    {
        hb_ret();
        return;
    }
    page_obj = pdf_document_get_page(&h->st, &h->xref, &page.page_arena, page_index);
    if (page_obj == NULL)
    {
        pdf_page_close(&page);
        hb_ret();
        return;
    }

    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        media_x0 = obj_num(mediabox->u.arr.items[0]);
        media_y0 = obj_num(mediabox->u.arr.items[1]);
        media_x1 = obj_num(mediabox->u.arr.items[2]);
        media_y1 = obj_num(mediabox->u.arr.items[3]);
        if (media_x1 < media_x0) { double t = media_x0; media_x0 = media_x1; media_x1 = t; }
        if (media_y1 < media_y0) { double t = media_y0; media_y0 = media_y1; media_y1 = t; }
    }
    else
    {
        media_x0 = 0.0; media_y0 = 0.0; media_x1 = 612.0; media_y1 = 792.0;
    }

    crop_x0 = media_x0; crop_y0 = media_y0; crop_x1 = media_x1; crop_y1 = media_y1;
    cropbox = pdf_page_get_inherited(&h->st, &h->xref, &page.page_arena, page_obj, "CropBox");
    if (cropbox != NULL && cropbox->type == PDF_ARRAY && cropbox->u.arr.count == 4)
    {
        double cx0 = obj_num(cropbox->u.arr.items[0]);
        double cy0 = obj_num(cropbox->u.arr.items[1]);
        double cx1 = obj_num(cropbox->u.arr.items[2]);
        double cy1 = obj_num(cropbox->u.arr.items[3]);
        if (cx1 < cx0) { double t = cx0; cx0 = cx1; cx1 = t; }
        if (cy1 < cy0) { double t = cy0; cy0 = cy1; cy1 = t; }
        if (cx0 > media_x0) crop_x0 = cx0;
        if (cy0 > media_y0) crop_y0 = cy0;
        if (cx1 < media_x1) crop_x1 = cx1;
        if (cy1 < media_y1) crop_y1 = cy1;
    }
    if (crop_x1 <= crop_x0) { crop_x0 = media_x0; crop_x1 = media_x1; }
    if (crop_y1 <= crop_y0) { crop_y0 = media_y0; crop_y1 = media_y1; }

    page_w = crop_x1 - crop_x0;
    page_h = crop_y1 - crop_y0;

    rotate = 0;
    rotate_obj = pdf_page_get_inherited(&h->st, &h->xref, &page.page_arena, page_obj, "Rotate");
    if (rotate_obj != NULL)
        rotate = pdf_normalize_rotation((int)obj_num(rotate_obj));
    rotate = pdf_normalize_rotation(rotate + extra_rotate);
    if (rotate == 90 || rotate == 270)
    {
        double t = page_w; page_w = page_h; page_h = t;
    }

    pdf_page_close(&page);

    hb_reta(2);
    hb_storvnd(page_w, -1, 1);
    hb_storvnd(page_h, -1, 2);
}

HB_FUNC(PDF_RENDERTOHBITMAP)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1; /* Harbour: 1-based; motor: 0-based */
    double scale = hb_parnd(3);       /* opcional: factor puntos-PDF -> pixeles.
                                        * 0 (no se paso) -> default 1.0 (72 DPI,
                                        * el tamanio "nativo" de siempre). Usar
                                        * p.ej. 3.0-4.0 para resolucion real de
                                        * fotos de imprenta (216-288 DPI) -- ver
                                        * DESIGN.md, bug real reportado con
                                        * La_Maldicion_del_Mutun.pdf (revista
                                        * con fotos CMYK: a 72 DPI se veian
                                        * borrosas/de baja resolucion aunque el
                                        * color y el contenido ya eran correctos). */
    int extra_rotate = hb_parni(4);   /* opcional (Arturo: "necesito un proceso
                                        * de rotar pagina 90 grados"): rotacion
                                        * ADICIONAL pedida por el usuario (boton
                                        * "Rotar" en pdf_viewer.prg), SUMADA a
                                        * la que ya trae el propio /Rotate del
                                        * PDF -- asi una pagina escaneada de
                                        * costado se puede enderezar en pantalla
                                        * (y al imprimir, que usa esta misma
                                        * funcion) sin tocar el archivo original.
                                        * 0 (no se paso) no cambia nada -- todos
                                        * los llamadores viejos siguen igual. */
    pdf_page page;
    pdf_obj *page_obj, *mediabox, *cropbox, *resources, *rotate_obj;
    double media_x0, media_y0, media_x1, media_y1;
    double crop_x0, crop_y0, crop_x1, crop_y1;
    double page_w, page_h;
    int rotate, bmp_w, bmp_h;
    pdf_bitmap bmp;
    pdf_render_device dev;
    pdf_content_ops ops;
    HBITMAP hBmp;

    if (scale <= 0.0) scale = 1.0;

    if (h == NULL || !h->open)
    {
        hb_ret();
        return;
    }

    if (pdf_page_open(&page, &h->doc, page_index + 1) != PDF_OK)
    {
        hb_ret();
        return;
    }

    page_obj = pdf_document_get_page(&h->st, &h->xref, &page.page_arena, page_index);
    if (page_obj == NULL)
    {
        pdf_page_close(&page);
        hb_ret();
        return;
    }

    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        media_x0 = obj_num(mediabox->u.arr.items[0]);
        media_y0 = obj_num(mediabox->u.arr.items[1]);
        media_x1 = obj_num(mediabox->u.arr.items[2]);
        media_y1 = obj_num(mediabox->u.arr.items[3]);
        if (media_x1 < media_x0) { double t = media_x0; media_x0 = media_x1; media_x1 = t; }
        if (media_y1 < media_y0) { double t = media_y0; media_y0 = media_y1; media_y1 = t; }
    }
    else
    {
        media_x0 = 0.0; media_y0 = 0.0; media_x1 = 612.0; media_y1 = 792.0;
    }

    /* BUG REAL ENCONTRADO (Arturo, PDF real con /CropBox 842x595 muy
     * distinto de /MediaBox 842x1190) -- el motor ignoraba /CropBox por
     * completo, siempre usaba /MediaBox entero como "la pagina". Por
     * estandar (PDF 32000-1 7.7.3.3): /CropBox es heredable (por eso
     * pdf_page_get_inherited, no pdf_dict_get directo aca -- a
     * diferencia de /MediaBox arriba, que en la practica casi siempre
     * esta en la hoja; arreglar esa inconsistencia tambien queda fuera
     * de alcance por ahora), default a /MediaBox si no esta presente, y
     * NUNCA puede ser mas grande que /MediaBox -- se intersecta/recorta
     * contra el mismo por las dudas de un PDF real con valores fuera de
     * rango. */
    crop_x0 = media_x0; crop_y0 = media_y0; crop_x1 = media_x1; crop_y1 = media_y1;
    cropbox = pdf_page_get_inherited(&h->st, &h->xref, &page.page_arena, page_obj, "CropBox");
    if (cropbox != NULL && cropbox->type == PDF_ARRAY && cropbox->u.arr.count == 4)
    {
        double cx0 = obj_num(cropbox->u.arr.items[0]);
        double cy0 = obj_num(cropbox->u.arr.items[1]);
        double cx1 = obj_num(cropbox->u.arr.items[2]);
        double cy1 = obj_num(cropbox->u.arr.items[3]);
        if (cx1 < cx0) { double t = cx0; cx0 = cx1; cx1 = t; }
        if (cy1 < cy0) { double t = cy0; cy0 = cy1; cy1 = t; }
        if (cx0 > media_x0) crop_x0 = cx0;
        if (cy0 > media_y0) crop_y0 = cy0;
        if (cx1 < media_x1) crop_x1 = cx1;
        if (cy1 < media_y1) crop_y1 = cy1;
    }
    if (crop_x1 <= crop_x0) { crop_x0 = media_x0; crop_x1 = media_x1; }
    if (crop_y1 <= crop_y0) { crop_y0 = media_y0; crop_y1 = media_y1; }

    page_w = crop_x1 - crop_x0;
    page_h = crop_y1 - crop_y0;

    resources = pdf_dict_get(page_obj, "Resources");
    if (resources != NULL && resources->type == PDF_REF)
        resources = pdf_parser_load_object(&h->st, &h->xref, resources->u.ref.num, &page.page_arena);

    /* BUG REAL ENCONTRADO (Arturo: "las letras estan volteadas", pagina
     * de titulo con /Rotate 90) -- ver pdf_render_device_set_rotation.
     * /Rotate es un atributo HEREDABLE (normalmente vive en un nodo
     * /Pages padre, no en la pagina hoja) -- pdf_page_get_inherited,
     * NO pdf_dict_get directo (que aca arriba SI se usa para /MediaBox/
     * /Resources, una simplificacion previa que no contempla herencia;
     * fuera de alcance arreglar eso ahora, /Rotate es el caso reportado). */
    rotate = 0;
    rotate_obj = pdf_page_get_inherited(&h->st, &h->xref, &page.page_arena, page_obj, "Rotate");
    if (rotate_obj != NULL)
        rotate = pdf_normalize_rotation((int)obj_num(rotate_obj));
    rotate = pdf_normalize_rotation(rotate + extra_rotate);

    bmp_w = (int)(page_w * scale);
    bmp_h = (int)(page_h * scale);
    if (rotate == 90 || rotate == 270)
    {
        int tmp = bmp_w;
        bmp_w = bmp_h;
        bmp_h = tmp;
    }

    if (pdf_bitmap_create(&page.page_arena, bmp_w, bmp_h, &bmp) != PDF_OK)
    {
        pdf_page_close(&page);
        hb_ret();
        return;
    }

    pdf_render_device_init(&dev, &bmp, page_h, scale, resources, &h->st, &h->xref, &page.page_arena);
    dev.page_width_pt = page_w;
    dev.crop_x0 = crop_x0;
    dev.crop_y0 = crop_y0;
    pdf_render_device_set_rotation(&dev, rotate);
    pdf_render_device_set_font_cache_hook(&dev, hb_doc_font_cache_fn, h);
    ops.op            = pdf_render_op;
    ops.inline_image   = NULL;
    ops.user           = &dev;

    {
        pdf_gdi_text_ctx gdi_text;
        int gdi_ok = pdf_gdi_text_ctx_init(&gdi_text, &bmp);

        /* Gancho GDI DESACTIVADO a pedido (ver DESIGN.md, ronda "render
         * de fuentes real"): sin registrar pdf_render_device_set_glyph_hook,
         * dev->glyph_draw queda NULL y pdf_render.c dibuja el contorno
         * TrueType real (embebido o sustituto de sistema, sin GDI --
         * ver pdf_ttf.h) en su lugar, el mismo camino ya validado en el
         * motor portable. El resto de esta funcion (pdf_gdi_text_ctx_init/
         * _destroy, pdf_gdi_draw_glyph mas abajo) queda intacto por si
         * hace falta volver a GDI -- alcanza con descomentar la linea
         * de abajo. */
        (void)gdi_ok;
        /* if (gdi_ok) pdf_render_device_set_glyph_hook(&dev, pdf_gdi_draw_glyph, &gdi_text); */

        {
            pdf_buf decoded;
            if (pdf_page_get_content(&h->st, &h->xref, page_obj, &page.decode_arena, &decoded) == PDF_OK)
                pdf_content_run(decoded.data, decoded.len, &page.decode_arena, &ops);
        }

        /* AcroForm: dibujar la apariencia actual de cada campo Widget
         * ENCIMA del contenido de pagina (asi lo exige la norma) --
         * ver DESIGN.md, fase AcroForm. */
        pdf_render_draw_annotations(&dev, page_obj);

        /* Resaltado de texto (/Subtype /Highlight, ver pdf_annot.h/
         * DESIGN.md) -- mismo momento que las anotaciones de arriba
         * (encima del contenido de pagina), cubre tanto los resaltados
         * agregados en esta sesion (HB_FUNC(PDF_ANNOTADDHIGHLIGHT))
         * como cualquier /Highlight preexistente en el archivo. */
        pdf_render_draw_highlight_annotations(&dev, page_obj);

        if (gdi_ok)
            pdf_gdi_text_ctx_destroy(&gdi_text);
    }

    /* Convertir a HBITMAP ANTES de cerrar la pagina: pdf_bitmap_to_hbitmap
     * copia los pixeles a memoria propia de GDI, asi que es seguro liberar
     * page_arena (donde vivia bmp.pixels) despues. */
    hBmp = pdf_bitmap_to_hbitmap(&bmp);

    pdf_page_close(&page);

    if (hBmp == NULL)
    {
        hb_ret();
        return;
    }

    {
        PHB_ITEM aResult = hb_itemArrayNew(3);
        /* IMPORTANTE: el HBITMAP va como NUMERO (hb_arraySetNL), no como
         * puntero (hb_arraySetPtr). FiveWin distingue handles GDI de
         * punteros GDI+ por el TIPO Harbour del valor: GetBmpHeight/
         * GetBmpWidth (source/function/imgtxtio.prg) hacen
         * "if HB_ISPOINTER(bmp) -> tratarlo como imagen GDI+" ANTES de
         * revisar ISHBITMAP -- si el HBITMAP llega como puntero, FiveWin
         * lo pasa a GDIP_ImageInfo() y crashea porque no es un pImage de
         * GDI+ valido. Confirmado contra imgtxtio.prg linea 4256:
         * "HB_ISNUMERIC(uValue) .and. ISHBITMAP(uValue)". */
        hb_arraySetNL(aResult, 1, (long)(HB_PTRUINT)hBmp);
        hb_arraySetNL(aResult, 2, (long)bmp.width);
        hb_arraySetNL(aResult, 3, (long)bmp.height);
        hb_itemReturnRelease(aResult);
    }
}

/* ============================================================================
 * Extraccion / busqueda / copia de texto (fase 2 del roadmap de
 * potencialidad MuPDF -- ver DESIGN.md seccion 70, pdf_text_extract.h).
 * ============================================================================
 *
 *   PDF_EXTRACTTEXT( pDoc, nPage )
 *       -> nGlyphs (llena/actualiza el cache de la pagina, 0 si fallo)
 *   PDF_FINDTEXT( pDoc, nPage, cNeedle, lCaseSensitive )
 *       -> array de { nStartIdx, nEndIdx, nX0, nY0, nX1, nY1 } (1-based,
 *          espacio de puntos PDF)
 *   PDF_GLYPHSINRECT( pDoc, nPage, nX0, nY0, nX1, nY1 )
 *       -> array de { nStartIdx, nEndIdx } (1-based), un elemento por
 *          linea de texto que el rectangulo toca
 *   PDF_GETGLYPHTEXT( pDoc, nPage, aRanges )
 *       -> cString UTF-8 concatenando los rangos de aRanges (formato
 *          identico a la salida de PDF_GLYPHSINRECT/columnas 1-2 de
 *          PDF_FINDTEXT), con salto de linea entre rangos no
 *          adyacentes.
 *
 * Todos los indices de glyph que cruzan la frontera C<->Harbour son
 * 1-based (convencion Harbour); pdf_text_extract.h es 0-based (convencion
 * C) -- la conversion se hace aca, no en el motor.
 */

/* Busca 'page_index' (0-based) en el cache; si no esta, lo extrae hacia
 * el proximo slot (round-robin) y lo cachea. Nunca devuelve NULL --
 * si la extraccion real falla (pagina invalida, etc.) el slot queda
 * con n_glyphs=0 y page_index en -1 (no se cachea un fallo, para que
 * un reintento futuro con datos validos SI se cachee). */
static pdf_hb_text_slot *ensure_text_cache(pdf_hb_doc *h, int page_index)
{
    int i;
    pdf_hb_text_slot *slot;
    pdf_page page;
    pdf_obj *page_obj;
    pdf_text_page tp;

    for (i = 0; i < PDF_HB_TEXT_CACHE_SLOTS; i++)
        if (h->text_cache[i].page_index == page_index)
            return &h->text_cache[i];

    slot = &h->text_cache[h->text_cache_next];
    h->text_cache_next = (h->text_cache_next + 1) % PDF_HB_TEXT_CACHE_SLOTS;
    slot->page_index = -1;
    slot->n_glyphs   = 0;

    if (pdf_page_open(&page, &h->doc, page_index + 1) != PDF_OK)
        return slot;

    page_obj = pdf_document_get_page(&h->st, &h->xref, &page.page_arena, page_index);
    if (page_obj == NULL)
    {
        pdf_page_close(&page);
        return slot;
    }

    tp.glyphs    = slot->glyphs;
    tp.glyph_cap = PDF_HB_TEXT_MAX_GLYPHS;
    if (pdf_text_extract_page(&h->st, &h->xref, &page, page_obj, &tp) == PDF_OK)
    {
        slot->n_glyphs      = tp.n_glyphs;
        slot->page_width_pt = tp.page_width_pt;
        slot->page_height_pt = tp.page_height_pt;
        slot->page_index    = page_index;
    }

    pdf_page_close(&page);
    return slot;
}

static void slot_to_text_page(const pdf_hb_text_slot *slot, pdf_text_page *tp)
{
    tp->glyphs         = (pdf_text_glyph *)slot->glyphs;
    tp->glyph_cap       = PDF_HB_TEXT_MAX_GLYPHS;
    tp->n_glyphs        = slot->n_glyphs;
    tp->page_width_pt   = slot->page_width_pt;
    tp->page_height_pt  = slot->page_height_pt;
}

HB_FUNC(PDF_EXTRACTTEXT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    pdf_hb_text_slot *slot;

    if (h == NULL || !h->open || page_index < 0)
    {
        hb_retni(0);
        return;
    }

    slot = ensure_text_cache(h, page_index);
    hb_retni(slot->n_glyphs);
}

#define PDF_HB_MAX_MATCHES 128

HB_FUNC(PDF_FINDTEXT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    const char *needle = hb_parc(3);
    int case_sensitive = hb_parl(4);
    pdf_hb_text_slot *slot;
    pdf_text_page tp;
    pdf_text_match matches[PDF_HB_MAX_MATCHES];
    int n_matches, i;
    PHB_ITEM aResult;

    if (h == NULL || !h->open || page_index < 0 || needle == NULL || needle[0] == '\0')
    {
        hb_reta(0);
        return;
    }

    slot = ensure_text_cache(h, page_index);
    slot_to_text_page(slot, &tp);

    n_matches = 0;
    pdf_text_search(&tp, needle, case_sensitive, matches, PDF_HB_MAX_MATCHES, &n_matches);
    if (n_matches > PDF_HB_MAX_MATCHES) n_matches = PDF_HB_MAX_MATCHES;

    aResult = hb_itemArrayNew(n_matches);
    for (i = 0; i < n_matches; i++)
    {
        PHB_ITEM aRow = hb_itemArrayNew(6);
        hb_arraySetNI(aRow, 1, matches[i].start_glyph_idx + 1);
        hb_arraySetNI(aRow, 2, matches[i].end_glyph_idx + 1);
        hb_arraySetND(aRow, 3, matches[i].x0);
        hb_arraySetND(aRow, 4, matches[i].y0);
        hb_arraySetND(aRow, 5, matches[i].x1);
        hb_arraySetND(aRow, 6, matches[i].y1);
        hb_arraySetForward(aResult, i + 1, aRow);
        hb_itemRelease(aRow);
    }
    hb_itemReturnRelease(aResult);
}

#define PDF_HB_MAX_RECT_RANGES 256

HB_FUNC(PDF_GLYPHSINRECT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    double rx0 = hb_parnd(3);
    double ry0 = hb_parnd(4);
    double rx1 = hb_parnd(5);
    double ry1 = hb_parnd(6);
    pdf_hb_text_slot *slot;
    pdf_text_page tp;
    int line_start, i, n_ranges;
    int range_lo[PDF_HB_MAX_RECT_RANGES], range_hi[PDF_HB_MAX_RECT_RANGES];
    double range_x0[PDF_HB_MAX_RECT_RANGES], range_y0[PDF_HB_MAX_RECT_RANGES];
    double range_x1[PDF_HB_MAX_RECT_RANGES], range_y1[PDF_HB_MAX_RECT_RANGES];
    PHB_ITEM aResult;
    double tmp;

    if (rx0 > rx1) { tmp = rx0; rx0 = rx1; rx1 = tmp; }
    if (ry0 > ry1) { tmp = ry0; ry0 = ry1; ry1 = tmp; }

    if (h == NULL || !h->open || page_index < 0)
    {
        hb_reta(0);
        return;
    }

    slot = ensure_text_cache(h, page_index);
    slot_to_text_page(slot, &tp);

    /* Agrupa por "linea" (corrida maxima de indices consecutivos con el
     * mismo baseline y0_px, ver comentario de pdf_text_extract.h) y,
     * dentro de cada linea que el rectangulo toca, emite UN rango que
     * va del primer al ultimo glyph que se superpone -- "un run por
     * linea", ver plan de esta fase. */
    n_ranges = 0;
    line_start = 0;
    for (i = 0; i <= tp.n_glyphs; i++)
    {
        int line_end = i; /* exclusivo */
        int same_line = (i < tp.n_glyphs) && (line_start < tp.n_glyphs) &&
                         (tp.glyphs[i].y0_px == tp.glyphs[line_start].y0_px);

        if (same_line)
            continue;

        if (line_end > line_start)
        {
            int lo = -1, hi = -1;
            double bx0 = 0.0, by0 = 0.0, bx1 = 0.0, by1 = 0.0;
            int k;
            for (k = line_start; k < line_end; k++)
            {
                const pdf_text_glyph *g = &tp.glyphs[k];
                double gx0 = g->x0_px, gx1 = g->x0_px + g->advance_px;
                /* ver PDF_TEXT_ASCENT_FRAC/PDF_TEXT_DESCENT_FRAC en
                 * pdf_text_extract.h -- mismo criterio que pdf_text_search,
                 * sin esto el recuadro de seleccion quedaba pegado al
                 * baseline (BUG REAL ENCONTRADO, Arturo: "sin encuadre a
                 * la seleccion en el sentido vertical"). */
                double gy0 = g->y0_px - g->height_px * PDF_TEXT_ASCENT_FRAC;
                double gy1 = g->y0_px + g->height_px * PDF_TEXT_DESCENT_FRAC;

                if (gx1 >= rx0 && gx0 <= rx1 && gy1 >= ry0 && gy0 <= ry1)
                {
                    if (lo < 0)
                    {
                        lo = k;
                        bx0 = gx0; bx1 = gx1; by0 = gy0; by1 = gy1;
                    }
                    else
                    {
                        if (gx0 < bx0) bx0 = gx0;
                        if (gx1 > bx1) bx1 = gx1;
                        if (gy0 < by0) by0 = gy0;
                        if (gy1 > by1) by1 = gy1;
                    }
                    hi = k;
                }
            }
            if (lo >= 0 && n_ranges < PDF_HB_MAX_RECT_RANGES)
            {
                range_lo[n_ranges] = lo;
                range_hi[n_ranges] = hi;
                range_x0[n_ranges] = bx0;
                range_y0[n_ranges] = by0;
                range_x1[n_ranges] = bx1;
                range_y1[n_ranges] = by1;
                n_ranges++;
            }
        }
        line_start = i;
    }

    aResult = hb_itemArrayNew(n_ranges);
    for (i = 0; i < n_ranges; i++)
    {
        PHB_ITEM aRow = hb_itemArrayNew(6);
        hb_arraySetNI(aRow, 1, range_lo[i] + 1);
        hb_arraySetNI(aRow, 2, range_hi[i] + 1);
        hb_arraySetND(aRow, 3, range_x0[i]);
        hb_arraySetND(aRow, 4, range_y0[i]);
        hb_arraySetND(aRow, 5, range_x1[i]);
        hb_arraySetND(aRow, 6, range_y1[i]);
        hb_arraySetForward(aResult, i + 1, aRow);
        hb_itemRelease(aRow);
    }
    hb_itemReturnRelease(aResult);
}

/* Pdf_GlyphsBetweenPoints( pDoc, nPage, nX0,nY0, nX1,nY1 )
 *     -> array de { nStartIdx, nEndIdx, nX0, nY0, nX1, nY1 } (1-based,
 *        puntos PDF), un elemento por linea visual -- pero a diferencia
 *        de Pdf_GlyphsInRect (interseccion GEOMETRICA de rectangulo,
 *        recorta por columna de X en cada linea), esta selecciona el
 *        rango CONTIGUO de indices de glyph entre el punto mas cercano
 *        a (nX0,nY0) y el mas cercano a (nX1,nY1) -- ver
 *        pdf_text_nearest_glyph (pdf_text_extract.h). Como el arreglo
 *        de glyphs ya esta en orden de aparicion del content stream, el
 *        texto resultante SIEMPRE sigue la secuencia natural de
 *        letras/palabras, incluso cruzando varias lineas completas --
 *        BUG REAL ENCONTRADO (Arturo: "la seleccion no deberia ser al
 *        azar en la pantalla sino que vaya de acuerdo con la secuencia
 *        de letras y palabras"). Se sigue partiendo en sub-rangos por
 *        linea SOLO para que el overlay de Paint() (ver TPdfBitmap,
 *        pdf_viewer.prg) dibuje un rectangulo por linea en vez de uno
 *        gigante cubriendo tambien los margenes de la primera/ultima
 *        linea -- el ORDEN de esos sub-rangos ya sale correcto (indices
 *        crecientes) porque se recorre [lo,hi] de una punta a la otra. */
HB_FUNC(PDF_GLYPHSBETWEENPOINTS)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    double x0 = hb_parnd(3);
    double y0 = hb_parnd(4);
    double x1 = hb_parnd(5);
    double y1 = hb_parnd(6);
    pdf_hb_text_slot *slot;
    pdf_text_page tp;
    int idxA, idxB, lo, hi;
    int line_start, i, n_ranges;
    int range_lo[PDF_HB_MAX_RECT_RANGES], range_hi[PDF_HB_MAX_RECT_RANGES];
    double range_x0[PDF_HB_MAX_RECT_RANGES], range_y0[PDF_HB_MAX_RECT_RANGES];
    double range_x1[PDF_HB_MAX_RECT_RANGES], range_y1[PDF_HB_MAX_RECT_RANGES];
    PHB_ITEM aResult;

    if (h == NULL || !h->open || page_index < 0)
    {
        hb_reta(0);
        return;
    }

    slot = ensure_text_cache(h, page_index);
    slot_to_text_page(slot, &tp);

    idxA = pdf_text_nearest_glyph(&tp, x0, y0);
    idxB = pdf_text_nearest_glyph(&tp, x1, y1);
    if (idxA < 0 || idxB < 0)
    {
        hb_reta(0);
        return;
    }

    lo = (idxA < idxB) ? idxA : idxB;
    hi = (idxA < idxB) ? idxB : idxA;

    n_ranges  = 0;
    line_start = lo;
    for (i = lo; i <= hi + 1; i++)
    {
        int line_end = i; /* exclusivo */
        int same_line = (i <= hi) &&
                         (tp.glyphs[i].y0_px == tp.glyphs[line_start].y0_px);

        if (same_line)
            continue;

        if (line_end > line_start)
        {
            double bx0, by0, bx1, by1;
            int k;

            bx0 = tp.glyphs[line_start].x0_px;
            bx1 = tp.glyphs[line_end - 1].x0_px + tp.glyphs[line_end - 1].advance_px;
            by0 = tp.glyphs[line_start].y0_px - tp.glyphs[line_start].height_px * PDF_TEXT_ASCENT_FRAC;
            by1 = tp.glyphs[line_start].y0_px + tp.glyphs[line_start].height_px * PDF_TEXT_DESCENT_FRAC;

            for (k = line_start; k < line_end; k++)
            {
                const pdf_text_glyph *g = &tp.glyphs[k];
                double gx1 = g->x0_px + g->advance_px;
                double gy0 = g->y0_px - g->height_px * PDF_TEXT_ASCENT_FRAC;
                double gy1 = g->y0_px + g->height_px * PDF_TEXT_DESCENT_FRAC;

                if (g->x0_px < bx0) bx0 = g->x0_px;
                if (gx1 > bx1) bx1 = gx1;
                if (gy0 < by0) by0 = gy0;
                if (gy1 > by1) by1 = gy1;
            }

            if (n_ranges < PDF_HB_MAX_RECT_RANGES)
            {
                range_lo[n_ranges] = line_start;
                range_hi[n_ranges] = line_end - 1;
                range_x0[n_ranges] = bx0;
                range_y0[n_ranges] = by0;
                range_x1[n_ranges] = bx1;
                range_y1[n_ranges] = by1;
                n_ranges++;
            }
        }
        line_start = i;
    }

    aResult = hb_itemArrayNew(n_ranges);
    for (i = 0; i < n_ranges; i++)
    {
        PHB_ITEM aRow = hb_itemArrayNew(6);
        hb_arraySetNI(aRow, 1, range_lo[i] + 1);
        hb_arraySetNI(aRow, 2, range_hi[i] + 1);
        hb_arraySetND(aRow, 3, range_x0[i]);
        hb_arraySetND(aRow, 4, range_y0[i]);
        hb_arraySetND(aRow, 5, range_x1[i]);
        hb_arraySetND(aRow, 6, range_y1[i]);
        hb_arraySetForward(aResult, i + 1, aRow);
        hb_itemRelease(aRow);
    }
    hb_itemReturnRelease(aResult);
}

/* Codifica 'cp' (codepoint Unicode) como UTF-8 en 'out' (buffer de al
 * menos 4 bytes). Devuelve la cantidad de bytes escritos. */
static int utf8_encode(int cp, char *out)
{
    if (cp < 0) cp = 0xFFFD;
    if (cp <= 0x7F)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

#define PDF_HB_GETTEXT_BUF_SIZE (256 * 1024)

HB_FUNC(PDF_GETGLYPHTEXT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    PHB_ITEM aRanges = hb_param(3, HB_IT_ARRAY);
    pdf_hb_text_slot *slot;
    pdf_text_page tp;
    char *buf;
    HB_SIZE out_len;
    HB_SIZE n_ranges, r;

    if (h == NULL || !h->open || page_index < 0 || aRanges == NULL)
    {
        hb_retc_null();
        return;
    }

    slot = ensure_text_cache(h, page_index);
    slot_to_text_page(slot, &tp);

    buf = (char *)malloc(PDF_HB_GETTEXT_BUF_SIZE);
    if (buf == NULL)
    {
        hb_retc_null();
        return;
    }
    out_len = 0;

    n_ranges = hb_arrayLen(aRanges);
    for (r = 1; r <= n_ranges; r++)
    {
        PHB_ITEM aRow = hb_arrayGetItemPtr(aRanges, r);
        int lo, hi, k;

        if (aRow == NULL || !HB_IS_ARRAY(aRow) || hb_arrayLen(aRow) < 2)
            continue;

        lo = hb_arrayGetNI(aRow, 1) - 1;
        hi = hb_arrayGetNI(aRow, 2) - 1;
        if (lo < 0 || hi >= tp.n_glyphs || lo > hi)
            continue;

        if (r > 1 && out_len > 0 && out_len < PDF_HB_GETTEXT_BUF_SIZE - 1)
            buf[out_len++] = '\n';

        for (k = lo; k <= hi; k++)
        {
            char enc[4];
            int enc_len, e;

            enc_len = utf8_encode(tp.glyphs[k].unicode, enc);
            if (out_len + (HB_SIZE)enc_len >= PDF_HB_GETTEXT_BUF_SIZE)
                break;
            for (e = 0; e < enc_len; e++)
                buf[out_len++] = enc[e];
        }
    }

    hb_retclen_buffer(buf, out_len);
}

/* ============================================================================
 * AcroForm (ver pdf_form.h/DESIGN.md, fase AcroForm del roadmap de
 * potencialidad MuPDF) -- lectura, edicion en memoria y guardado de
 * campos de texto/checkbox.
 * ============================================================================ */

/* Duplica SOLO la resolucion de /MediaBox+/CropBox de
 * HB_FUNC(PDF_RENDERTOHBITMAP) (ver el comentario grande ahi sobre el
 * bug real de /CropBox ignorado) -- necesaria aca para poder invertir
 * el eje Y de los /Rect de campos AcroForm a la MISMA convencion "top-
 * down, origen arriba-izquierda" que ya usa el resto de las
 * coordenadas que este archivo devuelve a Harbour (texto: ver
 * pdf_glyph_draw_fn en pdf_render.h, "ya con el flip de eje Y
 * aplicado") -- sin esto, Pdf_FormListFields devolveria /Rect en
 * espacio PDF crudo (Y creciendo hacia arriba), incompatible con
 * TPdfBitmap:BmpToPagePoint/PagePointToBmp (pdf_viewer.prg), que
 * asumen la convencion top-down en TODO lo que reciben.
 *
 * NO se corrige por /Rotate (a diferencia del render real) -- alcance
 * de esta etapa: paginas sin rotar. Documentado como limitacion
 * conocida en DESIGN.md. */
static void pdf_hb_resolve_page_box(pdf_stream *st, const pdf_xref_table *xref,
                                     pdf_arena *arena, pdf_obj *page_obj,
                                     double *out_crop_x0, double *out_crop_y0,
                                     double *out_page_w, double *out_page_h)
{
    pdf_obj *mediabox, *cropbox;
    double media_x0, media_y0, media_x1, media_y1;
    double crop_x0, crop_y0, crop_x1, crop_y1;

    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        media_x0 = obj_num(mediabox->u.arr.items[0]);
        media_y0 = obj_num(mediabox->u.arr.items[1]);
        media_x1 = obj_num(mediabox->u.arr.items[2]);
        media_y1 = obj_num(mediabox->u.arr.items[3]);
        if (media_x1 < media_x0) { double t = media_x0; media_x0 = media_x1; media_x1 = t; }
        if (media_y1 < media_y0) { double t = media_y0; media_y0 = media_y1; media_y1 = t; }
    }
    else
    {
        media_x0 = 0.0; media_y0 = 0.0; media_x1 = 612.0; media_y1 = 792.0;
    }

    crop_x0 = media_x0; crop_y0 = media_y0; crop_x1 = media_x1; crop_y1 = media_y1;
    cropbox = pdf_page_get_inherited(st, xref, arena, page_obj, "CropBox");
    if (cropbox != NULL && cropbox->type == PDF_ARRAY && cropbox->u.arr.count == 4)
    {
        double cx0 = obj_num(cropbox->u.arr.items[0]);
        double cy0 = obj_num(cropbox->u.arr.items[1]);
        double cx1 = obj_num(cropbox->u.arr.items[2]);
        double cy1 = obj_num(cropbox->u.arr.items[3]);
        if (cx1 < cx0) { double t = cx0; cx0 = cx1; cx1 = t; }
        if (cy1 < cy0) { double t = cy0; cy0 = cy1; cy1 = t; }
        if (cx0 > media_x0) crop_x0 = cx0;
        if (cy0 > media_y0) crop_y0 = cy0;
        if (cx1 < media_x1) crop_x1 = cx1;
        if (cy1 < media_y1) crop_y1 = cy1;
    }
    if (crop_x1 <= crop_x0) { crop_x0 = media_x0; crop_x1 = media_x1; }
    if (crop_y1 <= crop_y0) { crop_y0 = media_y0; crop_y1 = media_y1; }

    *out_crop_x0 = crop_x0;
    *out_crop_y0 = crop_y0;
    *out_page_w  = crop_x1 - crop_x0;
    *out_page_h  = crop_y1 - crop_y0;
}

/* Pdf_FormListFields( pDoc, nPage ) -> array de filas
 *     { nType, cValue, nX0, nY0, nX1, nY1, cName, lReadOnly,
 *       nObjNum, nObjGen, cOnStateName }
 * nType: 1=texto, 2=checkbox, 3=otro (radio/combo/lista/firma --
 * listado, no editable en esta etapa). x0..y1 en espacio de pagina
 * (puntos PDF), mismo espacio que el resto de los bindings de texto.
 * nObjNum/nObjGen identifican el objeto a pasarle a
 * Pdf_FormSetFieldValue al editar este campo. */
HB_FUNC(PDF_FORMLISTFIELDS)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    pdf_form_field fields[PDF_FORM_MAX_FIELDS];
    pdf_obj *page_obj;
    int n, i;
    PHB_ITEM aResult;
    double crop_x0, crop_y0, page_w, page_h;

    if (h == NULL || !h->open || page_index < 0)
    {
        hb_reta(0);
        return;
    }

    page_obj = pdf_document_get_page(&h->st, &h->xref, &h->doc.doc_arena, page_index);
    if (page_obj == NULL)
    {
        hb_reta(0);
        return;
    }

    pdf_hb_resolve_page_box(&h->st, &h->xref, &h->doc.doc_arena, page_obj,
                             &crop_x0, &crop_y0, &page_w, &page_h);

    n = pdf_form_list_fields(&h->st, &h->xref, &h->doc.doc_arena, page_obj, fields, PDF_FORM_MAX_FIELDS);

    aResult = hb_itemArrayNew(n);
    for (i = 0; i < n; i++)
    {
        pdf_form_field *f = &fields[i];
        PHB_ITEM aRow = hb_itemArrayNew(11);
        /* /Rect crudo (espacio PDF, Y creciendo hacia arriba) -> misma
         * convencion "top-down" que ya usa el resto de las coordenadas
         * devueltas a Harbour (ver comentario grande de
         * pdf_hb_resolve_page_box arriba). */
        double tx0 = f->rect.x0 - crop_x0;
        double tx1 = f->rect.x1 - crop_x0;
        double ty0 = page_h - (f->rect.y1 - crop_y0); /* el borde y1 (arriba en PDF) pasa a ser el MENOR */
        double ty1 = page_h - (f->rect.y0 - crop_y0);

        hb_arraySetNI(aRow, 1, (int)f->type);
        hb_arraySetC (aRow, 2, f->value);
        hb_arraySetND(aRow, 3, tx0);
        hb_arraySetND(aRow, 4, ty0);
        hb_arraySetND(aRow, 5, tx1);
        hb_arraySetND(aRow, 6, ty1);
        hb_arraySetC (aRow, 7, f->name);
        hb_arraySetL (aRow, 8, f->read_only ? HB_TRUE : HB_FALSE);
        hb_arraySetNL(aRow, 9, f->obj_num);
        hb_arraySetNL(aRow, 10, f->obj_gen);
        hb_arraySetC (aRow, 11, f->on_state_name);
        hb_arraySetForward(aResult, i + 1, aRow);
        hb_itemRelease(aRow);
    }
    hb_itemReturnRelease(aResult);
}

/* Pdf_FormSetFieldValue( pDoc, nObjNum, nObjGen, nFieldType, cNewValue )
 *     -> lReturn
 * Actualiza el campo EN MEMORIA (muta /V -- y /AS para checkbox, /AP/N
 * para texto -- directo en el arbol de objetos parseado, doc_arena) --
 * NO escribe nada a disco todavia (ver Pdf_FormSave). Un
 * Pdf_RenderToHBitmap posterior de la misma pagina ya refleja el
 * cambio, porque lee los MISMOS objetos mutados (nunca se vuelve a
 * parsear el archivo). nFieldType usa la misma convencion que la
 * columna 1 de Pdf_FormListFields (1=texto, 2=checkbox). */
HB_FUNC(PDF_FORMSETFIELDVALUE)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    long obj_num = hb_parnl(2);
    long obj_gen = hb_parnl(3);
    int field_type = hb_parni(4);
    const char *new_value = hb_parc(5);
    pdf_obj *widget;

    if (h == NULL || !h->open || obj_num <= 0 || new_value == NULL)
    {
        hb_retl(HB_FALSE);
        return;
    }

    widget = pdf_xref_load_object(&h->st, &h->xref, obj_num, &h->doc.doc_arena);
    if (widget == NULL || (widget->type != PDF_DICT && widget->type != PDF_STREAM))
    {
        hb_retl(HB_FALSE);
        return;
    }

    if (field_type == PDF_FORM_FIELD_CHECKBOX)
    {
        pdf_obj *name_obj = pdf_obj_new_name(&h->doc.doc_arena, new_value);
        pdf_dict_set(&h->doc.doc_arena, widget, "V", name_obj);
        pdf_dict_set(&h->doc.doc_arena, widget, "AS", name_obj);
        mark_touched(h, obj_num, obj_gen, widget);
        hb_retl(HB_TRUE);
        return;
    }

    if (field_type == PDF_FORM_FIELD_TEXT)
    {
        pdf_obj *str_obj = pdf_obj_new_string(&h->doc.doc_arena, new_value, (long)strlen(new_value));
        pdf_obj *ap_dict;
        pdf_obj *new_ap;
        long new_num;

        pdf_dict_set(&h->doc.doc_arena, widget, "V", str_obj);
        mark_touched(h, obj_num, obj_gen, widget);

        new_num = h->next_new_obj_num++;
        new_ap = pdf_form_generate_text_appearance(&h->st, &h->xref, &h->doc.doc_arena,
                                                     widget, new_value, new_num);
        if (new_ap != NULL)
        {
            ap_dict = pdf_dict_get(widget, "AP");
            if (ap_dict == NULL || ap_dict->type != PDF_DICT)
            {
                ap_dict = pdf_obj_new_dict(&h->doc.doc_arena);
                pdf_dict_set(&h->doc.doc_arena, widget, "AP", ap_dict);
            }
            pdf_dict_set(&h->doc.doc_arena, ap_dict, "N", new_ap);
            mark_touched(h, new_num, 0, new_ap);
        }
        hb_retl(HB_TRUE);
        return;
    }

    hb_retl(HB_FALSE);
}

/* Pdf_FormSave( pDoc, cRuta ) -> lReturn
 *     Escribe los campos editados de vuelta a 'cRuta' (incremental
 *     update, ver pdf_write.h) -- tipicamente el mismo archivo que se
 *     abrio (sobrescribe el original; el original se preserva como
 *     'cRuta.bak', ver pdf_write.h). Se niega (.F.) si el documento no
 *     tiene ninguna edicion pendiente, o si esta encriptado (escribir
 *     contenido nuevo en un PDF encriptado exige direccion ENCRYPT,
 *     fuera de alcance de esta etapa -- ver DESIGN.md). */
HB_FUNC(PDF_FORMSAVE)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    const char *out_path = hb_parc(2);
    pdf_obj *touched_objs[PDF_HB_MAX_TOUCHED_OBJS];
    long touched_nums[PDF_HB_MAX_TOUCHED_OBJS];
    long touched_gens[PDF_HB_MAX_TOUCHED_OBJS];
    int i, rc;

    if (h == NULL || !h->open || out_path == NULL || h->n_touched <= 0)
    {
        hb_retl(HB_FALSE);
        return;
    }

    if (h->xref.crypt.active)
    {
        /* limitacion de alcance explicita -- ver DESIGN.md */
        hb_retl(HB_FALSE);
        return;
    }

    for (i = 0; i < h->n_touched; i++)
    {
        touched_objs[i] = h->touched[i].obj_ptr;
        touched_nums[i] = h->touched[i].obj_num;
        touched_gens[i] = h->touched[i].obj_gen;
    }

    rc = pdf_write_incremental_update(&h->st, &h->xref, &h->doc.doc_arena,
                                       touched_objs, touched_nums, touched_gens,
                                       h->n_touched, out_path);

    hb_retl(rc == PDF_OK ? HB_TRUE : HB_FALSE);
}

/* Pdf_AnnotAddHighlight( pDoc, nPagina, aRects ) -> lReturn
 *     Agrega una anotacion /Highlight nueva a la pagina 'nPagina',
 *     cubriendo los rectangulos de 'aRects' (array de {x0,y0,x1,y1},
 *     4 numeros cada uno -- MISMO formato "topdown" local a la pagina
 *     que las columnas 4-7 de TPdfBitmap:aSelRanges en pdf_viewer.prg,
 *     un rect por linea visual de texto seleccionada). No escribe nada
 *     al archivo todavia -- solo muta el documento EN MEMORIA (mismo
 *     mecanismo "touched objects" que Pdf_FormSetFieldValue); hace
 *     falta llamar Pdf_FormSave() despues para persistir, igual que
 *     una edicion de AcroForm (mismo boton "Guardar" de la UI, generico
 *     -- ver DESIGN.md).
 *
 *     Color/opacidad fijos en esta primera version: amarillo (1,1,0)
 *     con blend Multiply al 40% -- sin selector de color todavia. */
HB_FUNC(PDF_ANNOTADDHIGHLIGHT)
{
    pdf_hb_doc *h = (pdf_hb_doc *)hb_parptr(1);
    int page_index = hb_parni(2) - 1;
    PHB_ITEM aRects = hb_param(3, HB_IT_ARRAY);
    pdf_obj *page_obj, *mediabox, *cropbox, *rotate_obj;
    long page_obj_num = -1;
    double media_x0, media_y0, media_x1, media_y1;
    double crop_x0, crop_y0, crop_x1, crop_y1;
    double page_w, page_h;
    int rotate;
    HB_SIZE n_rects, ri;
    int n_quads;
    double quads[PDF_ANNOT_MAX_QUADS * 8];
    double bbox[4];
    long ap_num, annot_num;
    pdf_obj *ap_obj, *annot_obj;
    pdf_obj *annots;

    if (h == NULL || !h->open || page_index < 0 || aRects == NULL)
    {
        hb_retl(HB_FALSE);
        return;
    }

    /* Guardas tempranas -- ver DESIGN.md (fase de resaltado de texto):
     * documento encriptado (misma limitacion que Pdf_FormSave, escribir
     * contenido nuevo exige direccion ENCRYPT, fuera de alcance);
     * cache de resolucion de objetos caido (ver pdf_xref.h) -- sin el,
     * una segunda mutacion de la MISMA pagina en esta sesion (otro
     * resaltado, o un re-render) resolveria una copia INDEPENDIENTE en
     * vez del mismo puntero ya mutado, perdiendo la primera mutacion
     * silenciosamente; headroom en touched[] -- esta operacion necesita
     * hasta 3 slots nuevos (apariencia, anotacion, pagina-o-array). */
    if (h->xref.crypt.active)
    {
        hb_retl(HB_FALSE);
        return;
    }
    if (h->xref.resolved == NULL)
    {
        hb_retl(HB_FALSE);
        return;
    }
    if (h->n_touched + 3 > PDF_HB_MAX_TOUCHED_OBJS)
    {
        hb_retl(HB_FALSE);
        return;
    }

    page_obj = pdf_document_get_page_ex(&h->st, &h->xref, &h->doc.doc_arena, page_index, &page_obj_num);
    if (page_obj == NULL || page_obj_num <= 0)
    {
        hb_retl(HB_FALSE);
        return;
    }

    /* /CropBox intersectado con /MediaBox, /Rotate heredado -- MISMO
     * calculo que PDF_GETPAGESIZEPT/PDF_RENDERTOHBITMAP/
     * pdf_text_extract_page (4 copias del mismo bloque en el motor,
     * cada una con este comentario cruzado -- si se corrige un bug aca
     * corregirlo en las otras 3 tambien). A diferencia de esas, NO se
     * intercambian ancho/alto para /Rotate 90/270: 'page_w'/'page_h'
     * tienen que ser las dimensiones NATIVAS (sin rotar), que es lo que
     * espera pdf_render_topdown_to_native() (mismo criterio que
     * dev->page_width_pt/page_height_pt en pdf_render.c, que tampoco
     * se intercambian). */
    mediabox = pdf_dict_get(page_obj, "MediaBox");
    if (mediabox != NULL && mediabox->type == PDF_ARRAY && mediabox->u.arr.count == 4)
    {
        media_x0 = obj_num(mediabox->u.arr.items[0]);
        media_y0 = obj_num(mediabox->u.arr.items[1]);
        media_x1 = obj_num(mediabox->u.arr.items[2]);
        media_y1 = obj_num(mediabox->u.arr.items[3]);
        if (media_x1 < media_x0) { double t = media_x0; media_x0 = media_x1; media_x1 = t; }
        if (media_y1 < media_y0) { double t = media_y0; media_y0 = media_y1; media_y1 = t; }
    }
    else
    {
        media_x0 = 0.0; media_y0 = 0.0; media_x1 = 612.0; media_y1 = 792.0;
    }

    crop_x0 = media_x0; crop_y0 = media_y0; crop_x1 = media_x1; crop_y1 = media_y1;
    cropbox = pdf_page_get_inherited(&h->st, &h->xref, &h->doc.doc_arena, page_obj, "CropBox");
    if (cropbox != NULL && cropbox->type == PDF_ARRAY && cropbox->u.arr.count == 4)
    {
        double cx0 = obj_num(cropbox->u.arr.items[0]);
        double cy0 = obj_num(cropbox->u.arr.items[1]);
        double cx1 = obj_num(cropbox->u.arr.items[2]);
        double cy1 = obj_num(cropbox->u.arr.items[3]);
        if (cx1 < cx0) { double t = cx0; cx0 = cx1; cx1 = t; }
        if (cy1 < cy0) { double t = cy0; cy0 = cy1; cy1 = t; }
        if (cx0 > media_x0) crop_x0 = cx0;
        if (cy0 > media_y0) crop_y0 = cy0;
        if (cx1 < media_x1) crop_x1 = cx1;
        if (cy1 < media_y1) crop_y1 = cy1;
    }
    if (crop_x1 <= crop_x0) { crop_x0 = media_x0; crop_x1 = media_x1; }
    if (crop_y1 <= crop_y0) { crop_y0 = media_y0; crop_y1 = media_y1; }

    page_w = crop_x1 - crop_x0;
    page_h = crop_y1 - crop_y0;

    rotate = 0;
    rotate_obj = pdf_page_get_inherited(&h->st, &h->xref, &h->doc.doc_arena, page_obj, "Rotate");
    if (rotate_obj != NULL)
        rotate = pdf_normalize_rotation((int)obj_num(rotate_obj));

    /* Cada rect de 'aRects' (topdown local) -> un quad TL/TR/BL/BR en
     * espacio nativo. No se asume que el orden de esquinas sobrevive
     * la rotacion -- se transforman AMBOS puntos y se toma min/max. */
    n_rects = hb_arrayLen(aRects);
    n_quads = 0;
    bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0.0;

    for (ri = 1; ri <= n_rects && n_quads < PDF_ANNOT_MAX_QUADS; ri++)
    {
        PHB_ITEM aRow = hb_arrayGetItemPtr(aRects, ri);
        double tx0, ty0, tx1, ty1;
        double nx0, ny0, nx1, ny1;
        double rx0, ry0, rx1, ry1;
        double *q;

        if (aRow == NULL || !HB_IS_ARRAY(aRow) || hb_arrayLen(aRow) < 4)
            continue;

        tx0 = hb_arrayGetND(aRow, 1);
        ty0 = hb_arrayGetND(aRow, 2);
        tx1 = hb_arrayGetND(aRow, 3);
        ty1 = hb_arrayGetND(aRow, 4);

        if (!pdf_render_topdown_to_native(rotate, crop_x0, crop_y0, page_w, page_h,
                                           tx0, ty0, &nx0, &ny0))
            continue;
        if (!pdf_render_topdown_to_native(rotate, crop_x0, crop_y0, page_w, page_h,
                                           tx1, ty1, &nx1, &ny1))
            continue;

        rx0 = (nx0 < nx1) ? nx0 : nx1;
        rx1 = (nx0 < nx1) ? nx1 : nx0;
        ry0 = (ny0 < ny1) ? ny0 : ny1;
        ry1 = (ny0 < ny1) ? ny1 : ny0;

        if (n_quads == 0)
        {
            bbox[0] = rx0; bbox[1] = ry0; bbox[2] = rx1; bbox[3] = ry1;
        }
        else
        {
            if (rx0 < bbox[0]) bbox[0] = rx0;
            if (ry0 < bbox[1]) bbox[1] = ry0;
            if (rx1 > bbox[2]) bbox[2] = rx1;
            if (ry1 > bbox[3]) bbox[3] = ry1;
        }

        /* TL,TR,BL,BR -- mismo orden de QuadPoints (ver pdf_annot.h). */
        q = &quads[n_quads * 8];
        q[0] = rx0; q[1] = ry1; /* TL */
        q[2] = rx1; q[3] = ry1; /* TR */
        q[4] = rx0; q[5] = ry0; /* BL */
        q[6] = rx1; q[7] = ry0; /* BR */
        n_quads++;
    }

    if (n_quads == 0)
    {
        hb_retl(HB_FALSE);
        return;
    }

    ap_num = h->next_new_obj_num++;
    annot_num = h->next_new_obj_num++;

    ap_obj = pdf_annot_generate_highlight_appearance(&h->doc.doc_arena, quads, n_quads,
                                                       1.0, 1.0, 0.0, 0.4, ap_num, bbox);
    if (ap_obj == NULL)
    {
        hb_retl(HB_FALSE);
        return;
    }

    annot_obj = pdf_annot_new_highlight(&h->doc.doc_arena, bbox, quads, n_quads,
                                          1.0, 1.0, 0.0, ap_obj);
    if (annot_obj == NULL)
    {
        hb_retl(HB_FALSE);
        return;
    }

    /* Resolver /Annots de la pagina -- 3 casos (ver DESIGN.md). El gen
     * de cualquier objeto tocado sale SIEMPRE de la tabla xref
     * (pdf_xref_entry_at), nunca asumido 0 ni tomado de un PDF_REF
     * literal. */
    annots = pdf_dict_get(page_obj, "Annots");
    if (annots != NULL && annots->type == PDF_REF)
    {
        long annots_num = annots->u.ref.num;
        const pdf_xref_entry *annots_entry;

        annots = pdf_xref_load_object(&h->st, &h->xref, annots_num, &h->doc.doc_arena);
        if (annots == NULL || annots->type != PDF_ARRAY)
        {
            hb_retl(HB_FALSE);
            return;
        }
        if (pdf_array_push(&h->doc.doc_arena, annots, pdf_obj_new_ref(&h->doc.doc_arena, annot_num, 0)) != PDF_OK)
        {
            hb_retl(HB_FALSE);
            return;
        }
        annots_entry = pdf_xref_entry_at(&h->xref, annots_num);
        mark_touched(h, annots_num, (annots_entry != NULL) ? annots_entry->gen : 0, annots);
    }
    else
    {
        const pdf_xref_entry *page_entry;

        if (annots == NULL || annots->type != PDF_ARRAY)
        {
            annots = pdf_obj_new_array(&h->doc.doc_arena, 4);
            pdf_dict_set(&h->doc.doc_arena, page_obj, "Annots", annots);
        }
        if (pdf_array_push(&h->doc.doc_arena, annots, pdf_obj_new_ref(&h->doc.doc_arena, annot_num, 0)) != PDF_OK)
        {
            hb_retl(HB_FALSE);
            return;
        }
        page_entry = pdf_xref_entry_at(&h->xref, page_obj_num);
        mark_touched(h, page_obj_num, (page_entry != NULL) ? page_entry->gen : 0, page_obj);
    }

    mark_touched(h, ap_num, 0, ap_obj);
    mark_touched(h, annot_num, 0, annot_obj);

    hb_retl(HB_TRUE);
}
