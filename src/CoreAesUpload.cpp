#include "CoreAesUpload.h"
#include "CoreMrbinId.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_heap_caps.h"

static const char *TAG = "core_aes_up";

#define AAD_PREFIX "mrbin-aes-gcm-v1"
#define HKDF_INFO_CLIP "mrbin-aes-gcm-v1-clip"
#define HKDF_INFO_NONCE "mrbin-aes-gcm-v1-nonce"
#define HKDF_INFO_ENROLL "mrbin-enroll-v1"

static void put_be64(uint8_t out[8], uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static bool sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    return mbedtls_sha256(data, len, out, 0) == 0;
}

static bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                        uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, key_len, msg, msg_len, out) == 0;
}

static bool hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                        const char *info, uint8_t *okm, size_t okm_len) {
    uint8_t null_salt[32] = {0};
    const uint8_t *s = salt;
    size_t sl = salt_len;
    if (!s || sl == 0) {
        s = null_salt;
        sl = 32;
    }
    uint8_t prk[32];
    if (!hmac_sha256(s, sl, ikm, ikm_len, prk)) {
        return false;
    }
    uint8_t t[32];
    size_t tlen = 0;
    size_t done = 0;
    uint8_t c = 1;
    const uint8_t *info_b = (const uint8_t *)(info ? info : "");
    size_t info_len = info ? strlen(info) : 0;
    while (done < okm_len) {
        uint8_t msg[32 + 64 + 1];
        size_t mlen = 0;
        if (tlen) {
            memcpy(msg, t, 32);
            mlen = 32;
        }
        if (info_len) {
            memcpy(msg + mlen, info_b, info_len);
            mlen += info_len;
        }
        msg[mlen++] = c;
        if (!hmac_sha256(prk, 32, msg, mlen, t)) {
            memset(prk, 0, sizeof(prk));
            return false;
        }
        tlen = 32;
        size_t take = okm_len - done;
        if (take > 32) {
            take = 32;
        }
        memcpy(okm + done, t, take);
        done += take;
        c++;
    }
    memset(prk, 0, sizeof(prk));
    memset(t, 0, sizeof(t));
    return true;
}

static void hex_lower(const uint8_t *in, size_t n, char *out, size_t out_len) {
    static const char *h = "0123456789abcdef";
    if (out_len < n * 2 + 1) {
        if (out_len) {
            out[0] = 0;
        }
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = h[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[n * 2] = 0;
}

static int b64url_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static size_t b64url_decode(const char *in, uint8_t *out, size_t out_max) {
    if (!in || !out) {
        return 0;
    }
    size_t n = 0, val = 0, valb = 0;
    for (const char *p = in; *p && *p != '"'; ++p) {
        if (*p == '=' || *p == '\n' || *p == '\r') {
            continue;
        }
        int d = b64url_val(*p);
        if (d < 0) {
            continue;
        }
        val = (val << 6) + (unsigned)d;
        valb += 6;
        if (valb >= 8) {
            valb -= 8;
            if (n < out_max) {
                out[n++] = (uint8_t)((val >> valb) & 0xFF);
            }
        }
    }
    return n;
}

static void b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 5 < out_len; i += 3) {
        uint32_t n = ((uint32_t)in[i]) << 16;
        if (i + 1 < in_len) n |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < in_len) n |= in[i + 2];
        out[o++] = t[(n >> 18) & 63];
        out[o++] = t[(n >> 12) & 63];
        if (i + 1 < in_len) out[o++] = t[(n >> 6) & 63];
        if (i + 2 < in_len) out[o++] = t[n & 63];
    }
    if (o < out_len) {
        out[o] = 0;
    }
}

static bool derive_clip_keys(const uint8_t *device_key, const char *upload_id,
                             uint8_t clip_key[32], uint8_t nonce_key[32]) {
    uint8_t salt[32];
    if (!sha256_buf((const uint8_t *)upload_id, strlen(upload_id), salt)) {
        return false;
    }
    if (!hkdf_sha256(device_key, 32, salt, 32, HKDF_INFO_CLIP, clip_key, 32)) {
        return false;
    }
    return hkdf_sha256(clip_key, 32, salt, 32, HKDF_INFO_NONCE, nonce_key, 32);
}

static bool derive_chunk_nonce(const uint8_t nonce_key[32], const char *upload_id,
                               uint64_t chunk_index, uint8_t nonce[12]) {
    uint8_t idx[8];
    put_be64(idx, chunk_index);
    uint8_t msg[CORE_MRBIN_UPLOAD_ID_MAX + 16];
    size_t ulen = strlen(upload_id);
    if (ulen + 1 + 8 > sizeof(msg)) {
        return false;
    }
    memcpy(msg, upload_id, ulen);
    msg[ulen] = ':';
    memcpy(msg + ulen + 1, idx, 8);
    uint8_t mac[32];
    if (!hmac_sha256(nonce_key, 32, msg, ulen + 1 + 8, mac)) {
        return false;
    }
    memcpy(nonce, mac, 12);
    return true;
}

static size_t build_aad(uint8_t *out, size_t out_max, const char *device_uid, const char *key_id,
                        const char *upload_id, uint64_t chunk_index, uint64_t plain_off,
                        uint64_t plain_len) {
    size_t n = 0;
    const char *p = AAD_PREFIX;
    size_t pl = strlen(p) + 1;
    if (n + pl >= out_max) return 0;
    memcpy(out + n, p, pl);
    n += pl;
    size_t dlen = strlen(device_uid) + 1;
    size_t klen = strlen(key_id) + 1;
    size_t ulen = strlen(upload_id) + 1;
    if (n + dlen + klen + ulen + 24 >= out_max) return 0;
    memcpy(out + n, device_uid, dlen);
    n += dlen;
    memcpy(out + n, key_id, klen);
    n += klen;
    memcpy(out + n, upload_id, ulen);
    n += ulen;
    put_be64(out + n, chunk_index);
    n += 8;
    put_be64(out + n, plain_off);
    n += 8;
    put_be64(out + n, plain_len);
    n += 8;
    return n;
}

static bool gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                        size_t aad_len, const uint8_t *plain, size_t plain_len,
                        uint8_t *cipher, uint8_t tag[16]) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain_len, nonce, 12,
                                        aad, aad_len, plain, cipher, 16, tag);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0;
}

static bool gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                        size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                        const uint8_t tag[16], uint8_t *plain) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, cipher_len, nonce, 12, aad, aad_len, tag, 16,
                                       cipher, plain);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0;
}

static bool atomic_replace(const char *tmp, const char *final_path) {
    remove(final_path);
    if (rename(tmp, final_path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static bool write_aes_meta(const char *meta_path, const core_mrbin_aes_meta_t *m) {
    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s.tmp", meta_path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return false;
    }
    fprintf(fp, "protocol_version=1\n");
    fprintf(fp, "upload_id=%s\n", m->upload_id);
    fprintf(fp, "device_uid=%s\n", m->device_uid);
    fprintf(fp, "key_id=%s\n", m->key_id);
    fprintf(fp, "video_name=%s\n", m->video_name);
    fprintf(fp, "plain_total=%llu\n", (unsigned long long)m->plain_total);
    fprintf(fp, "chunk_count=%lu\n", (unsigned long)m->chunk_count);
    fprintf(fp, "plain_sha256=%s\n", m->plain_sha256);
    fprintf(fp, "manifest_sha256=%s\n", m->manifest_sha256);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    return atomic_replace(tmp, meta_path);
}

bool core_aes_meta_load(const char *meta_path, core_mrbin_aes_meta_t *out) {
    if (!meta_path || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    FILE *fp = fopen(meta_path, "r");
    if (!fp) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line;
        const char *v = eq + 1;
        if (strcmp(k, "upload_id") == 0) snprintf(out->upload_id, sizeof(out->upload_id), "%s", v);
        else if (strcmp(k, "device_uid") == 0) snprintf(out->device_uid, sizeof(out->device_uid), "%s", v);
        else if (strcmp(k, "key_id") == 0) snprintf(out->key_id, sizeof(out->key_id), "%s", v);
        else if (strcmp(k, "video_name") == 0) snprintf(out->video_name, sizeof(out->video_name), "%s", v);
        else if (strcmp(k, "plain_total") == 0) out->plain_total = strtoull(v, nullptr, 10);
        else if (strcmp(k, "chunk_count") == 0) out->chunk_count = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "plain_sha256") == 0) snprintf(out->plain_sha256, sizeof(out->plain_sha256), "%s", v);
        else if (strcmp(k, "manifest_sha256") == 0) {
            snprintf(out->manifest_sha256, sizeof(out->manifest_sha256), "%s", v);
        }
    }
    fclose(fp);
    return out->upload_id[0] && out->chunk_count > 0;
}

bool core_aes_encrypt_clip(const char *h264_path, const char *aes_path, const char *meta_path,
                           const char *upload_id, const char *video_name) {
    const core_mrbin_id_t *id = core_mrbin_id_get();
    if (!id || !id->content_key_valid || !id->uid_valid || !h264_path || !aes_path ||
        !meta_path || !upload_id) {
        return false;
    }
    FILE *in = fopen(h264_path, "rb");
    if (!in) {
        ESP_LOGE(TAG, "Apertura H264 fallita: %s", h264_path);
        return false;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return false;
    }
    long sz = ftell(in);
    rewind(in);
    if (sz <= 0) {
        fclose(in);
        return false;
    }

    uint8_t clip_key[32], nonce_key[32];
    if (!derive_clip_keys(id->content_key, upload_id, clip_key, nonce_key)) {
        fclose(in);
        return false;
    }

    uint8_t *plain = (uint8_t *)heap_caps_malloc(CORE_MRBIN_CHUNK_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *cipher = (uint8_t *)heap_caps_malloc(CORE_MRBIN_CHUNK_BYTES + CORE_MRBIN_GCM_TAG_BYTES,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!plain || !cipher) {
        ESP_LOGE(TAG, "PSRAM 4 MiB non disponibile per AES-GCM");
        free(plain);
        free(cipher);
        fclose(in);
        return false;
    }

    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s.tmp", aes_path);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        heap_caps_free(plain);
        heap_caps_free(cipher);
        fclose(in);
        return false;
    }

    mbedtls_sha256_context plain_sha, man_sha;
    mbedtls_sha256_init(&plain_sha);
    mbedtls_sha256_init(&man_sha);
    mbedtls_sha256_starts(&plain_sha, 0);
    mbedtls_sha256_starts(&man_sha, 0);

    uint64_t offset = 0;
    uint32_t chunk = 0;
    bool ok = true;
    while (offset < (uint64_t)sz && ok) {
        size_t want = CORE_MRBIN_CHUNK_BYTES;
        if ((uint64_t)sz - offset < want) {
            want = (size_t)((uint64_t)sz - offset);
        }
        size_t got = fread(plain, 1, want, in);
        if (got != want) {
            ok = false;
            break;
        }
        mbedtls_sha256_update(&plain_sha, plain, got);

        uint8_t nonce[12];
        uint8_t aad[256];
        if (!derive_chunk_nonce(nonce_key, upload_id, chunk, nonce)) {
            ok = false;
            break;
        }
        size_t aad_len = build_aad(aad, sizeof(aad), id->device_uid, id->key_id, upload_id,
                                   chunk, offset, (uint64_t)got);
        uint8_t tag[16];
        if (!aad_len || !gcm_encrypt(clip_key, nonce, aad, aad_len, plain, got, cipher, tag)) {
            ok = false;
            break;
        }
        memcpy(cipher + got, tag, 16);
        if (fwrite(cipher, 1, got + 16, out) != got + 16) {
            ok = false;
            break;
        }

        uint8_t csha[32];
        sha256_buf(cipher, got + 16, csha);
        uint8_t man_item[8 + 8 + 8 + 32];
        put_be64(man_item, chunk);
        put_be64(man_item + 8, offset);
        put_be64(man_item + 16, (uint64_t)got);
        memcpy(man_item + 24, csha, 32);
        mbedtls_sha256_update(&man_sha, man_item, sizeof(man_item));

        offset += got;
        chunk++;
    }

    fflush(out);
    fsync(fileno(out));
    fclose(out);
    fclose(in);

    uint8_t pdig[32], mdig[32];
    mbedtls_sha256_finish(&plain_sha, pdig);
    mbedtls_sha256_finish(&man_sha, mdig);
    mbedtls_sha256_free(&plain_sha);
    mbedtls_sha256_free(&man_sha);
    heap_caps_free(plain);
    heap_caps_free(cipher);
    memset(clip_key, 0, sizeof(clip_key));
    memset(nonce_key, 0, sizeof(nonce_key));

    if (!ok || !atomic_replace(tmp, aes_path)) {
        remove(tmp);
        remove(aes_path);
        ESP_LOGE(TAG, "Cifratura clip fallita");
        return false;
    }

    core_mrbin_aes_meta_t meta = {};
    snprintf(meta.upload_id, sizeof(meta.upload_id), "%s", upload_id);
    snprintf(meta.device_uid, sizeof(meta.device_uid), "%s", id->device_uid);
    snprintf(meta.key_id, sizeof(meta.key_id), "%s", id->key_id);
    snprintf(meta.video_name, sizeof(meta.video_name), "%s",
             video_name ? video_name : "clip.h264");
    meta.plain_total = (uint64_t)sz;
    meta.chunk_count = chunk;
    hex_lower(pdig, 32, meta.plain_sha256, sizeof(meta.plain_sha256));
    hex_lower(mdig, 32, meta.manifest_sha256, sizeof(meta.manifest_sha256));
    if (!write_aes_meta(meta_path, &meta)) {
        ESP_LOGE(TAG, "Scrittura .aes.meta fallita");
        return false;
    }
    ESP_LOGI(TAG, "Clip cifrata: %s (%lu chunk, %llu B)", aes_path,
             (unsigned long)chunk, (unsigned long long)sz);
    return true;
}

static bool enroll_unwrap(const uint8_t shared[32], const uint8_t *nonce, size_t nonce_len,
                          const uint8_t *ct, size_t ct_len, uint8_t key_out[32]) {
    if (nonce_len < 12 || ct_len < 32 + 16) {
        return false;
    }
    const uint8_t *tag = ct + (ct_len - 16);
    size_t clen = ct_len - 16;
    if (clen != 32) {
        return false;
    }
    if (gcm_decrypt(shared, nonce, nullptr, 0, ct, clen, tag, key_out)) {
        return true;
    }
    uint8_t salt[32];
    const core_mrbin_id_t *id = core_mrbin_id_get();
    if (id && sha256_buf((const uint8_t *)id->device_uid, strlen(id->device_uid), salt)) {
        uint8_t wrap[32];
        if (hkdf_sha256(shared, 32, salt, 32, HKDF_INFO_ENROLL, wrap, 32) &&
            gcm_decrypt(wrap, nonce, nullptr, 0, ct, clen, tag, key_out)) {
            memset(wrap, 0, sizeof(wrap));
            return true;
        }
        memset(wrap, 0, sizeof(wrap));
    }
    uint8_t wrap2[32];
    if (hkdf_sha256(shared, 32, nonce, 12, HKDF_INFO_ENROLL, wrap2, 32) &&
        gcm_decrypt(wrap2, nonce, nullptr, 0, ct, clen, tag, key_out)) {
        memset(wrap2, 0, sizeof(wrap2));
        return true;
    }
    memset(wrap2, 0, sizeof(wrap2));
    return false;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_body_acc_t;

static esp_err_t http_evt(esp_http_client_event_t *evt) {
    if (!evt || evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) {
        return ESP_OK;
    }
    http_body_acc_t *acc = (http_body_acc_t *)evt->user_data;
    if (!evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (acc->len + (size_t)evt->data_len + 1 > acc->cap) {
        return ESP_OK;
    }
    memcpy(acc->buf + acc->len, evt->data, (size_t)evt->data_len);
    acc->len += (size_t)evt->data_len;
    acc->buf[acc->len] = 0;
    return ESP_OK;
}

static void apply_common_headers(esp_http_client_handle_t cli, const core_mrbin_aes_meta_t *m) {
    char num[24];
    esp_http_client_set_header(cli, "X-MrBin-Upload-Id", m->upload_id);
    esp_http_client_set_header(cli, "X-MrBin-Video-Name", m->video_name);
    esp_http_client_set_header(cli, "X-MrBin-Device-Uid", m->device_uid);
    esp_http_client_set_header(cli, "X-MrBin-Key-Id", m->key_id);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)m->plain_total);
    esp_http_client_set_header(cli, "X-MrBin-Plain-Total-Size", num);
}

static int http_post_json(const char *url, const char *body, char *resp, size_t resp_len,
                          int timeout_ms) {
    if (resp && resp_len) {
        resp[0] = 0;
    }
    http_body_acc_t acc = { resp, resp_len, 0 };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return -1;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    size_t blen = body ? strlen(body) : 0;
    esp_err_t err = esp_http_client_open(cli, (int)blen);
    int status = -1;
    if (err == ESP_OK) {
        if (blen) {
            esp_http_client_write(cli, body, (int)blen);
        }
        (void)esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        int rlen = esp_http_client_get_content_length(cli);
        if (rlen > 0 && acc.len == 0 && resp && resp_len > 1) {
            int n = esp_http_client_read(cli, resp, (int)resp_len - 1);
            if (n > 0) {
                resp[n] = 0;
                acc.len = (size_t)n;
            }
        }
    } else {
        ESP_LOGW(TAG, "HTTP open %s: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    return status;
}

core_mrbin_enroll_result_t core_aes_enroll(volatile bool *cancel) {
    (void)cancel;
    const core_mrbin_id_t *id = core_mrbin_id_get();
    if (!id || !id->uid_valid) {
        return CORE_MRBIN_ENROLL_FAIL;
    }
    if (id->content_key_valid) {
        return CORE_MRBIN_ENROLL_ISSUED;
    }

    uint8_t pub[32];
    if (!core_mrbin_id_ensure_x25519(pub)) {
        return CORE_MRBIN_ENROLL_FAIL;
    }
    char pub_b64[64];
    b64url_encode(pub, 32, pub_b64, sizeof(pub_b64));

    char url[160];
    snprintf(url, sizeof(url), "%s/device-crypto/enroll", id->api_base_url);
    char body[320];
    snprintf(body, sizeof(body),
             "{\"protocol_version\":1,\"device_uid\":\"%s\",\"device_public_key\":\"%s\"}",
             id->device_uid, pub_b64);

    char resp[1024] = {0};
    int status = http_post_json(url, body, resp, sizeof(resp), 20000);
    if (status < 0) {
        return CORE_MRBIN_ENROLL_NO_NET;
    }
    if (status == 403 || status == 404) {
        ESP_LOGW(TAG, "Enroll HTTP %d — device_uid non autorizzato?", status);
        return CORE_MRBIN_ENROLL_FAIL;
    }
    if (status >= 500) {
        return CORE_MRBIN_ENROLL_NO_NET;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        if (strstr(resp, "pending")) {
            (void)core_mrbin_id_set_pending();
            return CORE_MRBIN_ENROLL_PENDING;
        }
        ESP_LOGW(TAG, "Enroll: JSON non valido (HTTP %d)", status);
        return CORE_MRBIN_ENROLL_FAIL;
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    const char *sts = cJSON_IsString(st) ? st->valuestring : "";
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!payload) {
        payload = root;
    }
    core_mrbin_enroll_result_t result = CORE_MRBIN_ENROLL_FAIL;
    if (strcmp(sts, "pending") == 0) {
        (void)core_mrbin_id_set_pending();
        result = CORE_MRBIN_ENROLL_PENDING;
    } else if (strcmp(sts, "issued") == 0 || cJSON_GetObjectItemCaseSensitive(payload, "ciphertext")) {
        const cJSON *kid = cJSON_GetObjectItemCaseSensitive(payload, "key_id");
        const cJSON *spub = cJSON_GetObjectItemCaseSensitive(payload, "server_public_key");
        const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(payload, "ciphertext");
        uint8_t peer[32], nb[24], cbuf[80], shared[32], content[32];
        size_t nlen = 0, clen = 0;
        if (cJSON_IsString(kid) && cJSON_IsString(spub) && cJSON_IsString(nonce) &&
            cJSON_IsString(ct) &&
            b64url_decode(spub->valuestring, peer, sizeof(peer)) == 32 &&
            (nlen = b64url_decode(nonce->valuestring, nb, sizeof(nb))) >= 12 &&
            (clen = b64url_decode(ct->valuestring, cbuf, sizeof(cbuf))) >= 48 &&
            core_mrbin_id_x25519_shared(peer, shared) &&
            enroll_unwrap(shared, nb, nlen, cbuf, clen, content) &&
            core_mrbin_id_set_issued(kid->valuestring, content)) {
            result = CORE_MRBIN_ENROLL_ISSUED;
        } else {
            ESP_LOGE(TAG, "Enroll issued: unwrap content key fallito");
        }
        memset(shared, 0, sizeof(shared));
        memset(content, 0, sizeof(content));
    }
    cJSON_Delete(root);
    return result;
}

static bool parse_integrity(cJSON *root, uint64_t *plain_size, char *psha, size_t psha_len,
                            char *msha, size_t msha_len) {
    cJSON *integ = cJSON_GetObjectItemCaseSensitive(root, "integrity");
    if (!integ) {
        cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (res) {
            integ = cJSON_GetObjectItemCaseSensitive(res, "integrity");
        }
    }
    if (!integ) {
        return false;
    }
    cJSON *sz = cJSON_GetObjectItemCaseSensitive(integ, "plain_size_bytes");
    cJSON *ps = cJSON_GetObjectItemCaseSensitive(integ, "plain_sha256");
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(integ, "manifest_sha256");
    if (!cJSON_IsNumber(sz) || !cJSON_IsString(ps) || !cJSON_IsString(ms)) {
        return false;
    }
    *plain_size = (uint64_t)sz->valuedouble;
    snprintf(psha, psha_len, "%s", ps->valuestring);
    snprintf(msha, msha_len, "%s", ms->valuestring);
    return true;
}

static bool integrity_matches(const core_mrbin_aes_meta_t *m, uint64_t sz, const char *psha,
                              const char *msha) {
    return sz == m->plain_total &&
           strcasecmp(psha, m->plain_sha256) == 0 &&
           strcasecmp(msha, m->manifest_sha256) == 0;
}

static int http_post_empty(const char *url, const core_mrbin_aes_meta_t *m, char *resp,
                           size_t resp_len) {
    if (resp && resp_len) resp[0] = 0;
    http_body_acc_t acc = { resp, resp_len, 0 };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    apply_common_headers(cli, m);
    int status = -1;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        (void)esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        if (acc.len == 0 && resp && resp_len > 1) {
            int n = esp_http_client_read(cli, resp, (int)resp_len - 1);
            if (n > 0) resp[n] = 0;
        }
    }
    esp_http_client_cleanup(cli);
    return status;
}

static int http_post_chunk(const char *url, const core_mrbin_aes_meta_t *m, uint32_t idx,
                           uint64_t poff, uint32_t plen, const char *csha,
                           const uint8_t *body, size_t blen) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 120000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 4096;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    apply_common_headers(cli, m);
    esp_http_client_set_header(cli, "Content-Type", "application/octet-stream");
    char num[24];
    snprintf(num, sizeof(num), "%lu", (unsigned long)idx);
    esp_http_client_set_header(cli, "X-MrBin-Chunk-Index", num);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)poff);
    esp_http_client_set_header(cli, "X-MrBin-Plain-Offset", num);
    snprintf(num, sizeof(num), "%lu", (unsigned long)plen);
    esp_http_client_set_header(cli, "X-MrBin-Plain-Length", num);
    esp_http_client_set_header(cli, "X-MrBin-Cipher-SHA256", csha);
    int status = -1;
    if (esp_http_client_open(cli, (int)blen) == ESP_OK) {
        int wr = esp_http_client_write(cli, (const char *)body, (int)blen);
        if (wr == (int)blen) {
            (void)esp_http_client_fetch_headers(cli);
            status = esp_http_client_get_status_code(cli);
        }
    }
    esp_http_client_cleanup(cli);
    return status;
}

static int http_post_complete(const char *url, const core_mrbin_aes_meta_t *m,
                              const char *json_body, const char *sidecar_name,
                              const char *hw, const char *did, const char *ckey,
                              const char *cname, char *resp, size_t resp_len) {
    if (resp && resp_len) resp[0] = 0;
    http_body_acc_t acc = { resp, resp_len, 0 };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    apply_common_headers(cli, m);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "X-MrBin-Sidecar-Name", sidecar_name);
    esp_http_client_set_header(cli, "X-MrBin-Hardware-Id", hw);
    esp_http_client_set_header(cli, "X-MrBin-Device-Id", did);
    esp_http_client_set_header(cli, "X-MrBin-Camera-Key", ckey);
    esp_http_client_set_header(cli, "X-MrBin-Camera-Name", cname);
    char num[24];
    snprintf(num, sizeof(num), "%lu", (unsigned long)m->chunk_count);
    esp_http_client_set_header(cli, "X-MrBin-Chunk-Count", num);
    esp_http_client_set_header(cli, "X-MrBin-Plain-SHA256", m->plain_sha256);
    esp_http_client_set_header(cli, "X-MrBin-Manifest-SHA256", m->manifest_sha256);
    int status = -1;
    size_t blen = json_body ? strlen(json_body) : 0;
    if (esp_http_client_open(cli, (int)blen) == ESP_OK) {
        if (blen) {
            esp_http_client_write(cli, json_body, (int)blen);
        }
        (void)esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        if (acc.len == 0 && resp && resp_len > 1) {
            int n = esp_http_client_read(cli, resp, (int)resp_len - 1);
            if (n > 0) resp[n] = 0;
        }
    }
    esp_http_client_cleanup(cli);
    return status;
}

static core_aes_upload_result_t map_http_err(int status) {
    if (status == 409) return CORE_AES_UP_HTTP409;
    if (status == 403) return CORE_AES_UP_HTTP403;
    if (status == 507) return CORE_AES_UP_HTTP507;
    if (status >= 500 || status < 0) return CORE_AES_UP_RETRY;
    return CORE_AES_UP_FAIL;
}

core_aes_upload_result_t core_aes_upload_clip(const char *aes_path, const char *json_path,
                                              const core_mrbin_aes_meta_t *meta,
                                              volatile bool *cancel) {
    const core_mrbin_id_t *id = core_mrbin_id_get();
    if (!id || !meta || !aes_path || !json_path) {
        return CORE_AES_UP_FAIL;
    }

    char url_st[180], url_ch[180], url_co[180];
    snprintf(url_st, sizeof(url_st), "%s/upload-aes/v1/status", id->api_base_url);
    snprintf(url_ch, sizeof(url_ch), "%s/upload-aes/v1/chunk", id->api_base_url);
    snprintf(url_co, sizeof(url_co), "%s/upload-aes/v1/complete", id->api_base_url);

    char resp[1536] = {0};
    int status = http_post_empty(url_st, meta, resp, sizeof(resp));
    if (status < 0 || status >= 500) {
        return CORE_AES_UP_RETRY;
    }
    if (status == 403) return CORE_AES_UP_HTTP403;
    if (status == 409) return CORE_AES_UP_HTTP409;
    if (status == 507) return CORE_AES_UP_HTTP507;
    if (status != 200 && status != 201) {
        return map_http_err(status);
    }

    cJSON *root = cJSON_Parse(resp);
    bool already = false;
    bool have[256];
    memset(have, 0, sizeof(have));
    if (root) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
        if (cJSON_IsString(st) && strcmp(st->valuestring, "complete") == 0) {
            uint64_t sz = 0;
            char psha[65] = {0}, msha[65] = {0};
            if (parse_integrity(root, &sz, psha, sizeof(psha), msha, sizeof(msha)) &&
                integrity_matches(meta, sz, psha, msha)) {
                already = true;
            } else {
                cJSON_Delete(root);
                return CORE_AES_UP_INTEGRITY;
            }
        }
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "received_chunks");
        if (cJSON_IsArray(arr)) {
            cJSON *it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                int idx = it->valueint;
                if (idx >= 0 && idx < (int)(sizeof(have) / sizeof(have[0]))) {
                    have[idx] = true;
                }
            }
        }
        cJSON_Delete(root);
    }
    if (already) {
        return CORE_AES_UP_ALREADY;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(CORE_MRBIN_CHUNK_BYTES + 16,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return CORE_AES_UP_FAIL;
    }
    FILE *fp = fopen(aes_path, "rb");
    if (!fp) {
        heap_caps_free(buf);
        return CORE_AES_UP_FAIL;
    }

    core_aes_upload_result_t result = CORE_AES_UP_OK;
    for (uint32_t i = 0; i < meta->chunk_count; ++i) {
        if (cancel && *cancel) {
            result = CORE_AES_UP_CANCELLED;
            break;
        }
        if (i < sizeof(have) / sizeof(have[0]) && have[i]) {
            continue;
        }
        uint64_t poff = (uint64_t)i * CORE_MRBIN_CHUNK_BYTES;
        uint32_t plen = CORE_MRBIN_CHUNK_BYTES;
        if (poff + plen > meta->plain_total) {
            plen = (uint32_t)(meta->plain_total - poff);
        }
        uint64_t coff = poff + (uint64_t)i * 16ULL;
        uint32_t clen = plen + 16;
        if (fseek(fp, (long)coff, SEEK_SET) != 0 || fread(buf, 1, clen, fp) != clen) {
            result = CORE_AES_UP_FAIL;
            break;
        }
        uint8_t csha[32];
        sha256_buf(buf, clen, csha);
        char csha_hex[65];
        hex_lower(csha, 32, csha_hex, sizeof(csha_hex));
        int hs = http_post_chunk(url_ch, meta, i, poff, plen, csha_hex, buf, clen);
        if (hs == 200 || hs == 201) {
            continue;
        }
        result = map_http_err(hs);
        ESP_LOGW(TAG, "Chunk %lu HTTP %d", (unsigned long)i, hs);
        break;
    }
    fclose(fp);
    heap_caps_free(buf);
    if (result != CORE_AES_UP_OK) {
        return result;
    }

    FILE *jf = fopen(json_path, "rb");
    if (!jf) {
        return CORE_AES_UP_FAIL;
    }
    if (fseek(jf, 0, SEEK_END) != 0) {
        fclose(jf);
        return CORE_AES_UP_FAIL;
    }
    long jsz = ftell(jf);
    rewind(jf);
    if (jsz <= 0 || jsz > 8192) {
        fclose(jf);
        return CORE_AES_UP_FAIL;
    }
    char *jbody = (char *)malloc((size_t)jsz + 1);
    if (!jbody) {
        fclose(jf);
        return CORE_AES_UP_FAIL;
    }
    if (fread(jbody, 1, (size_t)jsz, jf) != (size_t)jsz) {
        free(jbody);
        fclose(jf);
        return CORE_AES_UP_FAIL;
    }
    jbody[jsz] = 0;
    fclose(jf);

    const char *slash = strrchr(json_path, '/');
    const char *sname = slash ? slash + 1 : json_path;
    char resp2[1024] = {0};
    int cs = http_post_complete(url_co, meta, jbody, sname, id->hardware_id, id->device_id,
                                id->camera_key, CORE_MRBIN_CAMERA_NAME_DEFAULT, resp2,
                                sizeof(resp2));
    free(jbody);
    if (cs == 403) return CORE_AES_UP_HTTP403;
    if (cs == 409) return CORE_AES_UP_HTTP409;
    if (cs == 507) return CORE_AES_UP_HTTP507;
    if (cs < 0 || cs >= 500) return CORE_AES_UP_RETRY;
    if (cs != 200 && cs != 201) return CORE_AES_UP_FAIL;

    cJSON *cr = cJSON_Parse(resp2);
    if (!cr) {
        return CORE_AES_UP_INTEGRITY;
    }
    uint64_t sz = 0;
    char psha[65] = {0}, msha[65] = {0};
    bool match = parse_integrity(cr, &sz, psha, sizeof(psha), msha, sizeof(msha)) &&
                 integrity_matches(meta, sz, psha, msha);
    cJSON_Delete(cr);
    if (!match) {
        ESP_LOGE(TAG, "complete: integrity non coincidente — file SD conservati");
        return CORE_AES_UP_INTEGRITY;
    }
    ESP_LOGI(TAG, "Upload verificato: %s", meta->upload_id);
    return CORE_AES_UP_OK;
}

static bool api_host_from_url(const char *url, char *host, size_t host_len) {
    if (!url || !host || host_len < 4) {
        return false;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= host_len) {
        return false;
    }
    memcpy(host, p, n);
    host[n] = 0;
    return true;
}

static bool ping_http_once(const char *url) {
    if (!url || url[0] == 0) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = (int)CORE_MRBIN_PING_TRY_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return false;
    }
    esp_err_t err = esp_http_client_open(cli, 0);
    int status = -1;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        (void)esp_http_client_close(cli);
    }
    esp_http_client_cleanup(cli);
    if (status > 0) {
        ESP_LOGI(TAG, "Ping server OK (HTTP %d) %s", status, url);
        return true;
    }
    return false;
}

bool core_mrbin_wait_server_online(uint32_t timeout_ms, volatile bool *cancel) {
    const core_mrbin_id_t *id = core_mrbin_id_get();
    const char *url = (id && id->api_base_url[0]) ? id->api_base_url : CORE_MRBIN_API_BASE_DEFAULT;
    char host[80] = {0};
    (void)api_host_from_url(url, host, sizeof(host));
    ESP_LOGI(TAG, "Ping server upload %s (host %s, timeout %lu s)",
             url, host[0] ? host : "?", (unsigned long)(timeout_ms / 1000U));

    uint32_t waited = 0;
    uint32_t tries = 0;
    while (waited <= timeout_ms) {
        if (cancel && *cancel) {
            return false;
        }
        tries++;
        if (ping_http_once(url)) {
            return true;
        }
        if ((tries % 5U) == 0) {
            ESP_LOGI(TAG, "Ping %s ancora assente (%lu s) — attesa router",
                     host[0] ? host : url, (unsigned long)(waited / 1000U));
        }
        vTaskDelay(pdMS_TO_TICKS(CORE_MRBIN_PING_PERIOD_MS));
        waited += CORE_MRBIN_PING_PERIOD_MS;
    }
    ESP_LOGW(TAG, "Ping server timeout dopo %lu s", (unsigned long)(timeout_ms / 1000U));
    return false;
}

bool core_mrbin_config_pull(void) {
    // Contratto v1: nessun endpoint di pull configurazione. Hook per il futuro.
    ESP_LOGI(TAG, "Nessuna configurazione da scaricare");
    return false;
}
