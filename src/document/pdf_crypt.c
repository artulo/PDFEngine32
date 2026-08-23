/* pdf_crypt.c
 *
 * Ver pdf_crypt.h. MD5, RC4, AES (pdf_aes.c) y SHA-2 (pdf_sha2.c)
 * escritos desde cero (algoritmos estandar, no tablas grandes propensas
 * a error de transcripcion como CCITT -- se validaron por separado
 * contra 'openssl'/Python antes de integrarlos, ver DESIGN.md).
 */

#include "pdf_crypt.h"
#include "pdf_aes.h"
#include "pdf_sha2.h"
#include <string.h>

typedef unsigned int pdf_u32;

/* ====================================================================
 * MD5 (RFC 1321)
 * ==================================================================== */

typedef struct
{
    pdf_u32       state[4];
    pdf_u32       count[2]; /* bits procesados, 64 bits como 2 words de 32 */
    unsigned char buffer[64];
} pdf_md5_ctx;

static pdf_u32 md5_rotl(pdf_u32 x, int n) { return (x << n) | (x >> (32 - n)); }

#define MD5_F(x,y,z) (((x) & (y)) | ((~(x)) & (z)))
#define MD5_G(x,y,z) (((x) & (z)) | ((y) & (~(z))))
#define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
#define MD5_I(x,y,z) ((y) ^ ((x) | (~(z))))

static void md5_transform(pdf_u32 state[4], const unsigned char block[64])
{
    pdf_u32 a = state[0], b = state[1], c = state[2], d = state[3];
    pdf_u32 x[16];
    int i;

    for (i = 0; i < 16; i++)
        x[i] = (pdf_u32)block[i*4]         | ((pdf_u32)block[i*4+1] << 8) |
               ((pdf_u32)block[i*4+2] << 16) | ((pdf_u32)block[i*4+3] << 24);

    #define FF(a,b,c,d,x,s,ac) { (a)+=MD5_F((b),(c),(d))+(x)+(pdf_u32)(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
    #define GG(a,b,c,d,x,s,ac) { (a)+=MD5_G((b),(c),(d))+(x)+(pdf_u32)(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
    #define HH(a,b,c,d,x,s,ac) { (a)+=MD5_H((b),(c),(d))+(x)+(pdf_u32)(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
    #define II(a,b,c,d,x,s,ac) { (a)+=MD5_I((b),(c),(d))+(x)+(pdf_u32)(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }

    FF(a,b,c,d,x[ 0], 7,0xd76aa478) FF(d,a,b,c,x[ 1],12,0xe8c7b756)
    FF(c,d,a,b,x[ 2],17,0x242070db) FF(b,c,d,a,x[ 3],22,0xc1bdceee)
    FF(a,b,c,d,x[ 4], 7,0xf57c0faf) FF(d,a,b,c,x[ 5],12,0x4787c62a)
    FF(c,d,a,b,x[ 6],17,0xa8304613) FF(b,c,d,a,x[ 7],22,0xfd469501)
    FF(a,b,c,d,x[ 8], 7,0x698098d8) FF(d,a,b,c,x[ 9],12,0x8b44f7af)
    FF(c,d,a,b,x[10],17,0xffff5bb1) FF(b,c,d,a,x[11],22,0x895cd7be)
    FF(a,b,c,d,x[12], 7,0x6b901122) FF(d,a,b,c,x[13],12,0xfd987193)
    FF(c,d,a,b,x[14],17,0xa679438e) FF(b,c,d,a,x[15],22,0x49b40821)

    GG(a,b,c,d,x[ 1], 5,0xf61e2562) GG(d,a,b,c,x[ 6], 9,0xc040b340)
    GG(c,d,a,b,x[11],14,0x265e5a51) GG(b,c,d,a,x[ 0],20,0xe9b6c7aa)
    GG(a,b,c,d,x[ 5], 5,0xd62f105d) GG(d,a,b,c,x[10], 9,0x02441453)
    GG(c,d,a,b,x[15],14,0xd8a1e681) GG(b,c,d,a,x[ 4],20,0xe7d3fbc8)
    GG(a,b,c,d,x[ 9], 5,0x21e1cde6) GG(d,a,b,c,x[14], 9,0xc33707d6)
    GG(c,d,a,b,x[ 3],14,0xf4d50d87) GG(b,c,d,a,x[ 8],20,0x455a14ed)
    GG(a,b,c,d,x[13], 5,0xa9e3e905) GG(d,a,b,c,x[ 2], 9,0xfcefa3f8)
    GG(c,d,a,b,x[ 7],14,0x676f02d9) GG(b,c,d,a,x[12],20,0x8d2a4c8a)

    HH(a,b,c,d,x[ 5], 4,0xfffa3942) HH(d,a,b,c,x[ 8],11,0x8771f681)
    HH(c,d,a,b,x[11],16,0x6d9d6122) HH(b,c,d,a,x[14],23,0xfde5380c)
    HH(a,b,c,d,x[ 1], 4,0xa4beea44) HH(d,a,b,c,x[ 4],11,0x4bdecfa9)
    HH(c,d,a,b,x[ 7],16,0xf6bb4b60) HH(b,c,d,a,x[10],23,0xbebfbc70)
    HH(a,b,c,d,x[13], 4,0x289b7ec6) HH(d,a,b,c,x[ 0],11,0xeaa127fa)
    HH(c,d,a,b,x[ 3],16,0xd4ef3085) HH(b,c,d,a,x[ 6],23,0x04881d05)
    HH(a,b,c,d,x[ 9], 4,0xd9d4d039) HH(d,a,b,c,x[12],11,0xe6db99e5)
    HH(c,d,a,b,x[15],16,0x1fa27cf8) HH(b,c,d,a,x[ 2],23,0xc4ac5665)

    II(a,b,c,d,x[ 0], 6,0xf4292244) II(d,a,b,c,x[ 7],10,0x432aff97)
    II(c,d,a,b,x[14],15,0xab9423a7) II(b,c,d,a,x[ 5],21,0xfc93a039)
    II(a,b,c,d,x[12], 6,0x655b59c3) II(d,a,b,c,x[ 3],10,0x8f0ccc92)
    II(c,d,a,b,x[10],15,0xffeff47d) II(b,c,d,a,x[ 1],21,0x85845dd1)
    II(a,b,c,d,x[ 8], 6,0x6fa87e4f) II(d,a,b,c,x[15],10,0xfe2ce6e0)
    II(c,d,a,b,x[ 6],15,0xa3014314) II(b,c,d,a,x[13],21,0x4e0811a1)
    II(a,b,c,d,x[ 4], 6,0xf7537e82) II(d,a,b,c,x[11],10,0xbd3af235)
    II(c,d,a,b,x[ 2],15,0x2ad7d2bb) II(b,c,d,a,x[ 9],21,0xeb86d391)

    #undef FF
    #undef GG
    #undef HH
    #undef II

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_init(pdf_md5_ctx *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(pdf_md5_ctx *ctx, const unsigned char *data, unsigned long len)
{
    unsigned int idx = (unsigned int)((ctx->count[0] >> 3) & 0x3F);
    unsigned int part_len = 64 - idx;
    unsigned long i;

    ctx->count[0] += (pdf_u32)(len << 3);
    if (ctx->count[0] < (pdf_u32)(len << 3))
        ctx->count[1]++;
    ctx->count[1] += (pdf_u32)(len >> 29);

    if (len >= part_len)
    {
        memcpy(ctx->buffer + idx, data, part_len);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 64 <= len; i += 64)
            md5_transform(ctx->state, data + i);
        idx = 0;
    }
    else
    {
        i = 0;
    }

    memcpy(ctx->buffer + idx, data + i, len - i);
}

static void md5_final(unsigned char digest[16], pdf_md5_ctx *ctx)
{
    static const unsigned char padding[64] = { 0x80 };
    unsigned char bits[8];
    unsigned int idx, pad_len;
    int i;

    for (i = 0; i < 4; i++) bits[i]     = (unsigned char)((ctx->count[0] >> (8*i)) & 0xFF);
    for (i = 0; i < 4; i++) bits[i + 4] = (unsigned char)((ctx->count[1] >> (8*i)) & 0xFF);

    idx = (unsigned int)((ctx->count[0] >> 3) & 0x3F);
    pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, 8);

    for (i = 0; i < 4; i++)
    {
        digest[i*4]   = (unsigned char)((ctx->state[i]) & 0xFF);
        digest[i*4+1] = (unsigned char)((ctx->state[i] >> 8) & 0xFF);
        digest[i*4+2] = (unsigned char)((ctx->state[i] >> 16) & 0xFF);
        digest[i*4+3] = (unsigned char)((ctx->state[i] >> 24) & 0xFF);
    }
}

static void pdf_md5(const unsigned char *data, long len, unsigned char digest[16])
{
    pdf_md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, (unsigned long)len);
    md5_final(digest, &ctx);
}

/* ====================================================================
 * RC4
 * ==================================================================== */

static void rc4_apply(const unsigned char *key, int key_len,
                       unsigned char *data, long len)
{
    unsigned char s[256];
    int i, j;
    long k;

    for (i = 0; i < 256; i++) s[i] = (unsigned char)i;

    j = 0;
    for (i = 0; i < 256; i++)
    {
        unsigned char tmp;
        j = (j + s[i] + key[i % key_len]) & 0xFF;
        tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    }

    i = 0; j = 0;
    for (k = 0; k < len; k++)
    {
        unsigned char tmp;
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        tmp = s[i]; s[i] = s[j]; s[j] = tmp;
        data[k] ^= s[(s[i] + s[j]) & 0xFF];
    }
}

/* ====================================================================
 * Standard Security Handler: derivacion de clave (Algoritmo 3.2) y
 * clave por objeto (Algoritmo 3.1), asumiendo contrasenia de usuario
 * vacia (el unico caso automatizable sin pedirle nada a nadie).
 * ==================================================================== */

static const unsigned char PDF_PASSWORD_PAD[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

/* sufijo agregado a la derivacion de clave por objeto (Algoritmo 1,
 * norma 7.6.2 paso e) cuando el metodo es AES -- NO se usa para RC4. */
static const unsigned char PDF_AES_SALT[4] = { 0x73,0x41,0x6C,0x54 };

/* ====================================================================
 * V5/AESV3: Algoritmo 2.A (derivacion de clave de archivo) y el hash
 * "endurecido" de R6 (Algoritmo 2.B) -- verificados contra la
 * implementacion real de qpdf (hash_V5) antes de escribirse, ver
 * DESIGN.md. Solo contrasenia de usuario vacia (udata = "" siempre en
 * este modulo -- udata = U solo aplica a la contrasenia de PROPIETARIO,
 * que no se soporta).
 * ==================================================================== */

/* Bucle de endurecimiento R6: parte de un K de 32 bytes (SHA-256) y lo
 * hace crecer/mutar hasta que la condicion de corte se cumple (minimo
 * 64 rondas). 'k' entra y sale con hasta 64 bytes validos (*k_len). */
static void hash_v5_r6(unsigned char k[64], int *k_len,
                        const unsigned char *password, int pw_len,
                        const unsigned char *udata, int udata_len)
{
    unsigned char unit[256];
    unsigned char k1[64 * 256];
    long unit_len, k1_len;
    int round, done, rep;

    round = 0;
    done = 0;
    do
    {
        unit_len = 0;
        if (pw_len > 0) { memcpy(unit + unit_len, password, (size_t)pw_len); unit_len += pw_len; }
        memcpy(unit + unit_len, k, (size_t)*k_len); unit_len += *k_len;
        if (udata_len > 0) { memcpy(unit + unit_len, udata, (size_t)udata_len); unit_len += udata_len; }

        k1_len = 0;
        for (rep = 0; rep < 64; rep++)
        {
            memcpy(k1 + k1_len, unit, (size_t)unit_len);
            k1_len += unit_len;
        }

        /* AES-128-CBC sin padding: clave = k[0:16], IV = k[16:32] --
         * k1_len siempre es multiplo de 64 (64 copias de 'unit'), asi
         * que siempre es multiplo de 16 tambien. */
        pdf_aes128_cbc_encrypt_nopad(k, k + 16, k1, k1_len);

        {
            long sum = 0;
            int i, sel;
            for (i = 0; i < 16; i++) sum += k1[i];
            sel = (int)(sum % 3);
            if (sel == 0)      { pdf_sha256(k1, k1_len, k); *k_len = 32; }
            else if (sel == 1) { pdf_sha384(k1, k1_len, k); *k_len = 48; }
            else               { pdf_sha512(k1, k1_len, k); *k_len = 64; }
        }

        round++;
        if (round >= 64)
        {
            int last_byte = k1[k1_len - 1];
            if (last_byte <= round - 32) done = 1;
        }
    }
    while (!done);
}

/* hash_V5 completo: K = SHA256(password+salt+udata); si r==6, correr
 * el endurecimiento de arriba. Devuelve siempre los primeros 32 bytes. */
static void hash_v5(const unsigned char *password, int pw_len,
                     const unsigned char *salt, int salt_len,
                     const unsigned char *udata, int udata_len,
                     int r, unsigned char out32[32])
{
    unsigned char input[256];
    long input_len = 0;
    unsigned char k[64];
    int k_len;

    if (pw_len > 0) { memcpy(input + input_len, password, (size_t)pw_len); input_len += pw_len; }
    memcpy(input + input_len, salt, (size_t)salt_len); input_len += salt_len;
    if (udata_len > 0) { memcpy(input + input_len, udata, (size_t)udata_len); input_len += udata_len; }

    pdf_sha256(input, input_len, k);
    k_len = 32;

    if (r == 6)
        hash_v5_r6(k, &k_len, password, pw_len, udata, udata_len);

    memcpy(out32, k, 32);
}

/* V5 (R5/R6): clave de archivo de 256 bits derivada de /U + /UE. Sin
 * derivacion por objeto -- la misma clave se usa para todos los
 * streams (ver pdf_crypt_decrypt). No hay PDF real V5 en tests/ para
 * verificar esta ruta de punta a punta (a diferencia de V4/AESV2, que
 * si tiene un archivo real) -- los primitivos (AES-256, SHA-256/384/512)
 * estan verificados por separado contra referencias reales, pero la
 * integracion completa queda sin confirmar hasta que aparezca un PDF
 * V5 real. Ver DESIGN.md y la memoria del proyecto. */
static void pdf_crypt_init_v5(pdf_crypt *crypt, pdf_obj *encrypt_dict, long r)
{
    pdf_obj *u_obj, *ue_obj;
    const unsigned char *u_str, *ue_str;
    long u_len, ue_len;
    unsigned char validation_hash[32];
    unsigned char inter_key[32];
    unsigned char file_key[32];
    unsigned char zero_iv[16];
    int i;

    if (r != 5 && r != 6) return;

    u_obj = pdf_dict_get(encrypt_dict, "U");
    ue_obj = pdf_dict_get(encrypt_dict, "UE");
    if (u_obj == NULL || u_obj->type != PDF_STRING) return;
    if (ue_obj == NULL || ue_obj->type != PDF_STRING) return;

    u_str = (const unsigned char *)u_obj->u.str.data;
    u_len = u_obj->u.str.len;
    ue_str = (const unsigned char *)ue_obj->u.str.data;
    ue_len = ue_obj->u.str.len;
    if (u_len < 48 || ue_len < 32) return;

    /* Validar que la contrasenia de usuario es vacia: hash("" + salt de
     * validacion, U[32:40]) debe coincidir con U[0:32]. Si no coincide,
     * no es soportable -- 'active' queda en 0 en vez de descifrar con
     * una clave equivocada y producir streams corruptos silenciosamente. */
    hash_v5(NULL, 0, u_str + 32, 8, NULL, 0, (int)r, validation_hash);
    if (memcmp(validation_hash, u_str, 32) != 0)
        return;

    /* Clave intermedia: hash("" + salt de clave, U[40:48]). */
    hash_v5(NULL, 0, u_str + 40, 8, NULL, 0, (int)r, inter_key);

    /* Clave de archivo = AES-256-CBC-descifrar UE con la clave
     * intermedia, IV de ceros, sin padding (UE son exactamente 32 bytes
     * -- 2 bloques -- la clave de archivo sale completa). */
    memcpy(file_key, ue_str, 32);
    for (i = 0; i < 16; i++) zero_iv[i] = 0;
    pdf_aes_cbc_decrypt(inter_key, 256, zero_iv, file_key, 32);

    crypt->key_len = 32;
    memcpy(crypt->key, file_key, 32);
    crypt->v = 5;
    crypt->r = (int)r;
    crypt->method = PDF_CRYPT_AES256;
    crypt->active = 1;
}

void pdf_crypt_init(pdf_crypt *crypt, pdf_obj *encrypt_dict,
                     const unsigned char *id0, long id0_len)
{
    pdf_obj *o_obj;
    long v, r, length_bits, p;
    const unsigned char *o_str;
    long o_len;
    unsigned char buf[32 + 32 + 4 + 32 + 4 + 8]; /* pad + O + P(4) + ID0 + metadata(4) + margen */
    long buf_len;
    unsigned char digest[16];
    int key_len_bytes;
    int i;
    pdf_crypt_method method;
    int encrypt_metadata;

    memset(crypt, 0, sizeof(*crypt));

    if (encrypt_dict == NULL || (encrypt_dict->type != PDF_DICT && encrypt_dict->type != PDF_STREAM))
        return;

    {
        const char *filter = pdf_dict_get_name(encrypt_dict, "Filter");
        if (filter != NULL && strcmp(filter, "Standard") != 0)
            return; /* handler no estandar: no soportado */
    }

    v = pdf_dict_get_int(encrypt_dict, "V", 0);
    r = pdf_dict_get_int(encrypt_dict, "R", 0);
    length_bits = pdf_dict_get_int(encrypt_dict, "Length", 40);
    p = pdf_dict_get_int(encrypt_dict, "P", 0);

    if (v == 5)
    {
        pdf_crypt_init_v5(crypt, encrypt_dict, r);
        return;
    }

    if (v != 1 && v != 2 && v != 4)
        return;
    if (v == 4) { if (r != 4) return; }
    else        { if (r != 2 && r != 3) return; }

    method = PDF_CRYPT_RC4;
    key_len_bytes = (int)(length_bits / 8);
    if (key_len_bytes < 5) key_len_bytes = 5;
    if (key_len_bytes > 16) key_len_bytes = 16;

    if (v == 4)
    {
        /* V4 usa /CF (diccionario de filtros de cifrado nombrados) +
         * /StmF (nombre del filtro que aplica a los streams, default
         * "StdCF" si falta). Solo se resuelve el metodo de STREAMS --
         * ver nota de alcance en pdf_crypt.h sobre /StrF. */
        pdf_obj *cf_obj, *stmf_filter_obj;
        const char *stmf_name;
        const char *cfm;

        cf_obj = pdf_dict_get(encrypt_dict, "CF");
        if (cf_obj == NULL) return;

        stmf_name = pdf_dict_get_name(encrypt_dict, "StmF");
        if (stmf_name == NULL) stmf_name = "StdCF";

        stmf_filter_obj = pdf_dict_get(cf_obj, stmf_name);
        if (stmf_filter_obj == NULL) return;

        cfm = pdf_dict_get_name(stmf_filter_obj, "CFM");
        if (cfm == NULL) return;

        if (strcmp(cfm, "AESV2") == 0)
        {
            method = PDF_CRYPT_AES128;
            key_len_bytes = 16; /* AESV2 siempre es 128 bits, sin importar /Length */
        }
        else if (strcmp(cfm, "V2") == 0)
        {
            method = PDF_CRYPT_RC4; /* V4 con RC4 explicito -- raro pero valido */
        }
        else
        {
            return; /* AESV3 (eso es V5), None, o algo no reconocido */
        }
    }

    o_obj = pdf_dict_get(encrypt_dict, "O");
    if (o_obj == NULL || o_obj->type != PDF_STRING)
        return;
    o_str = (const unsigned char *)o_obj->u.str.data;
    o_len = o_obj->u.str.len;
    if (o_len < 32) return;

    encrypt_metadata = 1;
    if (r >= 4)
    {
        pdf_obj *em = pdf_dict_get(encrypt_dict, "EncryptMetadata");
        if (em != NULL && em->type == PDF_BOOL) encrypt_metadata = em->u.boolean;
    }

    /* Algoritmo 3.2: MD5( pad_contrasenia_usuario(vacia) + O + P(4 bytes LE) + ID0 [+ 0xFFFFFFFF si R>=4 y metadata no encriptada] ) */
    buf_len = 0;
    memcpy(buf + buf_len, PDF_PASSWORD_PAD, 32); buf_len += 32; /* contrasenia de usuario vacia = solo el padding */
    memcpy(buf + buf_len, o_str, 32); buf_len += 32;
    buf[buf_len++] = (unsigned char)(p & 0xFF);
    buf[buf_len++] = (unsigned char)((p >> 8) & 0xFF);
    buf[buf_len++] = (unsigned char)((p >> 16) & 0xFF);
    buf[buf_len++] = (unsigned char)((p >> 24) & 0xFF);
    if (id0 != NULL && id0_len > 0)
    {
        long take = id0_len;
        if (buf_len + take > (long)sizeof(buf) - 4) take = (long)sizeof(buf) - 4 - buf_len;
        memcpy(buf + buf_len, id0, (size_t)take);
        buf_len += take;
    }
    if (r >= 4 && !encrypt_metadata)
    {
        buf[buf_len++] = 0xFF;
        buf[buf_len++] = 0xFF;
        buf[buf_len++] = 0xFF;
        buf[buf_len++] = 0xFF;
    }

    pdf_md5(buf, buf_len, digest);

    if (r >= 3)
    {
        for (i = 0; i < 50; i++)
            pdf_md5(digest, key_len_bytes, digest);
    }

    crypt->key_len = key_len_bytes;
    memcpy(crypt->key, digest, (size_t)key_len_bytes);
    crypt->v = (int)v;
    crypt->r = (int)r;
    crypt->method = method;
    crypt->active = 1;
}

long pdf_crypt_decrypt(const pdf_crypt *crypt, long obj_num, long obj_gen,
                        unsigned char *data, long len)
{
    unsigned char buf[16 + 5 + 4 + 4]; /* file key + 3 bytes objnum + 2 bytes gen + sAlT(4) + margen */
    long buf_len;
    unsigned char digest[16];
    int obj_key_len;

    if (crypt == NULL || !crypt->active || data == NULL || len <= 0)
        return len;

    if (crypt->method == PDF_CRYPT_AES256)
    {
        /* V5: la clave de archivo se usa directo, sin Algoritmo 1. */
        unsigned char iv[16];
        long cipher_len;
        int pad;

        if (len < 32) return 0;
        cipher_len = len - 16;
        if ((cipher_len % 16) != 0) return cipher_len; /* stream corrupto/no-AES: no tocar */

        memcpy(iv, data, 16);
        memmove(data, data + 16, (size_t)cipher_len);
        pdf_aes_cbc_decrypt(crypt->key, 256, iv, data, cipher_len);

        pad = data[cipher_len - 1];
        if (pad < 1 || pad > 16 || pad > cipher_len) return cipher_len;
        return cipher_len - pad;
    }

    /* Algoritmo 1: clave de objeto = MD5(clave_archivo + 3 bytes num +
     * 2 bytes gen [+ "sAlT" si el metodo es AES]), truncada a
     * min(key_len+5, 16) -- para AES eso siempre da 16 (key_len ya es
     * 16 en el caso AESV2), o sea el digest MD5 completo sin truncar. */
    buf_len = 0;
    memcpy(buf + buf_len, crypt->key, (size_t)crypt->key_len); buf_len += crypt->key_len;
    buf[buf_len++] = (unsigned char)(obj_num & 0xFF);
    buf[buf_len++] = (unsigned char)((obj_num >> 8) & 0xFF);
    buf[buf_len++] = (unsigned char)((obj_num >> 16) & 0xFF);
    buf[buf_len++] = (unsigned char)(obj_gen & 0xFF);
    buf[buf_len++] = (unsigned char)((obj_gen >> 8) & 0xFF);
    if (crypt->method == PDF_CRYPT_AES128)
    {
        memcpy(buf + buf_len, PDF_AES_SALT, 4);
        buf_len += 4;
    }

    pdf_md5(buf, buf_len, digest);

    obj_key_len = crypt->key_len + 5;
    if (obj_key_len > 16) obj_key_len = 16;

    if (crypt->method == PDF_CRYPT_AES128)
    {
        unsigned char iv[16];
        long cipher_len;
        int pad;

        if (len < 32) return 0;
        cipher_len = len - 16;
        if ((cipher_len % 16) != 0) return cipher_len; /* stream corrupto/no-AES: no tocar */

        memcpy(iv, data, 16);
        memmove(data, data + 16, (size_t)cipher_len);
        pdf_aes_cbc_decrypt(digest, 128, iv, data, cipher_len);

        pad = data[cipher_len - 1];
        if (pad < 1 || pad > 16 || pad > cipher_len) return cipher_len;
        return cipher_len - pad;
    }

    rc4_apply(digest, obj_key_len, data, len);
    return len;
}
