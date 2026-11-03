#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "mbedtls/aes.h"

typedef struct {
    uint8_t iv[16];
    uint8_t key[16];
    uint64_t block_offset;
    bool initialized;
    bool aes_ready;
    mbedtls_aes_context aes;
} core_crypto_ctx_t;

bool core_crypto_init_ctx(core_crypto_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16]);
void core_crypto_deinit_ctx(core_crypto_ctx_t *ctx);
void core_crypto_reset_block_offset(core_crypto_ctx_t *ctx, uint64_t block_offset);

// Cifra/decifra in-place (AES-128-CTR, stessa convenzione offset di core_crypto_crypt_at).
bool core_crypto_crypt_buffer(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset);

// Decifra con la convenzione legacy (pre dual-job / encrypt_mp4_file): IV poi block_idx su 16 byte.
bool core_crypto_crypt_buffer_legacy(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset);

// Cifra/decifra in-place a offset ARBITRARIO (anche non multiplo di 16): gestisce i
// blocchi parziali iniziale/finale via ECB e il blocco allineato via CTR/DMA.
bool core_crypto_crypt_at(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t offset);

// Scrive header MRBI + cifra chunk
bool core_crypto_write_encrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                       const uint8_t *plain, size_t len, uint64_t stream_offset);

// Legge e decifra chunk da file con header MRBI
bool core_crypto_read_decrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                      uint8_t *out, size_t len, uint64_t stream_offset, size_t *out_len);

#ifdef __cplusplus
}
#endif
