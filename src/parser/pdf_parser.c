/* pdf_parser.c
 *
 * Ver pdf_parser.h.
 */

#include "pdf_parser.h"
#include <string.h>
#include <stdio.h>

/* --- parseo de objetos (recursivo) ------------------------------------ */

static int tok_is_keyword(const pdf_token *tok, const char *kw)
{
    return tok->type == PDF_TOK_KEYWORD && strcmp(tok->text, kw) == 0;
}

/* Intenta reconocer "N G R" a partir de un INT ya leido en tok1. Si no
 * matchea, deja el stream posicionado justo despues de tok1 (como si el
 * lookahead nunca hubiese pasado) y devuelve NULL. Si matchea, consume
 * todo y devuelve el pdf_obj PDF_REF ya armado.
 *
 * BUG REAL ENCONTRADO Y ARREGLADO: con ciertos PDFs reales (confirmado
 * con tests/Los_Kajchas_y_los_proyectos_de_industria.pdf -- texto
 * "ilegible", patron de barras en vez de letras), una referencia
 * indirecta como "/Font 121 0 R" a veces se resolvia contra el numero
 * de objeto EQUIVOCADO (0 en vez de 121). Es una miscompilacion real de
 * bcc32 7.70, no logica nuestra ni UB (ver el comentario grande en
 * pdf_obj_new_ref, pdf_object.c, para el detalle completo de como se
 * aislo y confirmo). Esta funcion se reescribio para devolver el
 * pdf_obj directo (en vez de num/gen por punteros de salida) como parte
 * del fix -- junto con memcpy() en pdf_obj_new_ref y compilar AMBOS
 * archivos sin -O2 (ver win32/Build.bat), confirmado 8/8 contra un
 * reproductor minimo Y contra el PDF real completo. */
static pdf_obj *try_lex_ref(pdf_stream *st, const pdf_token *tok1, pdf_arena *arena)
{
    long pos_after_tok1;
    pdf_token tok2, tok3;

    pos_after_tok1 = pdf_stream_tell(st);

    pdf_lex_next(st, &tok2);
    if (tok2.type != PDF_TOK_INT)
    {
        pdf_stream_seek(st, pos_after_tok1);
        return NULL;
    }

    pdf_lex_next(st, &tok3);
    if (tok_is_keyword(&tok3, "R"))
        return pdf_obj_new_ref(arena, tok1->ival, tok2.ival);

    pdf_stream_seek(st, pos_after_tok1);
    return NULL;
}

pdf_obj *pdf_parse_object(pdf_stream *st, pdf_token *tok, pdf_arena *arena)
{
    pdf_token local;

    if (tok->type == PDF_TOK_EOF)
        pdf_lex_next(st, tok);

    switch (tok->type)
    {
    case PDF_TOK_INT:
    {
        pdf_obj *ref = try_lex_ref(st, tok, arena);
        if (ref != NULL)
            return ref;
        return pdf_obj_new_int(arena, tok->ival);
    }
    case PDF_TOK_REAL:
        return pdf_obj_new_real(arena, tok->dval);

    case PDF_TOK_STRING:
    {
        /* pdf_token_text()/pdf_token_free(): ver DESIGN.md seccion 57
         * -- 'tok' puede tener un buffer de reserva en el heap si el
         * string era mas largo que PDF_TOK_MAX_LEN (raro: paletas
         * /Indexed inline). pdf_obj_new_string ya copia el contenido
         * a la arena, asi que el buffer de reserva se libera aca
         * mismo, apenas se termina de usar. */
        pdf_obj *s = pdf_obj_new_string(arena, pdf_token_text(tok), tok->text_len);
        pdf_token_free(tok);
        return s;
    }

    case PDF_TOK_NAME:
        return pdf_obj_new_name(arena, tok->text);

    case PDF_TOK_ARRAY_OPEN:
    {
        pdf_obj *arr = pdf_obj_new_array(arena, 4);
        for (;;)
        {
            pdf_lex_next(st, &local);
            if (local.type == PDF_TOK_ARRAY_CLOSE || local.type == PDF_TOK_EOF)
                break;
            {
                pdf_obj *item = pdf_parse_object(st, &local, arena);
                if (item != NULL)
                {
                    if (pdf_array_push(arena, arr, item) != PDF_OK)
                        return NULL;
                }
            }
        }
        return arr;
    }

    case PDF_TOK_DICT_OPEN:
    {
        pdf_obj *dict = pdf_obj_new_dict(arena);
        for (;;)
        {
            pdf_lex_next(st, &local);
            if (local.type == PDF_TOK_DICT_CLOSE || local.type == PDF_TOK_EOF)
                break;
            if (local.type != PDF_TOK_NAME)
                continue; /* clave invalida: se ignora y se sigue (tolerante) */
            {
                char key[PDF_TOK_MAX_LEN];
                pdf_token valtok;
                pdf_obj *val;

                strcpy(key, local.text);
                pdf_lex_next(st, &valtok);
                val = pdf_parse_object(st, &valtok, arena);
                if (val != NULL)
                {
                    if (pdf_dict_set(arena, dict, key, val) != PDF_OK)
                        return NULL;
                }
            }
        }
        return dict;
    }

    case PDF_TOK_KEYWORD:
        if (strcmp(tok->text, "true") == 0)  return pdf_obj_new_bool(arena, 1);
        if (strcmp(tok->text, "false") == 0) return pdf_obj_new_bool(arena, 0);
        if (strcmp(tok->text, "null") == 0)  return pdf_obj_new_null(arena);
        return pdf_obj_new_null(arena); /* keyword inesperado: se trata como null (tolerante) */

    default:
        return NULL;
    }
}

pdf_obj *pdf_parse_indirect_object(pdf_stream *st, long offset,
                                    long *out_num, long *out_gen,
                                    pdf_arena *arena,
                                    const pdf_xref_table *xref_for_length)
{
    pdf_token t_num, t_gen, t_obj, t_body;
    pdf_obj *body;

    if (pdf_stream_seek(st, offset) != PDF_OK)
        return NULL;

    pdf_lex_next(st, &t_num);
    pdf_lex_next(st, &t_gen);
    pdf_lex_next(st, &t_obj);

    if (t_num.type != PDF_TOK_INT || t_gen.type != PDF_TOK_INT || !tok_is_keyword(&t_obj, "obj"))
        return NULL; /* no habia un objeto valido en ese offset */

    if (out_num != NULL) *out_num = t_num.ival;
    if (out_gen != NULL) *out_gen = t_gen.ival;

    pdf_lex_next(st, &t_body);
    body = pdf_parse_object(st, &t_body, arena);

    /* Verificar si sigue "stream" (solo valido si body es un dict) */
    if (body != NULL && body->type == PDF_DICT)
    {
        long pos_before;
        pdf_token t_stream;

        pos_before = pdf_stream_tell(st);
        pdf_lex_next(st, &t_stream);

        if (tok_is_keyword(&t_stream, "stream"))
        {
            long raw_offset, raw_length;
            int c;
            pdf_obj *length_obj;

            /* Segun el estandar: "stream" seguido de CRLF o LF (nunca CR
             * solo) y recien ahi empiezan los datos. */
            c = pdf_stream_peekc(st);
            if (c == '\r')
            {
                pdf_stream_getc(st);
                c = pdf_stream_peekc(st);
                if (c == '\n') pdf_stream_getc(st);
            }
            else if (c == '\n')
            {
                pdf_stream_getc(st);
            }

            /* raw_offset se captura ANTES de resolver /Length porque
             * resolver una referencia indirecta puede saltar a otra
             * parte del archivo (ver mas abajo) -- una vez guardado en
             * esta variable, no importa donde quede posicionado 'st'. */
            raw_offset = pdf_stream_tell(st);

            raw_length = -1;
            length_obj = pdf_dict_get(body, "Length");
            if (length_obj != NULL && length_obj->type == PDF_INT)
            {
                raw_length = length_obj->u.integer;
            }
            else if (length_obj != NULL && length_obj->type == PDF_REAL)
            {
                raw_length = (long)length_obj->u.real;
            }
            else if (length_obj != NULL && length_obj->type == PDF_REF && xref_for_length != NULL)
            {
                /* /Length N G R : patron comun en PDFs generados en
                 * streaming (el productor no conoce el tamanio del
                 * stream hasta despues de escribirlo). Se resuelve
                 * cargando ese objeto (que siempre es un entero simple,
                 * nunca otro stream) -- se pasa xref_for_length=NULL en
                 * la recursion porque un /Length jamas necesita resolver
                 * su propio /Length. */
                long len_offset = pdf_xref_offset(xref_for_length, length_obj->u.ref.num);
                if (len_offset >= 0)
                {
                    long got_num, got_gen;
                    pdf_obj *len_result = pdf_parse_indirect_object(
                        st, len_offset, &got_num, &got_gen, arena, NULL);

                    if (len_result != NULL && len_result->type == PDF_INT)
                        raw_length = len_result->u.integer;
                    else if (len_result != NULL && len_result->type == PDF_REAL)
                        raw_length = (long)len_result->u.real;
                }
            }

            return pdf_obj_new_stream(arena, body, raw_offset, raw_length, t_num.ival, t_gen.ival);
        }
        else
        {
            pdf_stream_seek(st, pos_before);
        }
    }

    return body;
}



/* Compatibilidad con la API historica. La resolucion real vive en pdf_xref.c. */
pdf_obj *pdf_parser_load_object(pdf_stream *st, const pdf_xref_table *xref,
                                 long num, pdf_arena *arena)
{
    return pdf_xref_load_object(st, xref, num, arena);
}
