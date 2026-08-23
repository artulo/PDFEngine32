/* pdf_aes.h
 *
 * AES-128 y AES-256 (FIPS-197), modo CBC, escrito desde cero -- solo
 * los dos tamanios de clave que usa el estandar PDF (AESV2 = 128 bits,
 * AESV3 = 256 bits; 192 bits no se implementa porque PDF nunca lo usa).
 *
 * Verificado por fuera contra 'openssl enc' con los vectores de prueba
 * estandar de FIPS-197 antes de integrarse a pdf_crypt.c.
 */

#ifndef PDF_AES_H
#define PDF_AES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Descifra 'data' (longitud 'len', multiplo de 16) IN PLACE con AES en
 * modo CBC. 'key_bits' debe ser 128 o 256. 'iv' son 16 bytes (no se
 * modifica). No saca padding -- eso lo maneja el llamador (pdf_crypt.c),
 * que conoce la convencion PKCS#7 de PDF. */
void pdf_aes_cbc_decrypt(const unsigned char *key, int key_bits,
                          const unsigned char *iv,
                          unsigned char *data, long len);

/* Cifra 'data' (longitud 'len', multiplo de 16) IN PLACE con AES-128 en
 * modo CBC, sin padding -- usado UNICAMENTE por el hash endurecido R6
 * (V5/AESV3) de pdf_crypt.c, nunca para desencriptar streams. */
void pdf_aes128_cbc_encrypt_nopad(const unsigned char *key,
                                   const unsigned char *iv,
                                   unsigned char *data, long len);

#ifdef __cplusplus
}
#endif

#endif /* PDF_AES_H */
