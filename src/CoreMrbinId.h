#pragma once

#include "CoreConfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     api_base_url[CORE_MRBIN_API_URL_MAX];
    char     device_uid[CORE_MRBIN_DEVICE_UID_MAX];
    char     hardware_id[32];
    char     device_id[32];
    char     camera_key[16];
    char     camera_label[48];
    char     key_id[CORE_MRBIN_KEY_ID_MAX];
    uint8_t  content_key[CORE_MRBIN_AES_KEY_BYTES];
    uint8_t  x25519_priv[32];
    bool     uid_valid;
    bool     content_key_valid;
    bool     x25519_valid;
    bool     enroll_pending;
} core_mrbin_id_t;

bool core_mrbin_id_init(void);
bool core_mrbin_id_load(void);
bool core_mrbin_id_save_public(void);
const core_mrbin_id_t *core_mrbin_id_get(void);

void core_mrbin_id_set_defaults(uint32_t core_id);
bool core_mrbin_id_set_public_fields(const char *api_base, const char *hardware_id,
                                     const char *device_id, const char *camera_key,
                                     const char *camera_label);

// Persiste MAC C6 → device_uid (chiamare con WiFi STA già up).
bool core_mrbin_id_capture_sta_mac(void);
bool core_mrbin_id_has_uid(void);
bool core_mrbin_id_has_content_key(void);

bool core_mrbin_id_ensure_x25519(uint8_t pub_out[32]);
bool core_mrbin_id_x25519_shared(const uint8_t peer_pub[32], uint8_t shared_out[32]);

bool core_mrbin_id_set_issued(const char *key_id, const uint8_t content_key[32]);
bool core_mrbin_id_set_pending(void);
bool core_mrbin_id_clear_content_key(void);

void core_mrbin_id_normalize_api_base(char *url, size_t url_len);
bool core_mrbin_id_make_upload_id(char *out, size_t out_len, uint16_t seq);

#ifdef __cplusplus
}
#endif
