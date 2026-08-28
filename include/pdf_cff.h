/* pdf_cff.h
 *
 * Lector de METADATA de nombres de glyph de un programa de fuente CFF
 * (Compact Font Format -- /FontFile3, /Subtype /Type1C), acotado a
 * Encoding + Charset + String INDEX. NO parsea CharStrings/contornos
 * -- este motor sigue sin dibujar glyphs Type1/CFF (solo TrueType
 * 'glyf', ver pdf_ttf.h); esto es solo para resolver, para un CODIGO
 * de caracter dado, el NOMBRE real que la fuente le puso a su glyph.
 *
 * Motivo (ver DESIGN.md, ronda "render de fuentes real"): ciertas
 * herramientas de maquetacion editorial (confirmado en un PDF real de
 * Elsevier/Arbortext, fuentes "Adv*") ubican ligaduras ("ffi", "fi")
 * en posiciones NO USADAS del WinAnsiEncoding estandar declarado
 * (p.ej. codigo 222, nominalmente "Thorn"), sin avisar por
 * /Differences ni /ToUnicode -- el PDF, por si solo, no da ninguna
 * pista de que ese codigo no es realmente esa letra. Adobe Acrobat lo
 * resuelve leyendo el Encoding INTERNO del programa de fuente
 * embebido (que si sabe que ese codigo se llama "ffi"); este modulo
 * replica ESA parte especifica (nombres, no contornos) para poder
 * completar pdf_font->to_unicode con la ligadura Unicode real en vez
 * de dejar pasar el codigo crudo (que puede coincidir por casualidad
 * con una letra Latin-1 real y mostrarse con confianza equivocada).
 *
 * Alcance deliberadamente chico: CFF "simple" (no CID-keyed -- si el
 * Top DICT trae el operador ROS, se devuelve 0, fuera de alcance),
 * Encoding/Charset CUSTOM (los predefinidos Standard/Expert/ISOAdobe
 * no hacen falta para el caso que motiva esto: una ligadura custom
 * SIEMPRE vive en un Encoding/Charset propio de la fuente). Tolerante
 * en cada paso: cualquier dato fuera de rango o con forma inesperada
 * hace que la funcion devuelva 0 sin crashear -- el llamador
 * (pdf_font.c) simplemente no completa ese codigo.
 */

#ifndef PDF_CFF_H
#define PDF_CFF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resuelve el nombre de glyph para 'code' (0-255) segun el
 * Encoding+Charset+String INDEX del programa CFF en 'data'/'len' (ya
 * descomprimido, tal cual sale de /FontFile3). Copia el nombre (hasta
 * out_size-1 bytes, con terminador) a 'out'. Devuelve 1 si tuvo
 * exito, 0 si no -- en particular, devuelve 0 (a proposito, ver
 * comentario de archivo) para nombres ESTANDAR (SID<391, letras/
 * digitos/puntuacion comunes: no hace falta resolverlos aca, el
 * codigo crudo via WinAnsiEncoding ya los identifica bien) -- solo
 * tiene exito para nombres CUSTOM de ESTA fuente especifica (SID>=391),
 * que es exactamente donde viven las ligaduras/simbolos que motivan
 * este modulo. */
int pdf_cff_glyph_name_for_code(const unsigned char *data, long len,
                                 int code, char *out, int out_size);

/* ---- Contornos reales (interprete de Type 2 charstrings) ------------
 *
 * A pedido explicito (ver DESIGN.md, ronda "render de fuentes real"):
 * el caso que motivo pdf_cff_glyph_name_for_code (arriba) resulto
 * tener un limite real -- hay codigos de caracter que el programa CFF
 * ni siquiera declara en su propio Encoding (ni por nombre custom ni
 * por SID estandar), asi que NINGUNA resolucion de nombre alcanza; la
 * unica forma de mostrar ese glyph es dibujar su contorno REAL
 * (CharStrings, formato Type 2 -- T.5177.Type2.pdf), ignorando
 * Encoding/nombres por completo y yendo directo Charset (code->GID
 * via Encoding cuando SI existe) -> CharStrings[GID] -> bytecode.
 *
 * Alcance: CFF "simple" (no CID-keyed / CIDFontType0C -- si el Top
 * DICT trae ROS, pdf_cff_load falla a proposito, ver comentario ahi;
 * un CFF CID-keyed tiene FDArray/FDSelect en vez de un Private DICT
 * unico, una estructura bastante distinta, fuera de alcance). Se
 * implementan TODOS los operadores de construccion de path (moveto/
 * lineto/curveto en sus variantes, incluidos los 4 operadores de
 * "flex"), hints (hstem/vstem/hintmask/cntrmask -- se leen solo para
 * saber cuantos bytes ocupan y saltarlos bien, el hinting en si se
 * ignora, este motor no hace fitting a grilla de pixeles) y
 * subrutinas locales/globales (con el sesgo de indice que exige el
 * spec). NO se implementa 'seac' (composicion antigua de acentos via
 * 'endchar' con 4 argumentos, deprecado) ni los operadores
 * aritmeticos/logicos (and/or/not/add/etc, para glyphs "programados"
 * -- rarisimos en fuentes reales) -- si aparecen, se ignoran
 * tolerantemente (glyph incompleto en vez de crashear).
 */

typedef struct pdf_cff_font_s
{
    const unsigned char *data;
    long data_len;

    long cs_count;       /* CharStrings INDEX: 1 item = 1 glyph, indexado por GID */
    int  cs_off_size;
    long cs_offsets_start;
    long cs_data_start;

    long gs_count;        /* Global Subr INDEX */
    int  gs_off_size;
    long gs_offsets_start;
    long gs_data_start;
    int  gs_bias;

    long ls_count;        /* Local Subr INDEX (via Private DICT) -- count=0 si no hay */
    int  ls_off_size;
    long ls_offsets_start;
    long ls_data_start;
    int  ls_bias;

    long charset_off;     /* 0/1/2 = predefinido (ISOAdobe/Expert/ExpertSubset) */
    long encoding_off;    /* 0/1 = predefinido (Standard/Expert) */

    double units_per_em;  /* de FontMatrix (Top DICT), default 1000.0 */
    int    num_glyphs;

    /* Private DICT nominalWidthX/defaultWidthX (operadores 21/20) --
     * ver comentario grande junto a pdf_cff_glyph_width. Default 0.0/0.0
     * (igual que el spec) si el Private DICT no los trae. */
    double nominal_width_x;
    double default_width_x;
} pdf_cff_font;

/* Parsea 'data'/'len' (programa CFF ya descomprimido, tal cual sale de
 * /FontFile3) hacia 'out'. Devuelve PDF_OK o error -- en particular,
 * falla a proposito (fuera de alcance) si el Top DICT trae ROS
 * (CID-keyed) o si falta CharStrings. 'data' debe seguir vivo mientras
 * se use 'out' (igual criterio que pdf_ttf_load, ver pdf_ttf.h). */
int pdf_cff_load(const unsigned char *data, long len, pdf_cff_font *out);

/* Codigo de caracter (0-255) -> glyph index, via el Encoding embebido
 * (formato 0/1, con soporte de supplements -- ver pdf_cff_glyph_name_for_code
 * para el mismo mecanismo aplicado a nombres). Devuelve 0 si el codigo
 * no tiene entrada en el Encoding de esta fuente (glyph .notdef,
 * mismo criterio que pdf_ttf_gid_for_unicode) -- el llamador cae a
 * sustitucion de sistema. */
int pdf_cff_gid_for_code(const pdf_cff_font *font, int code);

typedef void (*pdf_cff_moveto_fn)(void *user, double x, double y);
typedef void (*pdf_cff_lineto_fn)(void *user, double x, double y);
typedef void (*pdf_cff_curveto_fn)(void *user, double x1, double y1,
                                    double x2, double y2, double x3, double y3);

/* Extrae el contorno del glyph 'gid' (Type 2 charstring, ejecutado con
 * soporte completo de subrutinas/flex/hints-como-skip) y lo entrega
 * via 'moveto'/'lineto'/'curveto' (curvas CUBICAS, a diferencia de
 * pdf_ttf_glyph_outline que aplana a lineas -- el llamador puede pasar
 * estos puntos directo a pdf_path_curveto, que ya aplana cubicas, ver
 * pdf_path.h), en espacio de glyph normalizado a em (coordenadas /
 * units_per_em, igual convencion que pdf_ttf.h). Devuelve PDF_OK,
 * PDF_ERR_NOTFOUND (gid invalido o glyph vacio -- p.ej. espacio, no es
 * error real) o PDF_ERR_BADARG/UNSUPPORTED (charstring corrupto o con
 * datos fuera de rango) -- tolerante en cada paso, nunca crashea. */
int pdf_cff_glyph_outline(const pdf_cff_font *font, int gid,
                           pdf_cff_moveto_fn moveto, pdf_cff_lineto_fn lineto,
                           pdf_cff_curveto_fn curveto, void *user);

/* BUG REAL ENCONTRADO (ver DESIGN.md, ronda "fuentes con /Widths
 * incompleto"): confirmado contra un PDF real
 * (Agentes_de_inteligencia_artificial_y_workflows.pdf) exportado desde
 * Adobe InDesign -- un mismo font subset (BiomePro-SemiBold, via
 * FontFile3) aparece declarado en MULTIPLES objetos /Font distintos
 * (uno por cada cuadro de texto/Form XObject donde InDesign lo usa),
 * cada uno con SU PROPIO array /Widths recortado a los codigos que ESE
 * cuadro de texto usa -- pero al menos uno de esos arrays quedo mal
 * generado por InDesign: varios codigos REALMENTE USADOS en ese cuadro
 * (confirmado 'L' mayuscula, entre otros) declaran ancho 0 en vez del
 * ancho real, pese a que el glyph SI existe y se dibuja bien (el
 * contorno viene de este mismo programa CFF). Con ancho declarado 0,
 * el motor pisa cada glyph sobre el siguiente (avance nulo) --
 * exactamente el sintoma reportado ("no hay una muestra perfecta de
 * los fonts de este documento", encabezado con letras superpuestas).
 * Confirmado con una referencia real (fontTools, parseando este mismo
 * programa CFF): el glyph 'L' SI tiene un ancho real de 612 unidades
 * codificado en su propio charstring -- exactamente el mismo valor que
 * otro objeto /Font (con /Widths completo) para ESTE MISMO glyph en
 * ESTE MISMO documento. La corrupcion es del /Widths de ESE objeto en
 * particular (un defecto real del PDF, no de este parser -- ver
 * pdf_xref.c/pdf_font.c, se investigo y descarto una miscompilacion de
 * bcc32 antes de llegar a esta conclusion), asi que la unica forma de
 * mostrar el texto correctamente es no confiar ciegamente en un ancho
 * declarado de 0 cuando el font PROGRAMA embebido sabe algo mejor.
 * Extrae el ancho de avance REAL del glyph 'gid', tal como esta
 * codificado en su propio Type 2 charstring (operando "extra" del
 * primer operador que limpia la pila -- hstem/vstem(hm)/hintmask/
 * cntrmask/moveto/endchar -- interpretado via nominalWidthX si esta
 * presente, o defaultWidthX si no). Pensado como FALLBACK exclusivo de
 * pdf_font.c para cuando /Widths[code] es exactamente 0 -- el PDF sigue
 * siendo la fuente de verdad para cualquier ancho no-cero (un ancho 0
 * legitimo, como una marca diacritica combinante, es indistinguible de
 * este defecto SIN esta heuristica, pero es mucho mas raro en la
 * practica que este tipo de exportacion rota). Devuelve PDF_OK (con
 * '*out_width' en unidades de 1/1000 em, mismo escalado que /Widths del
 * PDF) o un error si el glyph no tiene contorno/charstring valido. */
int pdf_cff_glyph_width(const pdf_cff_font *font, int gid, double *out_width);

#ifdef __cplusplus
}
#endif

#endif /* PDF_CFF_H */
