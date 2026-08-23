/* pdf_stream.h
 *
 * Lector de archivo con buffer chico y ventana deslizante. A diferencia
 * de cargar el PDF entero a un buffer en memoria (tentador y lo que hace
 * mucho codigo "simple"), esto lee en bloques de PDF_STREAM_BUFSZ bytes
 * y permite fseek para saltar a los offsets del xref sin traer el resto
 * del archivo a RAM. Para un PDF de 500 MB con miles de imagenes, esto
 * es la diferencia entre picos de unos pocos KB y picos de cientos de MB.
 */

#ifndef PDF_STREAM_H
#define PDF_STREAM_H

#include <stdio.h>
#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_STREAM_BUFSZ 4096

typedef struct pdf_stream_s
{
    FILE          *fp;           /* NULL si es un stream de memoria (ver pdf_stream_open_memory) */
    long           file_size;
    long           buf_start;   /* offset en el archivo del byte buf[0] */
    int            buf_len;     /* bytes validos en buf                 */
    int            buf_pos;     /* posicion de lectura dentro de buf    */
    unsigned char  buf[PDF_STREAM_BUFSZ];

    /* modo memoria: para parsear objetos ya decodificados en RAM (p.ej.
     * un ObjStm ya descomprimido) reusando el mismo lexer/parser que el
     * resto del motor, sin necesitar un archivo real. */
    const unsigned char *mem_data;
    long                  mem_len;
    long                  mem_pos;
} pdf_stream;

/* Abre 'path' para lectura binaria. Devuelve PDF_OK o PDF_ERR_BADARG si
 * no se pudo abrir. No lee nada todavia (lectura perezosa por bloques). */
int  pdf_stream_open(pdf_stream *st, const char *path);

/* Envuelve un buffer en memoria (p.ej. un ObjStm ya descomprimido) para
 * poder usar pdf_lex_next/pdf_parse_object sobre el sin necesitar un
 * archivo. 'data' debe seguir viva mientras se use 'st' (tipicamente
 * vive en una pdf_arena que el llamador ya controla). */
void pdf_stream_open_memory(pdf_stream *st, const unsigned char *data, long len);

void pdf_stream_close(pdf_stream *st);

/* Posicion logica actual (equivalente a ftell). */
long pdf_stream_tell(const pdf_stream *st);

/* Salta a 'offset' absoluto. Es barato: si el offset cae dentro del
 * buffer actual, no toca el disco. */
int  pdf_stream_seek(pdf_stream *st, long offset);

/* Devuelve el siguiente byte y avanza, o -1 en EOF. */
int  pdf_stream_getc(pdf_stream *st);

/* Mira el siguiente byte sin avanzar, o -1 en EOF. */
int  pdf_stream_peekc(pdf_stream *st);

/* Lee 'n' bytes crudos hacia 'dst' (dst debe tener espacio). Devuelve
 * la cantidad efectivamente leida (puede ser menor que n en EOF). Usado
 * para copiar el contenido crudo de un stream (p.ej. antes de pasarlo
 * por un filtro) directo a una arena, sin buffer intermedio grande. */
long pdf_stream_read(pdf_stream *st, unsigned char *dst, long n);

long pdf_stream_size(const pdf_stream *st);

#ifdef __cplusplus
}
#endif

#endif /* PDF_STREAM_H */
