/* pdf_filter.h
 *
 * Filtros de decodificacion de streams PDF. Cada filtro recibe los
 * bytes crudos ya leidos (via pdf_stream_read) y escribe la salida
 * decodificada en una pdf_arena (tipicamente page->decode_arena) -- NUNCA
 * hace malloc directo. Esto es lo que hace que decodificar una imagen
 * grande quede sujeto al mismo presupuesto que todo lo demas, en vez de
 * pedirle al SO un bloque enorme sin pasar por el ledger.
 *
 * DCTDecode (JPEG) y CCITTFaxDecode quedan como puntos de enchufe
 * (pdf_filter_dct / pdf_filter_ccitt): portá ahí tu logica de HBPDF que
 * ya funciona -- el cambio mecanico es reemplazar cada malloc()/HB_XALLOC
 * por pdf_arena_alloc(decode_arena, ...). El patron es identico al de
 * pdf_filter_flate mas abajo.
 */

#ifndef PDF_FILTER_H
#define PDF_FILTER_H

#include "pdf_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdf_buf_s
{
    unsigned char *data;  /* vive en la arena pasada al filtro */
    long           len;
} pdf_buf;

/* ASCII85Decode: 'src' son los bytes crudos (incluye opcionalmente el
 * terminador "~>"). Devuelve el buffer decodificado en 'out', alocado en
 * 'arena'. Devuelve PDF_OK o error. */
int pdf_filter_ascii85(pdf_arena *arena, const unsigned char *src, long src_len,
                        pdf_buf *out);

/* FlateDecode (RFC 1950 zlib wrapper sobre RFC 1951 deflate). Implementa
 * un inflate propio (sin dependencias externas). 'expected_size' es un
 * hint opcional (0 si no se conoce) para reservar el primer bloque de
 * salida del tamanio justo y evitar realojar de mas. */
int pdf_filter_flate(pdf_arena *arena, const unsigned char *src, long src_len,
                      long expected_size, pdf_buf *out);

/* --- imagenes: JPEG baseline y CCITT Group 4 ---------------------------- */

typedef struct pdf_jpeg_image_s
{
    int            width, height;
    unsigned char *rgb;  /* width*height*3, alocado en la arena pasada al filtro */
} pdf_jpeg_image;

/* DCTDecode: decodificador JPEG propio -- baseline (SOF0/SOF1, Huffman
 * secuencial) Y progresivo (SOF2, ver DESIGN.md seccion 58; sin
 * soporte aritmetico/jerarquico). Soporta 1 componente (gray,
 * replicado a RGB), 3 componentes (YCbCr, con sub-muestreo de croma
 * 4:4:4/4:2:2/4:2:0) y 4 componentes (CMYK/YCCK, comun en fotos de
 * imprenta -- detecta el marcador Adobe APP14 y deshace tanto la
 * inversion de canales como la transformacion YCCK si corresponde,
 * ver DESIGN.md). No compone /SMask (transparencia por-pixel de la
 * imagen) -- se decodifica el color RGB correctamente pero se
 * compone opaco. */
int pdf_filter_dct(pdf_arena *arena, const unsigned char *src, long src_len,
                    pdf_jpeg_image *out);

/* CCITTFaxDecode Group 4 (K<0 -- el unico modo que soporta este
 * decodificador; K>=0, Group 3 1D/2D, no soportado). Devuelve los bits
 * empaquetados 1bpp (MSB primero, filas alineadas a byte), igual que
 * vendrian de una imagen /BitsPerComponent 1 sin filtro -- el llamador
 * (pdf_image.c) los desempaqueta con la misma logica que cualquier otra
 * imagen de 1 bpc. 'black_is_1' invierte la polaridad segun /BlackIs1. */
int pdf_filter_ccitt_g4(pdf_arena *arena, const unsigned char *src, long src_len,
                         int columns, int rows, int black_is_1, pdf_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* PDF_FILTER_H */
