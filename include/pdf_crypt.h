/* pdf_crypt.h
 *
 * Desencriptado del "Standard Security Handler" de PDF:
 *  - V1/V2 (R2/R3): RC4 clasico, clave de archivo via MD5 (Algoritmo 3.2).
 *  - V4 (R4) con /CF /CFM /AESV2: AES-128-CBC, misma clave de archivo
 *    MD5 que arriba mas el sufijo "sAlT" en la derivacion por objeto
 *    (Algoritmo 1, norma 7.6.2 paso e). V4 con /CFM /V2 sigue siendo
 *    RC4 (caso raro pero valido).
 *  - V5 (R5/R6) con /CFM /AESV3: AES-256-CBC, clave de archivo de 256
 *    bits derivada de /U+/UE (Algoritmo 2.A; R6 usa el hash "endurecido"
 *    con SHA-256/384/512 -- ver pdf_crypt.c). Sin derivacion por objeto:
 *    la misma clave de archivo se usa para todos los objetos.
 *
 * En todos los casos se asume contrasenia de USUARIO vacia (el unico
 * caso automatizable sin pedirle nada a nadie -- un lector no puede
 * pedir la contrasenia en medio de un render headless, y un PDF con
 * contrasenia de usuario real simplemente no se puede abrir sin ella).
 * Si la contrasenia no resulta ser vacia, o el esquema no es ninguno
 * de los de arriba, 'crypt->active' queda en 0 y los streams NO se
 * desencriptan -- van a fallar a decodificar mas arriba, pero de forma
 * clara (el filtro correspondiente rechaza basura), no silenciosa.
 *
 * NOTA DE ALCANCE: solo se desencriptan STREAMS (los 8 sitios que
 * llaman pdf_crypt_decrypt en todo el motor). Ningun /PDF_STRING suelto
 * (fuera de un stream) se desencripta hoy -- no hay ningun consumidor
 * de un string encriptado en el codigo actual.
 */

#ifndef PDF_CRYPT_H
#define PDF_CRYPT_H

#include "pdf_object.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pdf_crypt_method_e
{
    PDF_CRYPT_RC4 = 0,
    PDF_CRYPT_AES128,
    PDF_CRYPT_AES256
} pdf_crypt_method;

typedef struct pdf_crypt_s
{
    int  active;         /* 1 si el documento esta encriptado con un
                           * esquema que este modulo sabe manejar; 0 en
                           * cualquier otro caso (sin encriptar, esquema
                           * no soportado, o contrasenia no vacia). */
    unsigned char key[32]; /* clave de archivo: 5..16 bytes (RC4/AESV2)
                             * o 32 bytes (AESV3/256 bits). */
    int  key_len;         /* bytes de 'key' realmente usados */
    int  v, r;
    pdf_crypt_method method; /* metodo usado para STREAMS (StmF) */
} pdf_crypt;

/* Inicializa 'crypt' a partir del diccionario /Encrypt (ya resuelto) y
 * el primer elemento de /ID del trailer (bytes crudos del string, sin
 * desencriptar -- /ID nunca esta encriptado; no se usa para V5). Asume
 * contrasenia de usuario vacia. Si el esquema no es ninguno de los
 * soportados (ver arriba), 'crypt->active' queda en 0. */
void pdf_crypt_init(pdf_crypt *crypt, pdf_obj *encrypt_dict,
                     const unsigned char *id0, long id0_len);

/* Desencripta 'data' (longitud 'len') IN PLACE, usando la clave
 * derivada para el objeto (obj_num, obj_gen) -- para RC4/AESV2; AESV3
 * usa la clave de archivo directa, sin derivar por objeto. Devuelve la
 * longitud NUEVA de 'data': igual a 'len' para RC4 (o si !active), mas
 * corta para AES (se descarta el IV de 16 bytes al principio y el
 * padding PKCS#7 al final). No hace nada (devuelve 'len' sin tocar
 * 'data') si !crypt->active. */
long pdf_crypt_decrypt(const pdf_crypt *crypt, long obj_num, long obj_gen,
                        unsigned char *data, long len);

#ifdef __cplusplus
}
#endif

#endif /* PDF_CRYPT_H */
