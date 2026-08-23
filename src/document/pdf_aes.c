/* pdf_aes.c
 *
 * Ver pdf_aes.h. AES-128/256 (FIPS-197) escrito desde cero, C89, sin
 * dependencias -- mismo estilo que el MD5/RC4 de pdf_crypt.c. Estado
 * representado como bloque de 16 bytes en orden columna-mayor (FIPS-197:
 * state[r][c] = in[r + 4*c]), igual que el estandar.
 *
 * Verificado por fuera con 'openssl enc' (vectores FIPS-197 Appendix B
 * y C.3) antes de integrarse a pdf_crypt.c -- ver DESIGN.md.
 */

#include "pdf_aes.h"
#include <string.h>

typedef unsigned char u8;

/* ====================================================================
 * Tablas FIPS-197
 * ==================================================================== */

static const u8 AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const u8 AES_INV_SBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Rcon[i] = x^(i-1) en GF(2^8), byte alto de la word de round constant.
 * Rcon[0] no se usa (arranca en 1). Hacen falta hasta el indice 10 para
 * AES-128 (Nk=4, Nr=10) y hasta el 7 para AES-256 (Nk=8, Nr=14) -- ambos
 * caben en esta tabla de 10. */
static const u8 AES_RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36
};

/* ====================================================================
 * GF(2^8) y transformaciones de estado
 * ==================================================================== */

static u8 gmul(u8 a, u8 b)
{
    u8 p = 0;
    int i;
    for (i = 0; i < 8; i++)
    {
        if (b & 1) p ^= a;
        {
            u8 hi = (u8)(a & 0x80);
            a = (u8)(a << 1);
            if (hi) a = (u8)(a ^ 0x1B);
        }
        b = (u8)(b >> 1);
    }
    return p;
}

static void sub_bytes(u8 *state)
{
    int i;
    for (i = 0; i < 16; i++) state[i] = AES_SBOX[state[i]];
}

static void inv_sub_bytes(u8 *state)
{
    int i;
    for (i = 0; i < 16; i++) state[i] = AES_INV_SBOX[state[i]];
}

/* state[r + 4*c]: fila r se rota a la izquierda r posiciones (columnas). */
static void shift_rows(u8 *state)
{
    u8 t;
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    t = state[3]; state[3] = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = t;
}

static void inv_shift_rows(u8 *state)
{
    u8 t;
    t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    t = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = state[3]; state[3] = t;
}

static void mix_columns(u8 *state)
{
    int c;
    for (c = 0; c < 4; c++)
    {
        u8 *s = state + 4 * c;
        u8 a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (u8)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
        s[1] = (u8)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
        s[2] = (u8)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
        s[3] = (u8)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
    }
}

static void inv_mix_columns(u8 *state)
{
    int c;
    for (c = 0; c < 4; c++)
    {
        u8 *s = state + 4 * c;
        u8 a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
        s[0] = (u8)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
        s[1] = (u8)(gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
        s[2] = (u8)(gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11));
        s[3] = (u8)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14));
    }
}

static void add_round_key(u8 *state, const u8 *round_key)
{
    int i;
    for (i = 0; i < 16; i++) state[i] ^= round_key[i];
}

/* ====================================================================
 * Expansion de clave (FIPS-197 Figure 11)
 * ==================================================================== */

/* round_key: buffer de salida, 4*(Nr+1) words de 4 bytes (240 bytes
 * alcanza para el caso mas grande, AES-256/Nr=14 -> 60 words). */
static void aes_key_expansion(const unsigned char *key, int key_bits,
                               unsigned char round_key[240], int *nr_out)
{
    int nk, nr, total_words, i;
    unsigned char temp[4];

    nk = key_bits / 32; /* 4 (128 bits) u 8 (256 bits) */
    nr = nk + 6;         /* 10 u 14 */
    total_words = 4 * (nr + 1);

    memcpy(round_key, key, (size_t)(4 * nk));

    for (i = nk; i < total_words; i++)
    {
        unsigned char *w = round_key + 4 * i;
        const unsigned char *prev = round_key + 4 * (i - 1);
        memcpy(temp, prev, 4);

        if (i % nk == 0)
        {
            unsigned char t0 = temp[0];
            temp[0] = AES_SBOX[temp[1]];
            temp[1] = AES_SBOX[temp[2]];
            temp[2] = AES_SBOX[temp[3]];
            temp[3] = AES_SBOX[t0];
            temp[0] = (unsigned char)(temp[0] ^ AES_RCON[i / nk]);
        }
        else if (nk > 6 && (i % nk) == 4)
        {
            temp[0] = AES_SBOX[temp[0]];
            temp[1] = AES_SBOX[temp[1]];
            temp[2] = AES_SBOX[temp[2]];
            temp[3] = AES_SBOX[temp[3]];
        }

        {
            const unsigned char *back = round_key + 4 * (i - nk);
            int j;
            for (j = 0; j < 4; j++) w[j] = (unsigned char)(back[j] ^ temp[j]);
        }
    }

    *nr_out = nr;
}

/* ====================================================================
 * Cifrado / descifrado de un bloque de 16 bytes
 * ==================================================================== */

static void aes_encrypt_block(const unsigned char round_key[240], int nr, unsigned char *block)
{
    int round;

    add_round_key(block, round_key);

    for (round = 1; round < nr; round++)
    {
        sub_bytes(block);
        shift_rows(block);
        mix_columns(block);
        add_round_key(block, round_key + 16 * round);
    }

    sub_bytes(block);
    shift_rows(block);
    add_round_key(block, round_key + 16 * nr);
}

/* Inverse Cipher directo (FIPS-197 Figure 12) -- mismo orden de claves
 * de ronda que el cifrado, sin necesidad de transformarlas primero. */
static void aes_decrypt_block(const unsigned char round_key[240], int nr, unsigned char *block)
{
    int round;

    add_round_key(block, round_key + 16 * nr);

    for (round = nr - 1; round >= 1; round--)
    {
        inv_shift_rows(block);
        inv_sub_bytes(block);
        add_round_key(block, round_key + 16 * round);
        inv_mix_columns(block);
    }

    inv_shift_rows(block);
    inv_sub_bytes(block);
    add_round_key(block, round_key);
}

/* ====================================================================
 * API publica: CBC
 * ==================================================================== */

void pdf_aes_cbc_decrypt(const unsigned char *key, int key_bits,
                          const unsigned char *iv,
                          unsigned char *data, long len)
{
    unsigned char round_key[240];
    unsigned char prev[16];
    unsigned char cipher_copy[16];
    int nr;
    long off;

    if (data == NULL || len <= 0 || (len % 16) != 0) return;
    if (key_bits != 128 && key_bits != 256) return;

    aes_key_expansion(key, key_bits, round_key, &nr);
    memcpy(prev, iv, 16);

    for (off = 0; off < len; off += 16)
    {
        unsigned char *block = data + off;
        memcpy(cipher_copy, block, 16);
        aes_decrypt_block(round_key, nr, block);
        {
            int i;
            for (i = 0; i < 16; i++) block[i] = (unsigned char)(block[i] ^ prev[i]);
        }
        memcpy(prev, cipher_copy, 16);
    }
}

void pdf_aes128_cbc_encrypt_nopad(const unsigned char *key,
                                   const unsigned char *iv,
                                   unsigned char *data, long len)
{
    unsigned char round_key[240];
    unsigned char prev[16];
    int nr;
    long off;

    if (data == NULL || len <= 0 || (len % 16) != 0) return;

    aes_key_expansion(key, 128, round_key, &nr);
    memcpy(prev, iv, 16);

    for (off = 0; off < len; off += 16)
    {
        unsigned char *block = data + off;
        int i;
        for (i = 0; i < 16; i++) block[i] = (unsigned char)(block[i] ^ prev[i]);
        aes_encrypt_block(round_key, nr, block);
        memcpy(prev, block, 16);
    }
}
