/* pdf_sha2.c
 *
 * Ver pdf_sha2.h. SHA-256 (FIPS 180-4, palabras de 32 bits) y
 * SHA-384/SHA-512 (mismo estandar, palabras de 64 bits -- comparten
 * nucleo, solo cambian IV, tabla de constantes y largo de salida)
 * escritos desde cero, C89. bcc32 7.70 soporta 'unsigned __int64'
 * nativo (confirmado con un harness de prueba: rotacion y wraparound
 * de 64 bits dan el resultado esperado), asi que SHA-512/384 se
 * implementan con aritmetica de 64 bits real, no pares de 32 bits.
 *
 * Verificado por fuera contra 'python -c "import hashlib; ..."' antes
 * de integrarse a pdf_crypt.c -- ver DESIGN.md.
 */

#include "pdf_sha2.h"
#include <string.h>

typedef unsigned int  u32;
typedef unsigned __int64 u64;

/* ====================================================================
 * SHA-256
 * ==================================================================== */

static const u32 SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static u32 sha256_rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(u32 state[8], const unsigned char block[64])
{
    u32 w[64];
    u32 a,b,c,d,e,f,g,h;
    int t;

    for (t = 0; t < 16; t++)
        w[t] = ((u32)block[t*4] << 24) | ((u32)block[t*4+1] << 16) |
               ((u32)block[t*4+2] << 8) | (u32)block[t*4+3];

    for (t = 16; t < 64; t++)
    {
        u32 s0 = sha256_rotr(w[t-15],7) ^ sha256_rotr(w[t-15],18) ^ (w[t-15] >> 3);
        u32 s1 = sha256_rotr(w[t-2],17) ^ sha256_rotr(w[t-2],19) ^ (w[t-2] >> 10);
        w[t] = w[t-16] + s0 + w[t-7] + s1;
    }

    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];

    for (t = 0; t < 64; t++)
    {
        u32 S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + S1 + ch + SHA256_K[t] + w[t];
        u32 S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;

        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

void pdf_sha256(const unsigned char *data, long len, unsigned char digest[32])
{
    u32 state[8];
    unsigned char block[64];
    long full_blocks, rem, off;
    unsigned char tail[128]; /* rem (<64) + 0x80 + padding + 8 bytes de largo, cabe holgado */
    long tail_len, pad_len;
    unsigned __int64 bitlen;
    int i;

    state[0]=0x6a09e667; state[1]=0xbb67ae85; state[2]=0x3c6ef372; state[3]=0xa54ff53a;
    state[4]=0x510e527f; state[5]=0x9b05688c; state[6]=0x1f83d9ab; state[7]=0x5be0cd19;

    full_blocks = len / 64;
    rem = len % 64;

    for (off = 0; off < full_blocks * 64; off += 64)
    {
        memcpy(block, data + off, 64);
        sha256_transform(state, block);
    }

    bitlen = (unsigned __int64)len * 8;

    memcpy(tail, data + full_blocks * 64, (size_t)rem);
    tail_len = rem;
    tail[tail_len++] = 0x80;
    pad_len = (tail_len <= 56) ? (56 - tail_len) : (120 - tail_len);
    memset(tail + tail_len, 0, (size_t)pad_len);
    tail_len += pad_len;
    for (i = 7; i >= 0; i--)
        tail[tail_len++] = (unsigned char)((bitlen >> (8*i)) & 0xFF);

    for (off = 0; off < tail_len; off += 64)
    {
        memcpy(block, tail + off, 64);
        sha256_transform(state, block);
    }

    for (i = 0; i < 8; i++)
    {
        digest[i*4]   = (unsigned char)((state[i] >> 24) & 0xFF);
        digest[i*4+1] = (unsigned char)((state[i] >> 16) & 0xFF);
        digest[i*4+2] = (unsigned char)((state[i] >> 8) & 0xFF);
        digest[i*4+3] = (unsigned char)(state[i] & 0xFF);
    }
}

/* ====================================================================
 * SHA-512 / SHA-384 (nucleo comun de 64 bits)
 * ==================================================================== */

static const u64 SHA512_K[80] = {
    0x428a2f98d728ae22ui64,0x7137449123ef65cdui64,0xb5c0fbcfec4d3b2fui64,0xe9b5dba58189dbbcui64,
    0x3956c25bf348b538ui64,0x59f111f1b605d019ui64,0x923f82a4af194f9bui64,0xab1c5ed5da6d8118ui64,
    0xd807aa98a3030242ui64,0x12835b0145706fbeui64,0x243185be4ee4b28cui64,0x550c7dc3d5ffb4e2ui64,
    0x72be5d74f27b896fui64,0x80deb1fe3b1696b1ui64,0x9bdc06a725c71235ui64,0xc19bf174cf692694ui64,
    0xe49b69c19ef14ad2ui64,0xefbe4786384f25e3ui64,0x0fc19dc68b8cd5b5ui64,0x240ca1cc77ac9c65ui64,
    0x2de92c6f592b0275ui64,0x4a7484aa6ea6e483ui64,0x5cb0a9dcbd41fbd4ui64,0x76f988da831153b5ui64,
    0x983e5152ee66dfabui64,0xa831c66d2db43210ui64,0xb00327c898fb213fui64,0xbf597fc7beef0ee4ui64,
    0xc6e00bf33da88fc2ui64,0xd5a79147930aa725ui64,0x06ca6351e003826fui64,0x142929670a0e6e70ui64,
    0x27b70a8546d22ffcui64,0x2e1b21385c26c926ui64,0x4d2c6dfc5ac42aedui64,0x53380d139d95b3dfui64,
    0x650a73548baf63deui64,0x766a0abb3c77b2a8ui64,0x81c2c92e47edaee6ui64,0x92722c851482353bui64,
    0xa2bfe8a14cf10364ui64,0xa81a664bbc423001ui64,0xc24b8b70d0f89791ui64,0xc76c51a30654be30ui64,
    0xd192e819d6ef5218ui64,0xd69906245565a910ui64,0xf40e35855771202aui64,0x106aa07032bbd1b8ui64,
    0x19a4c116b8d2d0c8ui64,0x1e376c085141ab53ui64,0x2748774cdf8eeb99ui64,0x34b0bcb5e19b48a8ui64,
    0x391c0cb3c5c95a63ui64,0x4ed8aa4ae3418acbui64,0x5b9cca4f7763e373ui64,0x682e6ff3d6b2b8a3ui64,
    0x748f82ee5defb2fcui64,0x78a5636f43172f60ui64,0x84c87814a1f0ab72ui64,0x8cc702081a6439ecui64,
    0x90befffa23631e28ui64,0xa4506cebde82bde9ui64,0xbef9a3f7b2c67915ui64,0xc67178f2e372532bui64,
    0xca273eceea26619cui64,0xd186b8c721c0c207ui64,0xeada7dd6cde0eb1eui64,0xf57d4f7fee6ed178ui64,
    0x06f067aa72176fbaui64,0x0a637dc5a2c898a6ui64,0x113f9804bef90daeui64,0x1b710b35131c471bui64,
    0x28db77f523047d84ui64,0x32caab7b40c72493ui64,0x3c9ebe0a15c9bebcui64,0x431d67c49c100d4cui64,
    0x4cc5d4becb3e42b6ui64,0x597f299cfc657e2aui64,0x5fcb6fab3ad6faecui64,0x6c44198c4a475817ui64
};

static u64 sha512_rotr(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_transform(u64 state[8], const unsigned char block[128])
{
    u64 w[80];
    u64 a,b,c,d,e,f,g,h;
    int t;

    for (t = 0; t < 16; t++)
    {
        int i;
        u64 v = 0;
        for (i = 0; i < 8; i++) v = (v << 8) | block[t*8+i];
        w[t] = v;
    }

    for (t = 16; t < 80; t++)
    {
        u64 s0 = sha512_rotr(w[t-15],1) ^ sha512_rotr(w[t-15],8) ^ (w[t-15] >> 7);
        u64 s1 = sha512_rotr(w[t-2],19) ^ sha512_rotr(w[t-2],61) ^ (w[t-2] >> 6);
        w[t] = w[t-16] + s0 + w[t-7] + s1;
    }

    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];

    for (t = 0; t < 80; t++)
    {
        u64 S1 = sha512_rotr(e,14) ^ sha512_rotr(e,18) ^ sha512_rotr(e,41);
        u64 ch = (e & f) ^ ((~e) & g);
        u64 t1 = h + S1 + ch + SHA512_K[t] + w[t];
        u64 S0 = sha512_rotr(a,28) ^ sha512_rotr(a,34) ^ sha512_rotr(a,39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = S0 + maj;

        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

/* Nucleo comun: corre SHA-512/384 sobre 'data', devuelve los 8 words
 * de estado finales de 64 bits (el llamador recorta a 48 o 64 bytes de
 * salida y arma los bytes big-endian). El largo se codifica en un
 * campo de 128 bits por la norma, pero solo los 64 bits bajos importan
 * en la practica (los usos reales de este modulo nunca pasan de unos
 * pocos KB) -- los 64 bits altos quedan siempre en 0. */
static void sha512_core(const unsigned char *data, long len, const u64 iv[8], u64 state[8])
{
    unsigned char block[128];
    long full_blocks, rem, off;
    unsigned char tail[256];
    long tail_len, pad_len;
    unsigned __int64 bitlen;
    int i;

    memcpy(state, iv, 8 * sizeof(u64));

    full_blocks = len / 128;
    rem = len % 128;

    for (off = 0; off < full_blocks * 128; off += 128)
    {
        memcpy(block, data + off, 128);
        sha512_transform(state, block);
    }

    bitlen = (unsigned __int64)len * 8;

    memcpy(tail, data + full_blocks * 128, (size_t)rem);
    tail_len = rem;
    tail[tail_len++] = 0x80;
    pad_len = (tail_len <= 112) ? (112 - tail_len) : (240 - tail_len);
    memset(tail + tail_len, 0, (size_t)pad_len);
    tail_len += pad_len;
    for (i = 0; i < 8; i++) tail[tail_len++] = 0; /* mitad alta de 128 bits, siempre 0 */
    for (i = 7; i >= 0; i--)
        tail[tail_len++] = (unsigned char)((bitlen >> (8*i)) & 0xFF);

    for (off = 0; off < tail_len; off += 128)
    {
        memcpy(block, tail + off, 128);
        sha512_transform(state, block);
    }
}

static void state_to_bytes(const u64 state[8], unsigned char *out, int n_words)
{
    int i, j;
    for (i = 0; i < n_words; i++)
        for (j = 0; j < 8; j++)
            out[i*8+j] = (unsigned char)((state[i] >> (8*(7-j))) & 0xFF);
}

void pdf_sha512(const unsigned char *data, long len, unsigned char digest[64])
{
    static const u64 iv[8] = {
        0x6a09e667f3bcc908ui64,0xbb67ae8584caa73bui64,0x3c6ef372fe94f82bui64,0xa54ff53a5f1d36f1ui64,
        0x510e527fade682d1ui64,0x9b05688c2b3e6c1fui64,0x1f83d9abfb41bd6bui64,0x5be0cd19137e2179ui64
    };
    u64 state[8];
    sha512_core(data, len, iv, state);
    state_to_bytes(state, digest, 8);
}

void pdf_sha384(const unsigned char *data, long len, unsigned char digest[48])
{
    static const u64 iv[8] = {
        0xcbbb9d5dc1059ed8ui64,0x629a292a367cd507ui64,0x9159015a3070dd17ui64,0x152fecd8f70e5939ui64,
        0x67332667ffc00b31ui64,0x8eb44a8768581511ui64,0xdb0c2e0d64f98fa7ui64,0x47b5481dbefa4fa4ui64
    };
    u64 state[8];
    sha512_core(data, len, iv, state);
    state_to_bytes(state, digest, 6);
}
