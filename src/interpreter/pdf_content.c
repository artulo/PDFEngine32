/* pdf_content.c
 *
 * Ver pdf_content.h. Tokenizador propio (mas chico que pdf_lexer, que
 * esta atado a pdf_stream/archivo): un content stream ya decodificado
 * vive entero en la arena de decode como un buffer en memoria, asi que
 * ac* no hace falta lectura por bloques -- se indexa directo.
 */

#include "pdf_content.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* PDF_CONTENT_DEBUG=1: diagnostico TEMPORAL para el mismo consumo de
 * memoria sin techo de mupdf_bug.cgiid=701945-slow.rendering.pdf que
 * PDF_ARENA_DEBUG (pdf_mem.c) ya viene rastreando -- el content stream
 * de la PAGINA es de solo 470 bytes, asi que el loop tiene que estar
 * en un content stream ANIDADO (Form XObject o Pattern). Traza lx.pos
 * vs lx.len para distinguir "loop realmente trabado" (pos no avanza)
 * de "stream anidado genuinamente gigante" (pos avanza sin parar). */
static long g_pdf_content_debug_tok_count = 0;
static long g_pdf_content_debug_type_count[10];
static long g_pdf_content_debug_nested_num_count = 0;

typedef struct
{
    const unsigned char *data;
    long                  len;
    long                  pos;
} pdf_clex;

typedef enum
{
    CTOK_EOF = 0,
    CTOK_INT,
    CTOK_REAL,
    CTOK_STRING,
    CTOK_NAME,
    CTOK_ARRAY_OPEN,
    CTOK_ARRAY_CLOSE,
    CTOK_DICT_OPEN,
    CTOK_DICT_CLOSE,
    CTOK_KEYWORD
} pdf_ctok_type;

#define CTOK_MAX_LEN 256

typedef struct
{
    pdf_ctok_type type;
    long           ival;
    double         dval;
    char           text[CTOK_MAX_LEN];
    long           text_len;
} pdf_ctoken;

static int clex_peekc(pdf_clex *lx)
{
    if (lx->pos >= lx->len) return -1;
    return lx->data[lx->pos];
}

static int clex_getc(pdf_clex *lx)
{
    if (lx->pos >= lx->len) return -1;
    return lx->data[lx->pos++];
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '/' || c == '%';
}

static void skip_ws(pdf_clex *lx)
{
    int c;
    for (;;)
    {
        c = clex_peekc(lx);
        if (c == -1) return;
        if (is_ws(c)) { clex_getc(lx); continue; }
        if (c == '%')
        {
            while (c != -1 && c != '\r' && c != '\n') c = clex_getc(lx);
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

static void clex_name(pdf_clex *lx, pdf_ctoken *t)
{
    long n = 0;
    int c;
    clex_getc(lx); /* '/' */
    for (;;)
    {
        c = clex_peekc(lx);
        if (c == -1 || is_ws(c) || is_delim(c)) break;
        clex_getc(lx);
        if (c == '#')
        {
            int h1 = clex_getc(lx), h2 = clex_getc(lx);
            int v1 = hex_val(h1), v2 = hex_val(h2);
            if (v1 >= 0 && v2 >= 0) c = (v1 << 4) | v2;
        }
        if (n < CTOK_MAX_LEN - 1) t->text[n++] = (char)c;
    }
    t->text[n] = 0; t->text_len = n; t->type = CTOK_NAME;
}

static void clex_lit_string(pdf_clex *lx, pdf_ctoken *t)
{
    int depth = 1;
    long n = 0;
    int c;
    clex_getc(lx); /* '(' */
    while (depth > 0)
    {
        c = clex_getc(lx);
        if (c == -1) break;
        if (c == '\\')
        {
            int e = clex_getc(lx);
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
            case '\r': case '\n': continue;
            default:
                if (e >= '0' && e <= '7')
                {
                    int val = e - '0', k;
                    for (k = 0; k < 2; k++)
                    {
                        int p = clex_peekc(lx);
                        if (p < '0' || p > '7') break;
                        val = (val << 3) | (clex_getc(lx) - '0');
                    }
                    c = val & 0xFF;
                }
                else c = e;
            }
        }
        else if (c == '(') depth++;
        else if (c == ')') { depth--; if (depth == 0) break; }

        if (n < CTOK_MAX_LEN - 1) t->text[n++] = (char)c;
    }
    t->text[n] = 0; t->text_len = n; t->type = CTOK_STRING;
}

static void clex_hex_string(pdf_clex *lx, pdf_ctoken *t)
{
    long n = 0;
    int hi = -1, c;
    for (;;)
    {
        c = clex_getc(lx);
        if (c == -1 || c == '>') break;
        if (is_ws(c)) continue;
        {
            int v = hex_val(c);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else
            {
                if (n < CTOK_MAX_LEN - 1) t->text[n++] = (char)((hi << 4) | v);
                hi = -1;
            }
        }
    }
    if (hi >= 0 && n < CTOK_MAX_LEN - 1) t->text[n++] = (char)(hi << 4);
    t->text[n] = 0; t->text_len = n; t->type = CTOK_STRING;
}

static void clex_number_or_kw(pdf_clex *lx, pdf_ctoken *t)
{
    long n = 0;
    int c, is_real = 0, is_number = 1;
    for (;;)
    {
        c = clex_peekc(lx);
        if (c == -1 || is_ws(c) || is_delim(c)) break;
        clex_getc(lx);
        if (c == '.') is_real = 1;
        else if (!(isdigit(c) || c == '+' || c == '-')) is_number = 0;
        if (n < CTOK_MAX_LEN - 1) t->text[n++] = (char)c;
    }
    if (n == 0)
    {
        /* Un delimitador "huerfano" (p.ej. ')' sin '(' que lo abra --
         * pasa con datos binarios/corruptos, como un stream que no se
         * decodifico y quedo con bytes Flate crudos) no encaja en
         * ninguna otra rama de clex_next() y cae aca sin consumir nada
         * -- si no forzamos avanzar un byte, el lexer se queda mirando
         * el mismo caracter para siempre (bug real, encontrado con
         * quickstart.pdf: /Filter como array de un solo elemento hacia
         * que el stream se pasara SIN decodificar Flate, y el ')' en
         * el byte 75 de los datos comprimidos colgaba el programa). */
        int garbage = clex_getc(lx);
        if (garbage >= 0 && n < CTOK_MAX_LEN - 1)
            t->text[n++] = (char)garbage;
        is_number = 0;
    }
    t->text[n] = 0; t->text_len = n;

    if (is_number && n > 0)
    {
        if (is_real) { t->type = CTOK_REAL; t->dval = atof(t->text); }
        else         { t->type = CTOK_INT;  t->ival = atol(t->text); }
    }
    else t->type = CTOK_KEYWORD;
}

static void clex_next(pdf_clex *lx, pdf_ctoken *t)
{
    int c;
    t->type = CTOK_EOF; t->text[0] = 0; t->text_len = 0;

    skip_ws(lx);
    c = clex_peekc(lx);
    if (c == -1) return;

    if (c == '/') { clex_name(lx, t); return; }
    if (c == '(') { clex_lit_string(lx, t); return; }
    if (c == '<')
    {
        clex_getc(lx);
        if (clex_peekc(lx) == '<') { clex_getc(lx); t->type = CTOK_DICT_OPEN; return; }
        clex_hex_string(lx, t);
        return;
    }
    if (c == '>')
    {
        clex_getc(lx);
        if (clex_peekc(lx) == '>') { clex_getc(lx); t->type = CTOK_DICT_CLOSE; return; }
        t->type = CTOK_EOF; /* '>' suelto: sintaxis invalida, cortar */
        return;
    }
    if (c == '[') { clex_getc(lx); t->type = CTOK_ARRAY_OPEN; return; }
    if (c == ']') { clex_getc(lx); t->type = CTOK_ARRAY_CLOSE; return; }
    if (c == '{' || c == '}')
    {
        clex_getc(lx);
        t->text[0] = (char)c; t->text[1] = 0; t->text_len = 1;
        t->type = CTOK_KEYWORD;
        return;
    }
    clex_number_or_kw(lx, t);
}

/* --- parseo de valores de operando (recursivo) ------------------------ */

static pdf_obj *content_parse_value(pdf_clex *lx, pdf_ctoken *t, pdf_arena *arena)
{
    switch (t->type)
    {
    case CTOK_INT:
        if (getenv("PDF_CONTENT_DEBUG") != NULL) g_pdf_content_debug_nested_num_count++;
        return pdf_obj_new_int(arena, t->ival);
    case CTOK_REAL:
        if (getenv("PDF_CONTENT_DEBUG") != NULL) g_pdf_content_debug_nested_num_count++;
        return pdf_obj_new_real(arena, t->dval);
    case CTOK_STRING: return pdf_obj_new_string(arena, t->text, t->text_len);
    case CTOK_NAME:   return pdf_obj_new_name(arena, t->text);

    case CTOK_ARRAY_OPEN:
    {
        pdf_obj *arr = pdf_obj_new_array(arena, 4);
        pdf_ctoken inner;
        for (;;)
        {
            clex_next(lx, &inner);
            if (inner.type == CTOK_ARRAY_CLOSE || inner.type == CTOK_EOF) break;
            {
                pdf_obj *item = content_parse_value(lx, &inner, arena);
                if (item != NULL)
                {
                    if (pdf_array_push(arena, arr, item) != PDF_OK)
                        return NULL;
                }
            }
        }
        return arr;
    }

    case CTOK_DICT_OPEN:
    {
        pdf_obj *dict = pdf_obj_new_dict(arena);
        pdf_ctoken key, val;
        for (;;)
        {
            clex_next(lx, &key);
            if (key.type == CTOK_DICT_CLOSE || key.type == CTOK_EOF) break;
            if (key.type != CTOK_NAME) continue;
            {
                char keybuf[CTOK_MAX_LEN];
                pdf_obj *v;
                strcpy(keybuf, key.text);
                clex_next(lx, &val);
                v = content_parse_value(lx, &val, arena);
                if (v != NULL)
                {
                    if (pdf_dict_set(arena, dict, keybuf, v) != PDF_OK)
                        return NULL;
                }
            }
        }
        return dict;
    }

    case CTOK_KEYWORD:
        if (strcmp(t->text, "true") == 0)  return pdf_obj_new_bool(arena, 1);
        if (strcmp(t->text, "false") == 0) return pdf_obj_new_bool(arena, 0);
        if (strcmp(t->text, "null") == 0)  return pdf_obj_new_null(arena);
        return NULL; /* keyword que no es un valor: lo maneja el llamador */

    default:
        return NULL;
    }
}

/* --- imagenes inline (BI ... ID ... EI) -------------------------------- */

static void content_handle_inline_image(pdf_clex *lx, pdf_arena *arena,
                                         pdf_device *device)
{
    pdf_obj *dict = pdf_obj_new_dict(arena);
    pdf_ctoken tok;

    for (;;)
    {
        clex_next(lx, &tok);
        if (tok.type == CTOK_EOF) return; /* stream truncado */
        if (tok.type == CTOK_KEYWORD && strcmp(tok.text, "ID") == 0) break;
        if (tok.type != CTOK_NAME) continue; /* token invalido: se ignora */
        {
            char keybuf[CTOK_MAX_LEN];
            pdf_ctoken valtok;
            pdf_obj *val;
            strcpy(keybuf, tok.text);
            clex_next(lx, &valtok);
            val = content_parse_value(lx, &valtok, arena);
            if (val != NULL)
            {
                if (pdf_dict_set(arena, dict, keybuf, val) != PDF_OK)
                    return;
            }
        }
    }

    /* Segun el estandar, "ID" va seguido de EXACTAMENTE un caracter de
     * espacio blanco y ahi arrancan los datos crudos. */
    if (clex_peekc(lx) != -1 && is_ws(clex_peekc(lx)))
        clex_getc(lx);

    {
        long data_start = lx->pos;
        long i;
        long data_end = -1;

        /* Buscar "EI" delimitado por espacio antes y despues (heuristica
         * estandar: no hay forma 100% robusta sin conocer el /Length o
         * el filtro, ya que los datos crudos pueden contener "EI" por
         * casualidad -- documentado como limitacion conocida). */
        for (i = data_start; i + 1 < lx->len; i++)
        {
            if (lx->data[i] == 'E' && lx->data[i + 1] == 'I')
            {
                int before_ok = (i == data_start) || is_ws(lx->data[i - 1]);
                int after_ok  = (i + 2 >= lx->len) || is_ws(lx->data[i + 2]) || is_delim(lx->data[i+2]);
                if (before_ok && after_ok)
                {
                    data_end = i;
                    break;
                }
            }
        }

        if (data_end < 0)
        {
            lx->pos = lx->len; /* no se encontro EI: dar por terminado el stream */
            return;
        }

        {
            long dlen = data_end - data_start;
            if (dlen > 0 && is_ws(lx->data[data_end - 1]))
                dlen--; /* no incluir el whitespace previo a "EI" */
            pdf_device_emit_inline_image(device, dict, lx->data + data_start, dlen);
        }

        lx->pos = data_end + 2;
    }
}

/* --- bucle principal ---------------------------------------------------- */

int pdf_content_run_device(const unsigned char *data, long len,
                            pdf_arena *arena, pdf_device *device)
{
    pdf_clex lx;
    pdf_ctoken tok;
    pdf_obj *args[PDF_CONTENT_MAX_ARGS];
    int nargs;
    /* Pool de numeros rotativo, EN VEZ de arena: encontramos un archivo
     * real (tests/mupdf_bug.cgiid=701945-slow.rendering.pdf, un mapa
     * aeronautico denso) cuyo content stream decodificado pesa ~17 MB
     * y esta compuesto casi enteramente de operandos numericos para
     * dibujar millones de segmentos de linea -- cada numero pedia un
     * pdf_obj NUEVO en la arena (nunca liberado hasta cerrar la pagina
     * entera), agotando cualquier presupuesto de memoria antes de
     * terminar de dibujar el mapa (el resto del contenido se salteaba
     * en silencio, mismo criterio tolerante de siempre -- por eso se
     * veia "incompleto" en vez de fallar con un error). Los operandos
     * numericos se leen SIEMPRE de forma inmediata dentro del mismo
     * despacho de operador (pdf_device_emit -> pdf_render_op, que solo
     * convierte a double via pdf_obj_num) y ninguna parte del motor
     * guarda el puntero mas alla de eso (confirmado por revision de
     * pdf_render.c) -- asi que un pool de PDF_CONTENT_MAX_ARGS slots,
     * del tamanio maximo de argumentos que un operador puede acumular
     * antes de despacharse, es seguro para reciclar en vez de pedir
     * memoria nueva por cada numero. Strings/nombres/arrays/dicts (muy
     * poco frecuentes en comparacion, y con mas riesgo de que algo
     * retenga el puntero -- p.ej. resources) siguen yendo por la arena
     * como siempre. */
    pdf_obj num_pool[PDF_CONTENT_MAX_ARGS];
    int num_pool_next;

    if (data == NULL || arena == NULL || device == NULL ||
        device->ops == NULL || device->ops->op == NULL)
        return PDF_ERR_BADARG;

    lx.data = data; lx.len = len; lx.pos = 0;
    nargs = 0;
    num_pool_next = 0;

    for (;;)
    {
        clex_next(&lx, &tok);
        if (tok.type == CTOK_EOF)
        {
            if (getenv("PDF_CONTENT_DEBUG") != NULL)
                fprintf(stderr, "PDF_CONTENT_DEBUG: EOF len=%ld -- tipos: EOF=%ld INT=%ld REAL=%ld STRING=%ld NAME=%ld ARR_OPEN=%ld ARR_CLOSE=%ld DICT_OPEN=%ld DICT_CLOSE=%ld KEYWORD=%ld nested_nums(arena, NO pool)=%ld\n",
                        len, g_pdf_content_debug_type_count[0], g_pdf_content_debug_type_count[1],
                        g_pdf_content_debug_type_count[2], g_pdf_content_debug_type_count[3],
                        g_pdf_content_debug_type_count[4], g_pdf_content_debug_type_count[5],
                        g_pdf_content_debug_type_count[6], g_pdf_content_debug_type_count[7],
                        g_pdf_content_debug_type_count[8], g_pdf_content_debug_type_count[9],
                        g_pdf_content_debug_nested_num_count);
            break;
        }

        if (getenv("PDF_CONTENT_DEBUG") != NULL)
        {
            g_pdf_content_debug_tok_count++;
            if (tok.type >= 0 && tok.type < 10)
                g_pdf_content_debug_type_count[tok.type]++;
            if (g_pdf_content_debug_tok_count <= 20 ||
                (g_pdf_content_debug_tok_count % 200000) == 0)
                fprintf(stderr, "PDF_CONTENT_DEBUG: tok #%ld type=%d text='%s' pos=%ld/%ld\n",
                        g_pdf_content_debug_tok_count, (int)tok.type, tok.text, lx.pos, lx.len);
        }

        if (tok.type == CTOK_KEYWORD)
        {
            if (strcmp(tok.text, "true") == 0 || strcmp(tok.text, "false") == 0 ||
                strcmp(tok.text, "null") == 0)
            {
                pdf_obj *v = content_parse_value(&lx, &tok, arena);
                if (v != NULL && nargs < PDF_CONTENT_MAX_ARGS)
                    args[nargs++] = v;
                continue;
            }

            if (strcmp(tok.text, "BI") == 0)
            {
                content_handle_inline_image(&lx, arena, device);
                nargs = 0;
                continue;
            }

            /* cualquier otro keyword es un operador */
            pdf_device_emit(device, tok.text, args, nargs);
            nargs = 0;
            continue;
        }

        /* no es keyword: es un operando (numero, string, name, array, dict) */
        if (tok.type == CTOK_INT || tok.type == CTOK_REAL)
        {
            /* numero: reciclar del pool en vez de pedir arena (ver
             * comentario grande al inicio de la funcion). Si nargs ya
             * llego al maximo (stream corrupto/adversario, mismo caso
             * que el descarte silencioso de mas abajo) NO se toca el
             * pool -- avanzarlo igual corromperia un slot que un
             * args[i] anterior todavia no despachado sigue apuntando. */
            if (nargs < PDF_CONTENT_MAX_ARGS)
            {
                pdf_obj *v = &num_pool[num_pool_next];
                pdf_obj_type t = (tok.type == CTOK_INT) ? PDF_INT : PDF_REAL;
                num_pool_next = (num_pool_next + 1) % PDF_CONTENT_MAX_ARGS;
                memcpy(&v->type, &t, sizeof(t));
                if (tok.type == CTOK_INT)
                    memcpy(&v->u.integer, &tok.ival, sizeof(tok.ival));
                else
                    memcpy(&v->u.real, &tok.dval, sizeof(tok.dval));
                args[nargs++] = v;
            }
        }
        else
        {
            pdf_obj *v = content_parse_value(&lx, &tok, arena);
            if (v != NULL && nargs < PDF_CONTENT_MAX_ARGS)
                args[nargs++] = v;
            /* si nargs ya llego al maximo, el operando se descarta
             * silenciosamente -- no debería pasar con operadores PDF
             * validos, pero evita desbordar el array ante streams
             * corruptos o adversarios. */
        }
    }

    return PDF_OK;
}

/* Compatibilidad con la API anterior. El interprete real vive en
 * pdf_content_run_device(); esto evita mantener dos caminos de parseo. */
int pdf_content_run(const unsigned char *data, long len,
                    pdf_arena *arena, const pdf_content_ops *ops)
{
    pdf_device_ops dops;
    pdf_device device;

    if (data == NULL || arena == NULL || ops == NULL || ops->op == NULL)
        return PDF_ERR_BADARG;

    dops.op = ops->op;
    dops.inline_image = ops->inline_image;
    pdf_device_init(&device, &dops, ops->user);

    return pdf_content_run_device(data, len, arena, &device);
}
