#pragma once

#include "CoreConfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_MRBIN_ENROLL_ISSUED = 0,
    CORE_MRBIN_ENROLL_PENDING,
    CORE_MRBIN_ENROLL_FAIL,
    CORE_MRBIN_ENROLL_NO_NET,
} core_mrbin_enroll_result_t;

typedef enum {
    CORE_AES_UP_OK = 0,
    CORE_AES_UP_ALREADY,
    CORE_AES_UP_RETRY,
    CORE_AES_UP_HTTP409,
    CORE_AES_UP_HTTP403,
    CORE_AES_UP_HTTP507,
    CORE_AES_UP_INTEGRITY,
    CORE_AES_UP_CANCELLED,
    CORE_AES_UP_FAIL,
} core_aes_upload_result_t;

typedef struct {
    char     upload_id[CORE_MRBIN_UPLOAD_ID_MAX + 4];
    char     device_uid[CORE_MRBIN_DEVICE_UID_MAX];
    char     key_id[CORE_MRBIN_KEY_ID_MAX];
    char     video_name[80];
    uint64_t plain_total;
    uint32_t chunk_count;
    char     plain_sha256[65];
    char     manifest_sha256[65];
} core_mrbin_aes_meta_t;

core_mrbin_enroll_result_t core_aes_enroll(volatile bool *cancel);

bool core_aes_encrypt_clip(const char *h264_path, const char *aes_path, const char *meta_path,
                           const char *upload_id, const char *video_name);

bool core_aes_meta_load(const char *meta_path, core_mrbin_aes_meta_t *out);

core_aes_upload_result_t core_aes_upload_clip(const char *aes_path, const char *json_path,
                                              const core_mrbin_aes_meta_t *meta,
                                              volatile bool *cancel);

// Attende che l'host API (mrbin.planetel.dev) risponda: DNS + HTTP.
// Qualsiasi status HTTP indica "online" (anche 401/404).
bool core_mrbin_wait_server_online(uint32_t timeout_ms, volatile bool *cancel);

// Pull configurazione dal server. v1: niente da scaricare → false.
bool core_mrbin_config_pull(void);

#ifdef __cplusplus
}
#endif
