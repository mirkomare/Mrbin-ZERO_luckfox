#include "CoreCrypto.h"
#include "CoreConfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "core_crypto";

bool core_crypto_init_ctx(core_crypto_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16]) {
    if (!ctx || !key || !iv) return false;
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key, 16);
    memcpy(ctx->iv, iv, 16);
    mbedtls_aes_init(&ctx->aes);
    if (mbedtls_aes_setkey_enc(&ctx->aes, key, 128) != 0) {
        mbedtls_aes_free(&ctx->aes);
        return false;
    }
    ctx->aes_ready = true;
    ctx->block_offset = 0;
    ctx->initialized = true;
    return true;
}

void core_crypto_deinit_ctx(core_crypto_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->aes_ready) {
        mbedtls_aes_free(&ctx->aes);
        ctx->aes_ready = false;
    }
    ctx->initialized = false;
}

void core_crypto_reset_block_offset(core_crypto_ctx_t *ctx, uint64_t block_offset) {
    if (ctx) ctx->block_offset = block_offset;
}

// Stessa convenzione CTR di core_crypto_crypt_at (offset logico MP4 post-header MRBI).
bool core_crypto_crypt_buffer(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    if (!ctx || !ctx->initialized || !buf) return false;
    return core_crypto_crypt_at(ctx, buf, len, stream_offset);
}

static void ctr_crypt_legacy(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    if (!ctx->aes_ready) {
        return;
    }

    uint8_t stream_block[16];
    uint8_t nonce_counter[16];
    memcpy(nonce_counter, ctx->iv, 16);

    uint64_t block_idx = stream_offset / 16;
    for (int i = 15; i >= 0; --i) {
        nonce_counter[i] = (uint8_t)(block_idx & 0xFF);
        block_idx >>= 8;
    }
    size_t nc_off = stream_offset % 16;

    if (mbedtls_aes_crypt_ctr(&ctx->aes, len, &nc_off, nonce_counter, stream_block, buf, buf) != 0) {
        return;
    }
}

bool core_crypto_crypt_buffer_legacy(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    if (!ctx || !ctx->initialized || !buf) return false;
    ctr_crypt_legacy(ctx, buf, len, stream_offset);
    return true;
}

// Costruisce il counter del blocco `block_idx`: byte [8..15] = block_idx BE, [0..7] = 0.
static void build_counter(uint64_t block_idx, uint8_t counter[16]) {
    memset(counter, 0, 16);
    uint64_t b = block_idx;
    for (int i = 15; i >= 8; --i) {
        counter[i] = (uint8_t)(b & 0xFF);
        b >>= 8;
    }
}

// Keystream ECB del blocco (AES-CTR usa sempre la chiave di cifratura).
static bool ecb_keystream(core_crypto_ctx_t *ctx, uint64_t block_idx, uint8_t ks[16]) {
    uint8_t counter[16];
    build_counter(block_idx, counter);
    return mbedtls_aes_crypt_ecb(&ctx->aes, MBEDTLS_AES_ENCRYPT, counter, ks) == 0;
}

bool core_crypto_crypt_at(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t offset) {
    if (!ctx || !ctx->initialized || !ctx->aes_ready || !buf) return false;
    if (len == 0) return true;

    size_t pos = 0;

    // Blocco parziale iniziale (offset non allineato a 16).
    size_t misalign = (size_t)(offset % 16);
    if (misalign) {
        uint8_t ks[16];
        if (!ecb_keystream(ctx, offset / 16, ks)) {
            return false;
        }
        size_t take = 16 - misalign;
        if (take > len) take = len;
        for (size_t i = 0; i < take; ++i) {
            buf[pos + i] ^= ks[misalign + i];
        }
        pos += take;
    }

    // Blocco allineato: CTR/DMA in un colpo solo (offset+pos multiplo di 16).
    size_t remaining = len - pos;
    size_t bulk = remaining - (remaining % 16);
    if (bulk > 0) {
        uint8_t nonce_counter[16];
        build_counter((offset + pos) / 16, nonce_counter);
        uint8_t stream_block[16];
        size_t nc_off = 0;
        if (mbedtls_aes_crypt_ctr(&ctx->aes, bulk, &nc_off, nonce_counter, stream_block,
                                  buf + pos, buf + pos) != 0) {
            return false;
        }
        pos += bulk;
    }

    // Blocco parziale finale.
    if (pos < len) {
        uint8_t ks[16];
        if (!ecb_keystream(ctx, (offset + pos) / 16, ks)) {
            return false;
        }
        size_t rem = len - pos;
        for (size_t i = 0; i < rem; ++i) {
            buf[pos + i] ^= ks[i];
        }
    }
    return true;
}

bool core_crypto_write_encrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                       const uint8_t *plain, size_t len, uint64_t stream_offset) {
    if (!ctx || !fp || !plain || len == 0) return false;
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = (uint8_t *)malloc(len);
    }
    if (!tmp) {
        return false;
    }
    memcpy(tmp, plain, len);
    core_crypto_crypt_buffer(ctx, tmp, len, stream_offset);
    size_t written = fwrite(tmp, 1, len, fp);
    heap_caps_free(tmp);
    return written == len;
}

bool core_crypto_read_decrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                      uint8_t *out, size_t len, uint64_t stream_offset, size_t *out_len) {
    if (!ctx || !fp || !out) return false;
    size_t rd = fread(out, 1, len, fp);
    if (out_len) *out_len = rd;
    if (rd == 0) return false;
    return core_crypto_crypt_buffer(ctx, out, rd, stream_offset);
}
