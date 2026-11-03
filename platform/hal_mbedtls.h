#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0
#define MBEDTLS_GCM_ENCRYPT 1
#define MBEDTLS_CIPHER_ID_AES 2
#define MBEDTLS_MD_SHA256 5

typedef struct {
    uint8_t key[32];
    int bits;
    uint8_t rk[176]; /* AES-128 expanded schedule (11 * 16) */
    int ready;
} mbedtls_aes_context;

typedef struct {
    uint8_t key[32];
    int bits;
} mbedtls_gcm_context;

typedef struct {
    uint8_t state[32];
    void *impl;
} mbedtls_sha256_context;

typedef struct mbedtls_md_info {
    int type;
} mbedtls_md_info_t;

void mbedtls_aes_init(mbedtls_aes_context *ctx);
void mbedtls_aes_free(mbedtls_aes_context *ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned bits);
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char in[16],
                          unsigned char out[16]);
int mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off,
                          unsigned char nonce_counter[16], unsigned char stream_block[16],
                          const unsigned char *input, unsigned char *output);

void mbedtls_gcm_init(mbedtls_gcm_context *ctx);
void mbedtls_gcm_free(mbedtls_gcm_context *ctx);
int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, int cipher, const unsigned char *key, unsigned bits);
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length,
                              const unsigned char *iv, size_t iv_len,
                              const unsigned char *add, size_t add_len,
                              const unsigned char *input, unsigned char *output,
                              size_t tag_len, unsigned char *tag);
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length,
                             const unsigned char *iv, size_t iv_len,
                             const unsigned char *add, size_t add_len,
                             const unsigned char *tag, size_t tag_len,
                             const unsigned char *input, unsigned char *output);

int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char output[32], int is224);
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]);

const mbedtls_md_info_t *mbedtls_md_info_from_type(int type);
int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output);

#ifdef __cplusplus
}
#endif
