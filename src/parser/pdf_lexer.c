/* pdf_lexer.c
 *
 * Ver pdf_lexer.h.
 */

#include "pdf_lexer.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int is_pdf_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static int is_pdf_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '/' || c == '%';
}

static void skip_ws_and_comments(pdf_stream *st)
{
    int c;
    for (;;)
    {
        c = pdf_stream_peekc(st);
        if (c == -1)
            return;
        if (is_pdf_ws(c))
        {
            pdf_stream_getc(st);
            continue;
        }
        if (c == '%')
        {
            /* comentario hasta fin de linea */
            while (c != -1 && c != '\r' && c != '\n')
                c = pdf_stream_getc(st);
            continue;
        }
        return;
    }
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* BUG REAL ENCONTRADO Y CORREGIDO (ver DESIGN.md seccion 57): agrega
 * un byte a un token STRING que puede exceder PDF_TOK_MAX_LEN,
 * usando 'text_ovf' (buffer de reserva en el HEAP, no en el stack)
 * en vez de agrandar el buffer fijo -- ver comentario junto a
 * 'text_ovf' en pdf_lexer.h para el porque (agrandar el buffer FIJO
 * hacia 2048 bytes causo overflow de stack real en produccion, al
 * vivir 'pdf_token' como variable local de una funcion recursiva).
 * Si malloc/realloc fallan (memoria realmente agotada), los bytes de
 * mas simplemente se descartan -- se prefiere un token truncado a
 * crashear, mismo espiritu tolerante del resto del motor. */
static void tok_append_byte(pdf_token *tok, long *n, int c)
{
    if (*n < PDF_TOK_MAX_LEN - 1)
    {
        tok->text[*n] = (char)c;
        (*n)++;
        return;
    }

    if (tok->text_ovf == NULL)
    {
        long cap = PDF_TOK_MAX_LEN * 4L; /* arranca en 1024 -- cubre el caso tipico (paleta CMYK, 1024B) sin reallocar de nuevo */
        tok->text_ovf = (char *)malloc((size_t)cap);
        if (tok->text_ovf == NULL)
            return; /* sin memoria: se descartan bytes de mas, no crashear */
        memcpy(tok->text_ovf, tok->text, (size_t)*n);
        tok->text_ovf_cap = cap;
    }
    else if (*n >= tok->text_ovf_cap)
    {
        long newcap = tok->text_ovf_cap * 2L;
        char *bigger = (char *)realloc(tok->text_ovf, (size_t)newcap);
        if (bigger == NULL)
            return; /* sin memoria: se descartan bytes de mas */
        tok->text_ovf = bigger;
        tok->text_ovf_cap = newcap;
    }

    tok->text_ovf[*n] = (char)c;
    (*n)++;
}

const char *pdf_token_text(const pdf_token *tok)
{
    return (tok->text_ovf != NULL) ? tok->text_ovf : tok->text;
}

void pdf_token_free(pdf_token *tok)
{
    if (tok->text_ovf != NULL)
    {
        free(tok->text_ovf);
        tok->text_ovf = NULL;
        tok->text_ovf_cap = 0;
    }
}

static void lex_name(pdf_stream *st, pdf_token *tok)
{
    long n = 0;
    int c;

    pdf_stream_getc(st); /* consumir '/' */
    for (;;)
    {
        c = pdf_stream_peekc(st);
        if (c == -1 || is_pdf_ws(c) || is_pdf_delim(c))
            break;
        pdf_stream_getc(st);

        if (c == '#')
        {
            int h1 = pdf_stream_getc(st);
            int h2 = pdf_stream_getc(st);
            int v1 = hex_val(h1);
            int v2 = hex_val(h2);
            if (v1 >= 0 && v2 >= 0)
                c = (v1 << 4) | v2;
        }

        if (n < PDF_TOK_MAX_LEN - 1)
            tok->text[n++] = (char)c;
    }
    tok->text[n] = 0;
    tok->text_len = n;
    tok->type = PDF_TOK_NAME;
}

static void lex_literal_string(pdf_stream *st, pdf_token *tok)
{
    int depth = 1;
    long n = 0;
    int c;

    pdf_stream_getc(st); /* consumir '(' */
    while (depth > 0)
    {
        c = pdf_stream_getc(st);
        if (c == -1)
            break;

        if (c == '\\')
        {
            int e = pdf_stream_getc(st);
            switch (e)
            {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '(': c = '('; break;
            case ')': c = ')'; break;
            case '\\': c = '\\'; break;
            case '\r':
            case '\n':
                continue; /* salto de linea escapado: no emite caracter */
            default:
                if (e >= '0' && e <= '7')
                {
                    int val = e - '0';
                    int k;
                    for (k = 0; k < 2; k++)
                    {
                        int p = pdf_stream_peekc(st);
                        if (p < '0' || p > '7') break;
                        val = (val << 3) | (pdf_stream_getc(st) - '0');
                    }
                    c = val & 0xFF;
                }
                else
                {
                    c = e;
                }
            }
        }
        else if (c == '(')
        {
            depth++;
        }
        else if (c == ')')
        {
            depth--;
            if (depth == 0) break;
        }

        tok_append_byte(tok, &n, c);
    }
    /* el terminador NUL en el buffer FIJO es solo cortesia para quien
     * lea tok->text directo en casos donde se sabe que no hay overflow
     * (p.ej. lex_name) -- para STRING, el largo real es 'text_len'
     * (un string PDF puede tener bytes NUL adentro perfectamente
     * valido), asi que ni pdf_token_text() ni los consumidores reales
     * (pdf_obj_new_string, que usa memcpy con el largo explicito)
     * dependen de este terminador cuando se uso 'text_ovf'. */
    if (n < PDF_TOK_MAX_LEN)
        tok->text[n] = 0;
    tok->text_len = n;
    tok->type = PDF_TOK_STRING;
}

static void lex_number_or_keyword(pdf_stream *st, pdf_token *tok)
{
    long n = 0;
    int c;
    int is_real = 0;
    int is_number = 1;

    for (;;)
    {
        c = pdf_stream_peekc(st);
        if (c == -1 || is_pdf_ws(c) || is_pdf_delim(c))
            break;
        pdf_stream_getc(st);

        if (c == '.')
            is_real = 1;
        else if (!(isdigit(c) || c == '+' || c == '-'))
            is_number = 0;

        if (n < PDF_TOK_MAX_LEN - 1)
            tok->text[n++] = (char)c;
    }
    if (n == 0)
    {
        /* delimitador "huerfano" (p.ej. ')' sin '(' que lo abra, comun
         * en datos corruptos/binarios): sin esto el lexer no avanza y
         * queda en loop infinito -- mismo bug real encontrado en
         * pdf_content.c con quickstart.pdf, corregido aca tambien por
         * las dudas (este lexer procesa archivos, no solo streams ya
         * decodificados, asi que esta igual de expuesto). */
        int garbage = pdf_stream_getc(st);
        if (garbage >= 0 && n < PDF_TOK_MAX_LEN - 1)
            tok->text[n++] = (char)garbage;
        is_number = 0;
    }
    tok->text[n] = 0;
    tok->text_len = n;

    if (is_number && n > 0)
    {
        if (is_real)
        {
            tok->type = PDF_TOK_REAL;
            tok->dval = atof(tok->text);
        }
        else
        {
            tok->type = PDF_TOK_INT;
            tok->ival = atol(tok->text);
        }
    }
    else
    {
        tok->type = PDF_TOK_KEYWORD;
    }
}

void pdf_lex_next(pdf_stream *st, pdf_token *tok)
{
    int c;

    tok->type = PDF_TOK_EOF;
    tok->text[0] = 0;
    tok->text_len = 0;
    /* BUG REAL ENCONTRADO (ver DESIGN.md seccion 57 y comentario junto
     * a 'text_ovf' en pdf_lexer.h): esto se pisa siempre aca, sin
     * liberar un 'text_ovf' anterior -- si el llamador reusa el mismo
     * 'pdf_token' para llamadas sucesivas (comun, ver los muchos "for
     * (;;) { pdf_lex_next(st,&tok); ... }" en pdf_parser.c) sin llamar
     * pdf_token_free() entre medio, un token largo (raro) perderia su
     * buffer de reserva -- una fuga chica y acotada (unos pocos KB,
     * como maximo una vez por documento en la practica, ya que
     * strings tan largos son raros), preferible a la alternativa de
     * intentar 'free()' un puntero que podria ser basura de stack sin
     * inicializar en la primera llamada sobre un 'pdf_token' recien
     * declarado. */
    tok->text_ovf = NULL;
    tok->text_ovf_cap = 0;

    skip_ws_and_comments(st);

    c = pdf_stream_peekc(st);
    if (c == -1)
    {
        tok->type = PDF_TOK_EOF;
        return;
    }

    if (c == '/')
    {
        lex_name(st, tok);
        return;
    }
    if (c == '(')
    {
        lex_literal_string(st, tok);
        return;
    }
    if (c == '<')
    {
        pdf_stream_getc(st);
        if (pdf_stream_peekc(st) == '<')
        {
            pdf_stream_getc(st);
            tok->type = PDF_TOK_DICT_OPEN;
            return;
        }
        /* retroceder logicamente: como no hay ungetc de 2 niveles,
         * reconstruimos leyendo el hex string desde aca (ya consumimos
         * el '<' de apertura, asi que hex string custom sin re-consumir) */
        {
            long n = 0;
            int hi = -1;
            for (;;)
            {
                int cc = pdf_stream_getc(st);
                if (cc == -1 || cc == '>')
                    break;
                if (is_pdf_ws(cc))
                    continue;
                {
                    int v = hex_val(cc);
                    if (v < 0) continue;
                    if (hi < 0)
                    {
                        hi = v;
                    }
                    else
                    {
                        tok_append_byte(tok, &n, (hi << 4) | v);
                        hi = -1;
                    }
                }
            }
            if (hi >= 0)
                tok_append_byte(tok, &n, hi << 4);
            if (n < PDF_TOK_MAX_LEN)
                tok->text[n] = 0; /* ver comentario en lex_literal_string sobre por que esto es solo cortesia */
            tok->text_len = n;
            tok->type = PDF_TOK_STRING;
        }
        return;
    }
    if (c == '>')
    {
        pdf_stream_getc(st);
        if (pdf_stream_peekc(st) == '>')
        {
            pdf_stream_getc(st);
            tok->type = PDF_TOK_DICT_CLOSE;
            return;
        }
        tok->type = PDF_TOK_ERROR;
        return;
    }
    if (c == '[')
    {
        pdf_stream_getc(st);
        tok->type = PDF_TOK_ARRAY_OPEN;
        return;
    }
    if (c == ']')
    {
        pdf_stream_getc(st);
        tok->type = PDF_TOK_ARRAY_CLOSE;
        return;
    }
    if (c == '{' || c == '}')
    {
        /* PostScript calculator functions: se tratan como keyword de
         * un caracter; el parser de mas alto nivel decide si los usa. */
        pdf_stream_getc(st);
        tok->text[0] = (char)c;
        tok->text[1] = 0;
        tok->text_len = 1;
        tok->type = PDF_TOK_KEYWORD;
        return;
    }

    lex_number_or_keyword(st, tok);
}
