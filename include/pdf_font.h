/* pdf_font.h
 *
 * Parseo de recursos de fuente PDF "simple" (1 byte por caracter --
 * Type1/TrueType con /Encoding tipo WinAnsiEncoding/MacRomanEncoding/
 * diferencias custom) y compuesta (Type0/CID, 2 bytes/caracter, ver
 * 'is_cid'). Extrae lo necesario para medir el avance de texto
 * correctamente (/Widths, /FirstChar, /LastChar) y para render real:
 * si /FontDescriptor trae /FontFile2 (TrueType embebido), este modulo
 * lo descomprime y lo parsea con pdf_ttf_load (ver pdf_ttf.h) hacia
 * 'embedded_ttf' -- el llamador (pdf_render.c) usa eso mas
 * pdf_ttf_glyph_outline para dibujar contornos reales. Si no hay
 * /FontFile2 (el caso mas comun: PDFs reales que dependen de
 * sustitucion por nombre), 'has_embedded_ttf' queda 0 y el llamador
 * debe sustituir con una fuente de sistema real via
 * pdf_ttf_find_system_font -- NO por GDI, ver pdf_ttf.h.
 *
 * Si /FontDescriptor trae /FontFile3 (CFF/Type1C embebido) en vez de
 * /FontFile2, este modulo tambien lo descomprime y lo parsea con
 * pdf_cff_load (ver pdf_cff.h) hacia 'embedded_cff' -- el llamador usa
 * pdf_cff_glyph_outline (interprete completo de charstrings Type 2)
 * para dibujar esos contornos, mismo nivel de fidelidad que TrueType.
 * Este modulo en si NO parsea Type1 puro (charstrings Type 1 clasicas,
 * /FontFile) ni CFF CID-keyed (ROS/FDArray/FDSelect) -- solo extrae/
 * descomprime los bytes, delegando el parseo de contornos a pdf_ttf.c/
 * pdf_cff.c segun corresponda (separacion de responsabilidades: este
 * modulo sabe de PDF, pdf_ttf.c/pdf_cff.c saben de sfnt/CFF).
 */

#ifndef PDF_FONT_H
#define PDF_FONT_H

#include "pdf_object.h"
#include "pdf_stream.h"
#include "pdf_parser.h"
#include "pdf_ttf.h"
#include "pdf_cff.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_FONT_NAME_MAX 128

typedef struct pdf_font_s
{
    char   base_font[PDF_FONT_NAME_MAX]; /* p.ej. "TimesNewRomanPSMT", "Arial-BoldMT" */
    int    first_char;
    int    last_char;
    double widths[256];      /* en milesimos de unidad de texto (escala 1000/em), -1 = no definido */
    double missing_width;
    int    is_bold;          /* heuristica desde el nombre (ver pdf_font_load) */
    int    is_italic;
    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 65): cuando /BaseFont
     * es un nombre generico sin ninguna pista reconocible (p.ej.
     * "CIDFont+F1", confirmado en un PDF real -- el subconjunto no
     * conserva el nombre real de la fuente original), el backend GDI
     * no tiene forma de elegir una fuente sustituta razonable por
     * NOMBRE -- pero el /FontDescriptor SIEMPRE trae el flag /Flags
     * (bit 2 = Serif), que SI da una pista util incluso sin nombre:
     * al menos permite elegir entre una familia serif (Times New
     * Roman) o sans-serif (Arial) como fallback, en vez de un nombre
     * literal como "F1" que casi seguro no existe como fuente
     * instalada (GDI cae a lo que sea que tenga por defecto,
     * tipicamente sans-serif, mientras el documento necesitaba
     * serif -- un desajuste de proporciones de letra mucho mayor que
     * el esperable entre dos fuentes sans-serif similares, causando
     * un escalado horizontal por glyph extremo y visualmente
     * "sucio"). */
    int    is_serif;

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 55): fuentes con
     * subconjunto embebido (comun en PDFs exportados de PowerPoint,
     * nombre con prefijo de 6 letras tipo "BAAAAA+") suelen NO traer
     * /Encoding -- los codigos de caracter (0,1,2...) son indices
     * arbitrarios y COMPACTADOS que el generador eligio para ESE
     * subconjunto especifico, sin relacion con ningun estandar
     * (WinAnsi, MacRoman, ni siquiera ASCII). Pasar esos codigos
     * directo a GDI como si fueran ANSI (que es lo que hacia este
     * motor) busca glyphs que no existen en la posicion pedida --
     * resultado: "tofu" (cuadraditos de glyph faltante) en vez de
     * texto legible. La solucion (sin necesidad de parsear los
     * contornos del subconjunto embebido, fuera de alcance -- ver
     * comentario del archivo): estas fuentes casi siempre traen
     * /ToUnicode, un CMap que SI mapea cada codigo a su caracter
     * Unicode real (para que copiar/pegar y buscar texto funcionen en
     * cualquier visor) -- si lo leemos, podemos pedirle a GDI el
     * caracter UNICODE REAL (via TextOutW en vez de TextOutA) en la
     * fuente sustituta, sin importar que codigos arbitrarios use el
     * subconjunto original. to_unicode[code]=0 significa "sin mapeo,
     * usar el codigo tal cual" (compatible con el comportamiento
     * anterior para fuentes con /Encoding estandar, que no necesitan
     * esto) -- SALVO que 'has_custom_diff[code]' este seteado, ver
     * comentario ahi debajo. */
    unsigned short to_unicode[256];

    /* BUG REAL ENCONTRADO (render de fuentes real, PDFs de editoriales
     * tipo Elsevier -- confirmado contra
     * Utilization_and_efficiency_of_ground_gra.pdf): /Encoding puede
     * traer /Differences remapeando un codigo a un glyph CUSTOM del
     * font original (p.ej. "/C223", una ligadura "ffi" o un simbolo
     * propio de la fuente Type1/CFF embebida, /FontFile3 -- fuera de
     * alcance de este motor, solo TrueType 'glyf'), SIN que el PDF
     * traiga /ToUnicode que diga que caracter Unicode representa
     * realmente ese codigo. Antes de esta ronda esto era inofensivo
     * (todo texto se dibujaba como caja placeholder, sin importar que
     * codigo tuviera), pero con glyphs reales pasar el codigo crudo
     * como si fuera Unicode (fallback documentado arriba) puede
     * coincidir por pura casualidad con una letra Latin-1 real (p.ej.
     * codigo 223 remapeado a un glyph custom coincide con 'ß' en
     * WinAnsi/Latin-1) -- resultado: se dibuja confiadamente la letra
     * EQUIVOCADA en vez de simplemente no saber que dibujar.
     * 'has_custom_diff[code]'=1 marca los codigos que /Differences
     * remapeo (sin importar el nombre del glyph -- no se mantiene una
     * Adobe Glyph List completa, ver pdf_font_load) -- pdf_font_get_unicode
     * devuelve 0xFFFD (REPLACEMENT CHARACTER) para esos codigos
     * cuando no hay /ToUnicode que los desambigue, en vez del codigo
     * crudo -- el llamador (pdf_render.c) no encuentra ese glyph en
     * ninguna fuente real y cae a la caja placeholder, honesto en vez
     * de una letra equivocada. */
    unsigned char has_custom_diff[256];

    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 62): fuentes
     * compuestas (/Type0, "CID-keyed") -- MUY comunes en PDFs
     * generados por herramientas modernas de reportes/formularios
     * (confirmado en un PDF real, Persona-Juridica-THERCONSULT...pdf,
     * generado por alguna libreria de reportes). A diferencia de las
     * fuentes "simples" de arriba (1 byte = 1 caracter), en una
     * fuente Type0 con /Encoding /Identity-H cada caracter en el
     * string de Tj/TJ ocupa 2 BYTES (un CID de 16 bits, big-endian).
     * Sin este campo, el motor lee 1 byte a la vez como si fuera una
     * fuente simple -- el resultado es leer "medio caracter" por
     * vez, produciendo texto con letras mezcladas/duplicadas sin
     * relacion aparente con el texto real (exactamente el sintoma
     * reportado). Los arrays de abajo son analogos a 'widths' y
     * 'to_unicode' de arriba pero indexados por CID completo
     * (0-65535, no 0-255) -- se alocan en la arena SOLO si la fuente
     * es CID (is_cid=1), para no gastar memoria en el caso comun
     * (fuente simple). */
    int             is_cid;
    /* BUG REAL ENCONTRADO Y ARREGLADO: la asuncion "toda fuente Type0
     * usa 2 bytes por caracter" (valida para /Identity-H estandar) es
     * FALSA para una variante real vista en PDFs generados por ciertas
     * herramientas (confirmado con tests/Los_Kajchas_y_los_proyectos_
     * de_industria.pdf): el /BaseFont declara explicitamente
     * "...OneByteIdentityH" (p.ej. "SKZRKD+Candara-Italic-
     * OneByteIdentityH") -- el CID mapea 1 a 1 con el CODIGO PDF, pero
     * el string de Tj/TJ trae 1 byte por caracter, no 2. Leer 2 bytes
     * de todos modos produce exactamente el patron reportado (texto
     * "ilegible", como barras/rayas): se combinan pares de caracteres
     * en un unico CID que no tiene relacion con ninguno de los dos
     * originales. Detectado en pdf_font_load() buscando el sufijo
     * "OneByteIdentityH" en el nombre (sin subset-prefix) -- no hay
     * forma mas robusta de detectarlo sin parsear el CMap embebido
     * (fuera de alcance), pero el nombre es exactamente la senial que
     * la herramienta que genero este PDF deja a proposito para esto. */
    int             is_cid_one_byte;
    double          cid_default_width; /* /DW del CIDFont descendiente, 1000 si ausente (default del estandar) */
    float          *cid_widths;        /* NULL o arena[65536], -1.0 = sin entrada explicita en /W (usar cid_default_width) */
    unsigned short *cid_to_unicode;    /* NULL o arena[65536], 0 = sin mapeo (usar el CID tal cual, mismo criterio que to_unicode) */

    /* CID -> glyph index real, via /CIDToGIDMap del CIDFont
     * descendiente. NULL significa identidad (GID=CID directo, el
     * default del estandar cuando /CIDToGIDMap es /Identity o esta
     * ausente -- el caso mas comun). Si NO es NULL, es un arena[65536]
     * con el GID real por CID (decodificado del stream /CIDToGIDMap,
     * 2 bytes big-endian por entrada segun T.32000-1 9.7.4.4). Solo
     * tiene sentido junto con 'has_embedded_ttf'. */
    unsigned short *cid_to_gid;

    /* Programa de fuente TrueType embebido (/FontDescriptor
     * /FontFile2), ya descomprimido y parseado -- ver pdf_ttf.h.
     * 'has_embedded_ttf' es 0 si no habia /FontFile2, el filtro no es
     * soportado (algo distinto de FlateDecode/ninguno), o el parseo
     * sfnt fallo (fuente corrupta, o CFF/OpenType sin 'glyf') -- en
     * cualquiera de esos casos el llamador debe sustituir con una
     * fuente de sistema (pdf_ttf_find_system_font), nunca asumir que
     * 'embedded_ttf' es valido sin chequear este flag primero. */
    int          has_embedded_ttf;
    pdf_ttf_font embedded_ttf;

    /* Programa de fuente CFF embebido (/FontDescriptor /FontFile3,
     * /Subtype /Type1C -- CFF "simple", no CID-keyed), ya
     * descomprimido y parseado -- ver pdf_cff.h. Analogo a
     * 'has_embedded_ttf'/'embedded_ttf' de arriba; el llamador
     * (pdf_render.c) intenta primero embedded_ttf, despues
     * embedded_cff, y si ninguno aplica sustituye con una fuente de
     * sistema. 'data'/'data_len' dentro de embedded_cff apuntan a
     * memoria de la MISMA arena que 'font' (ver pdf_font_load), asi
     * que siguen validos mientras 'font' este vivo. */
    int          has_embedded_cff;
    pdf_cff_font embedded_cff;
} pdf_font;

/* Parsea 'font_dict' hacia 'font'. Copia todo lo que necesita (no
 * guarda punteros a la arena de origen), asi que 'font' se puede
 * cachear con vida propia.
 *
 * 'st'/'xref'/'arena': BUG REAL ENCONTRADO Y ARREGLADO (ver DESIGN.md
 * seccion 49) -- la version anterior de esta funcion asumia que
 * 'font_dict' llegaba "sin referencias indirectas pendientes en las
 * claves que se leen", pero eso es falso en PDFs reales: es comun
 * (confirmado en Flujograma_Propuesto.pdf, los 4 fonts de la pagina)
 * que /Widths sea en si misma una referencia indirecta ("10 0 R") en
 * vez de un array inline, aunque el /Font dict que la contiene ya este
 * resuelto. Sin 'st'/'xref'/'arena' esta funcion no tiene como
 * dereferenciar eso, y el chequeo 'widths->type == PDF_ARRAY' fallaba
 * silenciosamente, dejando font->widths[*] en -1 para TODOS los
 * codigos -- pdf_font_get_width devolvia entonces el generico 500
 * parejo para cada caracter, sin importar la letra. Ahora estos tres
 * parametros permiten resolver /Widths (y /MissingWidth, por las
 * dudas) si vienen como referencia, igual que ya se hacia para el
 * /Font dict en si (ver find_font_dict en pdf_render.c). Devuelve
 * PDF_OK o error. */
int pdf_font_load(pdf_obj *font_dict, pdf_font *font,
                   pdf_stream *st, const pdf_xref_table *xref, pdf_arena *arena);

/* Ancho de avance del codigo de caracter 'code', en milesimos de
 * unidad de texto -- dividir por 1000 y multiplicar por el tamanio de
 * fuente para obtener el avance real en espacio de texto. Para
 * fuentes simples 'code' es 0-255; para fuentes CID (is_cid=1) es el
 * CID completo, 0-65535 (ver comentario de 'is_cid' en el struct). Si
 * 'font' es NULL o no tiene el ancho definido, devuelve un valor
 * generico razonable (500, o el /DW de la fuente CID) para no trabar
 * el layout. */
double pdf_font_get_width(const pdf_font *font, int code);

/* Devuelve el codepoint Unicode real para 'code' (0-255 para fuentes
 * simples, CID completo 0-65535 para fuentes CID) segun el
 * /ToUnicode CMap del font (ver comentario de 'to_unicode'/
 * 'cid_to_unicode' arriba). Si no hay mapeo para ese codigo (fuente
 * con /Encoding estandar, o CMap ausente/no parseable), devuelve
 * 'code' tal cual -- asi el llamador puede usar SIEMPRE esta funcion
 * sin chequear casos especiales. */
int pdf_font_get_unicode(const pdf_font *font, int code);

#ifdef __cplusplus
}
#endif

#endif /* PDF_FONT_H */
