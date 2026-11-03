#include "hal_mbedtls.h"
#include "hal_psa.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#ifdef OPENSSL_VERSION_MAJOR
#if OPENSSL_VERSION_MAJOR >= 3
#include <openssl/core_names.h>
#endif
#endif
#endif

/* AES-128 (FIPS-197) so SD MRBI+CTR works when OpenSSL is not linked.
 * Luckfox cross-builds often miss find_package(OpenSSL); ECB used to memcpy
 * the counter and CTR aborted, leaving plaintext after the MRBI header. */
static const uint8_t s_aes_sbox[256] = {
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

static uint8_t aes_xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
}

static void aes128_expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    memcpy(rk, key, 16);
    int r = 0;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
        if ((i & 15) == 0) {
            uint8_t tmp = t0;
            t0 = (uint8_t)(s_aes_sbox[t1] ^ rcon[r++]);
            t1 = s_aes_sbox[t2];
            t2 = s_aes_sbox[t3];
            t3 = s_aes_sbox[tmp];
        }
        rk[i]     = (uint8_t)(rk[i - 16] ^ t0);
        rk[i + 1] = (uint8_t)(rk[i - 15] ^ t1);
        rk[i + 2] = (uint8_t)(rk[i - 14] ^ t2);
        rk[i + 3] = (uint8_t)(rk[i - 13] ^ t3);
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; ++i) {
        s[i] ^= rk[i];
    }
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) {
            s[i] = s_aes_sbox[s[i]];
        }
        uint8_t t;
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;     t = s[6];      s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
        if (round < 10) {
            for (int c = 0; c < 4; ++c) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(aes_xtime(a0) ^ aes_xtime(a1) ^ a1 ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ aes_xtime(a1) ^ aes_xtime(a2) ^ a2 ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ aes_xtime(a2) ^ aes_xtime(a3) ^ a3);
                col[3] = (uint8_t)(aes_xtime(a0) ^ a0 ^ a1 ^ a2 ^ aes_xtime(a3));
            }
        }
        const uint8_t *kr = rk + round * 16;
        for (int i = 0; i < 16; ++i) {
            s[i] ^= kr[i];
        }
    }
    memcpy(out, s, 16);
}

static int aes128_selftest(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                             0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    const uint8_t pt[16]  = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                             0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    const uint8_t exp[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                             0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t rk[176], out[16];
    aes128_expand(key, rk);
    aes128_encrypt_block(rk, pt, out);
    cached = (memcmp(out, exp, 16) == 0) ? 1 : 0;
    return cached;
}

void mbedtls_aes_init(mbedtls_aes_context *ctx) { if (ctx) memset(ctx, 0, sizeof(*ctx)); }
void mbedtls_aes_free(mbedtls_aes_context *ctx) { if (ctx) memset(ctx, 0, sizeof(*ctx)); }
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned bits) {
    if (!ctx || !key || (bits != 128 && bits != 256)) return -1;
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key, bits / 8);
    ctx->bits = (int)bits;
#ifndef HAVE_OPENSSL
    if (bits != 128 || !aes128_selftest()) {
        return -1;
    }
    aes128_expand(key, ctx->rk);
    ctx->ready = 1;
#endif
    return 0;
}

#ifdef HAVE_OPENSSL
static int aes_ecb(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *ci = (bits == 256) ? EVP_aes_256_ecb() : EVP_aes_128_ecb();
    int outl = 0, fin = 0;
    int ok = c && EVP_EncryptInit_ex(c, ci, nullptr, key, nullptr) &&
             EVP_CIPHER_CTX_set_padding(c, 0) &&
             EVP_EncryptUpdate(c, out, &outl, in, 16) &&
             EVP_EncryptFinal_ex(c, out + outl, &fin);
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}
#endif

int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int, const unsigned char in[16],
                          unsigned char out[16]) {
    if (!ctx || !in || !out) return -1;
#ifdef HAVE_OPENSSL
    return aes_ecb(ctx->key, ctx->bits, in, out);
#else
    if (!ctx->ready || ctx->bits != 128) return -1;
    aes128_encrypt_block(ctx->rk, in, out);
    return 0;
#endif
}

int mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off,
                          unsigned char nonce_counter[16], unsigned char stream_block[16],
                          const unsigned char *input, unsigned char *output) {
    size_t n = nc_off ? *nc_off : 0;
    size_t i = 0;
    while (i < length) {
        if (n == 0) {
            if (mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_ENCRYPT, nonce_counter, stream_block) != 0) {
                return -1;
            }
            for (int k = 15; k >= 0; --k) {
                if (++nonce_counter[k] != 0) break;
            }
        }
        output[i] = (unsigned char)(input[i] ^ stream_block[n]);
        n = (n + 1) & 15;
        ++i;
    }
    if (nc_off) *nc_off = n;
    return 0;
}

void mbedtls_gcm_init(mbedtls_gcm_context *ctx) { if (ctx) memset(ctx, 0, sizeof(*ctx)); }
void mbedtls_gcm_free(mbedtls_gcm_context *ctx) { if (ctx) memset(ctx, 0, sizeof(*ctx)); }
int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, int, const unsigned char *key, unsigned bits) {
    memcpy(ctx->key, key, bits / 8);
    ctx->bits = (int)bits;
    return 0;
}

#ifdef HAVE_OPENSSL
static int gcm_crypt(mbedtls_gcm_context *ctx, int enc, size_t length,
                     const unsigned char *iv, size_t iv_len,
                     const unsigned char *add, size_t add_len,
                     const unsigned char *input, unsigned char *output,
                     size_t tag_len, unsigned char *tag, int decrypt) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *ci = (ctx->bits == 256) ? EVP_aes_256_gcm() : EVP_aes_128_gcm();
    int outl = 0, fin = 0, ok = 0;
    if (!c) return -1;
    if (decrypt) {
        ok = EVP_DecryptInit_ex(c, ci, nullptr, nullptr, nullptr) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, nullptr) &&
             EVP_DecryptInit_ex(c, nullptr, nullptr, ctx->key, iv);
        if (ok && add && add_len) EVP_DecryptUpdate(c, nullptr, &outl, add, (int)add_len);
        if (ok) ok = EVP_DecryptUpdate(c, output, &outl, input, (int)length);
        if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, (int)tag_len, (void *)tag);
        if (ok) ok = EVP_DecryptFinal_ex(c, output + outl, &fin);
    } else {
        ok = EVP_EncryptInit_ex(c, ci, nullptr, nullptr, nullptr) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, nullptr) &&
             EVP_EncryptInit_ex(c, nullptr, nullptr, ctx->key, iv);
        if (ok && add && add_len) EVP_EncryptUpdate(c, nullptr, &outl, add, (int)add_len);
        if (ok) ok = EVP_EncryptUpdate(c, output, &outl, input, (int)length);
        if (ok) ok = EVP_EncryptFinal_ex(c, output + outl, &fin);
        if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, (int)tag_len, tag);
    }
    (void)enc;
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}
#endif

int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length,
                              const unsigned char *iv, size_t iv_len,
                              const unsigned char *add, size_t add_len,
                              const unsigned char *input, unsigned char *output,
                              size_t tag_len, unsigned char *tag) {
#ifdef HAVE_OPENSSL
    return gcm_crypt(ctx, mode, length, iv, iv_len, add, add_len, input, output, tag_len, tag, 0);
#else
    (void)ctx; (void)mode; (void)length; (void)iv; (void)iv_len; (void)add; (void)add_len;
    (void)input; (void)output; (void)tag_len; (void)tag;
    return -1;
#endif
}

int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length,
                             const unsigned char *iv, size_t iv_len,
                             const unsigned char *add, size_t add_len,
                             const unsigned char *tag, size_t tag_len,
                             const unsigned char *input, unsigned char *output) {
#ifdef HAVE_OPENSSL
    return gcm_crypt(ctx, 0, length, iv, iv_len, add, add_len, input, output, tag_len,
                     (unsigned char *)tag, 1);
#else
    (void)ctx; (void)length; (void)iv; (void)iv_len; (void)add; (void)add_len;
    (void)tag; (void)tag_len; (void)input; (void)output;
    return -1;
#endif
}

/* Software SHA-256 so login/HMAC work when OpenSSL is not linked (Luckfox
 * cross-builds often miss find_package(OpenSSL) and used to hash to zeros). */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} sha256_sw_t;

static uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_sw_transform(sha256_sw_t *ctx, const uint8_t data[64]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t m[64];
    for (int i = 0; i < 16; ++i) {
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = sha256_rotr(m[i - 15], 7) ^ sha256_rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(m[i - 2], 17) ^ sha256_rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + m[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_sw_init(sha256_sw_t *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_sw_update(sha256_sw_t *ctx, const unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_sw_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_sw_final(sha256_sw_t *ctx, unsigned char hash[32]) {
    size_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_sw_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_sw_transform(ctx, ctx->data);
    for (i = 0; i < 8; ++i) {
        hash[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_sw(const unsigned char *input, size_t ilen, unsigned char output[32]) {
    sha256_sw_t ctx;
    sha256_sw_init(&ctx);
    sha256_sw_update(&ctx, input, ilen);
    sha256_sw_final(&ctx, output);
}

int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char output[32], int is224) {
    (void)is224;
#ifdef HAVE_OPENSSL
    SHA256(input, ilen, output);
    return 0;
#else
    if (!output) return -1;
    sha256_sw(input ? input : (const unsigned char *)"", input ? ilen : 0, output);
    return 0;
#endif
}

#ifdef HAVE_OPENSSL
typedef struct { SHA256_CTX c; } sha_impl_t;
#endif

void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
#ifdef HAVE_OPENSSL
    ctx->impl = malloc(sizeof(SHA256_CTX));
#else
    ctx->impl = malloc(sizeof(sha256_sw_t));
#endif
}
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) {
    if (!ctx) return;
    free(ctx->impl);
    ctx->impl = nullptr;
}
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int) {
    if (!ctx) return -1;
#ifdef HAVE_OPENSSL
    if (!ctx->impl) ctx->impl = malloc(sizeof(SHA256_CTX));
    SHA256_Init((SHA256_CTX *)ctx->impl);
    return 0;
#else
    if (!ctx->impl) ctx->impl = malloc(sizeof(sha256_sw_t));
    if (!ctx->impl) return -1;
    sha256_sw_init((sha256_sw_t *)ctx->impl);
    return 0;
#endif
}
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen) {
#ifdef HAVE_OPENSSL
    SHA256_Update((SHA256_CTX *)ctx->impl, input, ilen);
    return 0;
#else
    if (!ctx || !ctx->impl) return -1;
    sha256_sw_update((sha256_sw_t *)ctx->impl, input ? input : (const unsigned char *)"",
                     input ? ilen : 0);
    return 0;
#endif
}
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]) {
#ifdef HAVE_OPENSSL
    SHA256_Final(output, (SHA256_CTX *)ctx->impl);
    return 0;
#else
    if (!ctx || !ctx->impl || !output) {
        if (output) memset(output, 0, 32);
        return -1;
    }
    sha256_sw_final((sha256_sw_t *)ctx->impl, output);
    return 0;
#endif
}

static const mbedtls_md_info_t s_sha256_info = { MBEDTLS_MD_SHA256 };
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {
    return type == MBEDTLS_MD_SHA256 ? &s_sha256_info : nullptr;
}
int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output) {
#ifdef HAVE_OPENSSL
    unsigned int olen = 0;
    if (!HMAC(EVP_sha256(), key, (int)keylen, input, ilen, output, &olen)) return -1;
    return 0;
#else
    (void)key; (void)keylen; (void)input; (void)ilen; (void)output;
    return -1;
#endif
}

/* --- PSA / X25519 --- */
#define PSA_MAX_KEYS 8
typedef struct {
    int used;
    uint8_t priv[32];
} psa_slot_t;
static psa_slot_t s_keys[PSA_MAX_KEYS];
static int s_psa;

psa_status_t psa_crypto_init(void) { s_psa = 1; return PSA_SUCCESS; }
void psa_set_key_type(psa_key_attributes_t *a, uint16_t t) { if (a) a->type = t; }
void psa_set_key_bits(psa_key_attributes_t *a, uint16_t b) { if (a) a->bits = b; }
void psa_set_key_algorithm(psa_key_attributes_t *a, uint32_t alg) { if (a) a->alg = alg; }
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t u) { if (a) a->usage = u; }

static int alloc_slot(void) {
    for (int i = 1; i < PSA_MAX_KEYS; ++i) {
        if (!s_keys[i].used) {
            s_keys[i].used = 1;
            return i;
        }
    }
    return 0;
}

#ifdef HAVE_OPENSSL
static int x25519_pub(const uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY *p = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    if (!p) return -1;
    size_t olen = 32;
    int ok = EVP_PKEY_get_raw_public_key(p, pub, &olen) == 1 && olen == 32;
    EVP_PKEY_free(p);
    return ok ? 0 : -1;
}
static int x25519_gen(uint8_t priv[32]) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY *p = nullptr;
    int ok = ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &p) == 1;
    size_t olen = 32;
    if (ok) ok = EVP_PKEY_get_raw_private_key(p, priv, &olen) == 1;
    EVP_PKEY_free(p);
    EVP_PKEY_CTX_free(ctx);
    return ok ? 0 : -1;
}
static int x25519_shared(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    EVP_PKEY *me = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    EVP_PKEY *ot = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer, 32);
    EVP_PKEY_CTX *ctx = me ? EVP_PKEY_CTX_new(me, nullptr) : nullptr;
    size_t olen = 32;
    int ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, ot) == 1 &&
             EVP_PKEY_derive(ctx, out, &olen) == 1;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(me);
    EVP_PKEY_free(ot);
    return ok ? 0 : -1;
}
#endif

psa_status_t psa_generate_key(const psa_key_attributes_t *, psa_key_id_t *id) {
    int s = alloc_slot();
    if (!s) return -1;
#ifdef HAVE_OPENSSL
    if (x25519_gen(s_keys[s].priv) != 0) {
        s_keys[s].used = 0;
        return -1;
    }
#else
    s_keys[s].used = 0;
    return -1;
#endif
    *id = (psa_key_id_t)s;
    return PSA_SUCCESS;
}

psa_status_t psa_import_key(const psa_key_attributes_t *, const uint8_t *data, size_t len,
                            psa_key_id_t *id) {
    if (len != 32) return -1;
    int s = alloc_slot();
    if (!s) return -1;
    memcpy(s_keys[s].priv, data, 32);
    *id = (psa_key_id_t)s;
    return PSA_SUCCESS;
}

psa_status_t psa_export_key(psa_key_id_t id, uint8_t *data, size_t cap, size_t *olen) {
    if (id >= PSA_MAX_KEYS || !s_keys[id].used || cap < 32) return -1;
    memcpy(data, s_keys[id].priv, 32);
    *olen = 32;
    return PSA_SUCCESS;
}

psa_status_t psa_export_public_key(psa_key_id_t id, uint8_t *data, size_t cap, size_t *olen) {
    if (id >= PSA_MAX_KEYS || !s_keys[id].used || cap < 32) return -1;
#ifdef HAVE_OPENSSL
    if (x25519_pub(s_keys[id].priv, data) != 0) return -1;
    *olen = 32;
    return PSA_SUCCESS;
#else
    (void)data;
    (void)olen;
    return -1;
#endif
}

psa_status_t psa_raw_key_agreement(uint32_t, psa_key_id_t id, const uint8_t *peer,
                                   size_t peer_len, uint8_t *out, size_t cap, size_t *olen) {
    if (peer_len != 32 || cap < 32 || id >= PSA_MAX_KEYS || !s_keys[id].used) return -1;
#ifdef HAVE_OPENSSL
    if (x25519_shared(s_keys[id].priv, peer, out) != 0) return -1;
    *olen = 32;
    return PSA_SUCCESS;
#else
    (void)out;
    (void)olen;
    return -1;
#endif
}

psa_status_t psa_destroy_key(psa_key_id_t id) {
    if (id < PSA_MAX_KEYS) {
        memset(&s_keys[id], 0, sizeof(s_keys[id]));
    }
    return PSA_SUCCESS;
}
