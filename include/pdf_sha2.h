/* pdf_sha2.h
 *
 * SHA-256, SHA-384 y SHA-512 (FIPS 180-4), escrito desde cero, C89 --
 * solo lo que necesita la derivacion de clave V5/AESV3 de pdf_crypt.c
 * (Algoritmo 2.A y el hash "endurecido" de R6, que usa los tres).
 *
 * Verificado por fuera contra 'python -c "import hashlib; ..."' antes
 * de integrarse a pdf_crypt.c.
 */

#ifndef PDF_SHA2_H
#define PDF_SHA2_H

#ifdef __cplusplus
extern "C" {
#endif

/* Cada una calcula el digest completo de una sola pasada (sin API
 * incremental -- los datos de entrada en el uso real, contrasenia +
 * salt + buffers intermedios de a lo sumo unos pocos KB, siempre caben
 * enteros en memoria de una). */
void pdf_sha256(const unsigned char *data, long len, unsigned char digest[32]);
void pdf_sha384(const unsigned char *data, long len, unsigned char digest[48]);
void pdf_sha512(const unsigned char *data, long len, unsigned char digest[64]);

#ifdef __cplusplus
}
#endif

#endif /* PDF_SHA2_H */
