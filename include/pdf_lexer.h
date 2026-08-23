/* pdf_lexer.h
 *
 * Tokenizador minimo de sintaxis PDF (COS), sobre pdf_stream (lectura
 * por bloques, no todo el archivo en memoria).
 */

#ifndef PDF_LEXER_H
#define PDF_LEXER_H

#include "pdf_stream.h"
#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_tok_type_e
{
    PDF_TOK_EOF = 0,
    PDF_TOK_INT,
    PDF_TOK_REAL,
    PDF_TOK_STRING,       /* (...) o <...> ya des-escapado */
    PDF_TOK_NAME,         /* /Algo, ya des-escapado (#xx)  */
    PDF_TOK_ARRAY_OPEN,   /* [ */
    PDF_TOK_ARRAY_CLOSE,  /* ] */
    PDF_TOK_DICT_OPEN,    /* << */
    PDF_TOK_DICT_CLOSE,   /* >> */
    PDF_TOK_KEYWORD,      /* obj, endobj, stream, endstream, R, true, false,
                              null, xref, trailer, startxref, N (para n/f de xref) */
    PDF_TOK_ERROR
} pdf_tok_type;

/* BUG REAL ENCONTRADO Y CORREGIDO (ver DESIGN.md secciones 54 y 57):
 * 256 alcanzaba para claves/nombres tipicos, pero NO para un caso
 * legitimo y nada raro: una paleta de color inline como PDF_STRING
 * literal (no stream) en un color space /Indexed -- hasta 256
 * entradas * hasta 4 componentes (base CMYK) = 1024 bytes, muy por
 * encima de 256. El primer intento de arreglar esto (seccion 54)
 * agrando PDF_TOK_MAX_LEN a 2048 -- funcionaba, pero introdujo una
 * REGRESION grave (seccion 57): 'pdf_token' se declara como variable
 * LOCAL (en el stack) en 'pdf_parse_object', una funcion RECURSIVA
 * (dicts/arrays anidados se parsean llamandose a si misma). Agrandar
 * el buffer fijo de 256 a 2048 bytes multiplico por ~8 el consumo de
 * stack por cada nivel de recursion -- con un PDF real, complejo y
 * con anidamiento profundo, esto agoto el stack real de Windows
 * (bcc32/Borland, confirmado con el .map de un crash real: la pila
 * mostraba 'pdf_parse_object' llamandose a si misma varias veces
 * antes de un ACCESS_VIOLATION dentro de memmove -- una firma
 * clasica de overflow de stack, no relacionada logicamente con
 * memmove en si). PDF_TOK_MAX_LEN vuelve a 256 (el tamanio que
 * SIEMPRE alcanza para nombres/claves/numeros, que nunca son largos
 * en la practica) -- el caso de strings largos (la paleta) se maneja
 * aparte, con un buffer de reserva alocado dinamicamente (ver
 * 'text_ovf' mas abajo) que NO vive en el stack ni crece con la
 * recursion. */
#define PDF_TOK_MAX_LEN 256

typedef struct pdf_token_s
{
    pdf_tok_type type;
    long         ival;
    double       dval;
    char         text[PDF_TOK_MAX_LEN]; /* string/name/keyword corto, terminado en 0 */
    long         text_len;
    /* Buffer de reserva para PDF_TOK_STRING mas largos que
     * PDF_TOK_MAX_LEN-1 (caso raro: paletas /Indexed inline, alguna
     * cadena binaria grande). NULL si no hizo falta -- el caso comun
     * (99.9% de los tokens reales) nunca toca esto. Alocado con
     * malloc() dentro del lexer cuando se necesita; el llamador que
     * termina de usar el token DEBE liberarlo con pdf_token_free()
     * cuando ya copio el contenido a donde corresponda (la arena, via
     * pdf_obj_new_string por ejemplo) -- ver pdf_token_text(). */
    char        *text_ovf;
    long         text_ovf_cap; /* capacidad alocada de 'text_ovf' (bookkeeping interno) */
} pdf_token;

/* Devuelve el puntero al contenido real del token (el buffer fijo, o
 * el de reserva si se uso). Los llamadores que leen tok->text
 * directo para un PDF_TOK_STRING deben usar esto en su lugar, para
 * no truncar silenciosamente strings largos. */
const char *pdf_token_text(const pdf_token *tok);

/* Libera 'text_ovf' si esta alocado, y lo deja en NULL. Debe llamarse
 * despues de haber copiado el contenido de un token STRING a destino
 * final (arena, etc.) -- no hace falta para tokens que nunca usaron
 * el buffer de reserva (no-op en ese caso). */
void pdf_token_free(pdf_token *tok);

/* Lee el siguiente token, saltando espacios y comentarios (%...). Las
 * cadenas de PDF_TOK_STRING mas largas que PDF_TOK_MAX_LEN-1 usan
 * 'text_ovf' (ver comentario arriba) en vez de truncarse -- el
 * llamador debe usar pdf_token_text(tok) para leer el contenido
 * completo, y pdf_token_free(tok) cuando termine de usarlo. Los
 * datos de un stream NUNCA pasan por aca, se leen aparte con
 * pdf_stream_read directo a la arena de decode. */
void pdf_lex_next(pdf_stream *st, pdf_token *tok);

#ifdef __cplusplus
}
#endif

#endif /* PDF_LEXER_H */
