#include "CoreMrbinId.h"
#include "CoreConfig.h"
#include "CoreTime.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "core_mrbin_id";

static core_mrbin_id_t s_id;
static nvs_handle_t s_nvs = 0;
static bool s_ready = false;
static bool s_psa_ok = false;

static bool psa_ensure(void) {
    if (s_psa_ok) {
        return true;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init fallito");
        return false;
    }
    s_psa_ok = true;
    return true;
}

static void strip_trailing_slash(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') {
        s[--n] = '\0';
    }
}

void core_mrbin_id_normalize_api_base(char *url, size_t url_len) {
    if (!url || url_len == 0) {
        return;
    }
    strip_trailing_slash(url);
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(url, url_len, "%s", CORE_MRBIN_API_BASE_DEFAULT);
    }
}

static void apply_defaults_unlocked(uint32_t core_id) {
    if (s_id.api_base_url[0] == 0) {
        snprintf(s_id.api_base_url, sizeof(s_id.api_base_url), "%s", CORE_MRBIN_API_BASE_DEFAULT);
    }
    core_mrbin_id_normalize_api_base(s_id.api_base_url, sizeof(s_id.api_base_url));
    if (s_id.hardware_id[0] == 0) {
        snprintf(s_id.hardware_id, sizeof(s_id.hardware_id), "%s", CORE_MRBIN_HARDWARE_ID_DEFAULT);
    }
    if (s_id.device_id[0] == 0 && core_id >= CORE_ID_MIN && core_id <= CORE_ID_MAX) {
        snprintf(s_id.device_id, sizeof(s_id.device_id), "mrbin-%05lu", (unsigned long)core_id);
    } else if (s_id.device_id[0] == 0) {
        snprintf(s_id.device_id, sizeof(s_id.device_id), "mrbin-core");
    }
    if (s_id.camera_key[0] == 0) {
        snprintf(s_id.camera_key, sizeof(s_id.camera_key), "%s", CORE_MRBIN_CAMERA_KEY_DEFAULT);
    }
    if (s_id.camera_label[0] == 0) {
        snprintf(s_id.camera_label, sizeof(s_id.camera_label), "%s", CORE_MRBIN_CAMERA_LABEL_DEFAULT);
    }
}

static bool nvs_get_str_opt(const char *key, char *out, size_t out_len) {
    size_t len = out_len;
    return nvs_get_str(s_nvs, key, out, &len) == ESP_OK && out[0] != 0;
}

bool core_mrbin_id_init(void) {
    if (s_ready) {
        return true;
    }
    esp_err_t err = nvs_open(CORE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return core_mrbin_id_load();
}

bool core_mrbin_id_load(void) {
    if (!s_ready && !core_mrbin_id_init()) {
        return false;
    }
    memset(&s_id, 0, sizeof(s_id));
    nvs_get_str_opt("mb_api", s_id.api_base_url, sizeof(s_id.api_base_url));
    nvs_get_str_opt("mb_uid", s_id.device_uid, sizeof(s_id.device_uid));
    nvs_get_str_opt("mb_hw", s_id.hardware_id, sizeof(s_id.hardware_id));
    nvs_get_str_opt("mb_did", s_id.device_id, sizeof(s_id.device_id));
    nvs_get_str_opt("mb_ckey", s_id.camera_key, sizeof(s_id.camera_key));
    nvs_get_str_opt("mb_clab", s_id.camera_label, sizeof(s_id.camera_label));
    nvs_get_str_opt("mb_kid", s_id.key_id, sizeof(s_id.key_id));

    size_t n = sizeof(s_id.content_key);
    if (nvs_get_blob(s_nvs, "mb_dkey", s_id.content_key, &n) == ESP_OK &&
        n == sizeof(s_id.content_key) && s_id.key_id[0] != 0) {
        s_id.content_key_valid = true;
    }
    n = sizeof(s_id.x25519_priv);
    if (nvs_get_blob(s_nvs, "mb_xpriv", s_id.x25519_priv, &n) == ESP_OK &&
        n == sizeof(s_id.x25519_priv)) {
        s_id.x25519_valid = true;
    }
    uint8_t pend = 0;
    if (nvs_get_u8(s_nvs, "mb_pend", &pend) == ESP_OK) {
        s_id.enroll_pending = (pend != 0);
    }
    // Luckfox: luckfox-<eth MAC>. Accetta anche uid ESP32-P4 già persistiti (non rompere header).
    s_id.uid_valid = ((strncmp(s_id.device_uid, CORE_MRBIN_DEVICE_UID_PREFIX,
                               sizeof(CORE_MRBIN_DEVICE_UID_PREFIX) - 1) == 0 ||
                       strncmp(s_id.device_uid, "esp32p4-", 8) == 0) &&
                      strlen(s_id.device_uid) >= 20);
    apply_defaults_unlocked(0);
    ESP_LOGI(TAG, "MrBin id: uid=%s enroll=%s",
             s_id.uid_valid ? s_id.device_uid : "(assente)",
             s_id.content_key_valid ? "issued" : (s_id.enroll_pending ? "pending" : "no"));
    return true;
}

static bool commit_nvs(void) {
    return nvs_commit(s_nvs) == ESP_OK;
}

bool core_mrbin_id_save_public(void) {
    if (!s_ready) {
        return false;
    }
    apply_defaults_unlocked(0);
    if (nvs_set_str(s_nvs, "mb_api", s_id.api_base_url) != ESP_OK) return false;
    if (s_id.device_uid[0] && nvs_set_str(s_nvs, "mb_uid", s_id.device_uid) != ESP_OK) return false;
    if (nvs_set_str(s_nvs, "mb_hw", s_id.hardware_id) != ESP_OK) return false;
    if (nvs_set_str(s_nvs, "mb_did", s_id.device_id) != ESP_OK) return false;
    if (nvs_set_str(s_nvs, "mb_ckey", s_id.camera_key) != ESP_OK) return false;
    if (nvs_set_str(s_nvs, "mb_clab", s_id.camera_label) != ESP_OK) return false;
    if (nvs_set_u8(s_nvs, "mb_pend", s_id.enroll_pending ? 1 : 0) != ESP_OK) return false;
    return commit_nvs();
}

const core_mrbin_id_t *core_mrbin_id_get(void) {
    if (!s_ready) {
        core_mrbin_id_init();
    }
    return &s_id;
}

void core_mrbin_id_set_defaults(uint32_t core_id) {
    if (!s_ready) {
        core_mrbin_id_init();
    }
    apply_defaults_unlocked(core_id);
    (void)core_mrbin_id_save_public();
}

bool core_mrbin_id_set_public_fields(const char *api_base, const char *hardware_id,
                                     const char *device_id, const char *camera_key,
                                     const char *camera_label) {
    if (!s_ready) {
        return false;
    }
    if (api_base && api_base[0]) {
        snprintf(s_id.api_base_url, sizeof(s_id.api_base_url), "%s", api_base);
        core_mrbin_id_normalize_api_base(s_id.api_base_url, sizeof(s_id.api_base_url));
    }
    if (hardware_id && hardware_id[0]) {
        snprintf(s_id.hardware_id, sizeof(s_id.hardware_id), "%s", hardware_id);
    }
    if (device_id && device_id[0]) {
        snprintf(s_id.device_id, sizeof(s_id.device_id), "%s", device_id);
    }
    if (camera_key && camera_key[0]) {
        snprintf(s_id.camera_key, sizeof(s_id.camera_key), "%s", camera_key);
    }
    if (camera_label && camera_label[0]) {
        snprintf(s_id.camera_label, sizeof(s_id.camera_label), "%s", camera_label);
    }
    return core_mrbin_id_save_public();
}

bool core_mrbin_id_capture_sta_mac(void) {
    if (!s_ready && !core_mrbin_id_init()) {
        return false;
    }
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
            ESP_LOGW(TAG, "MAC STA non disponibile");
            return s_id.uid_valid;
        }
    }
    char uid[CORE_MRBIN_DEVICE_UID_MAX];
    snprintf(uid, sizeof(uid), CORE_MRBIN_DEVICE_UID_PREFIX "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (s_id.uid_valid && strcmp(s_id.device_uid, uid) == 0) {
        return true;
    }
    snprintf(s_id.device_uid, sizeof(s_id.device_uid), "%s", uid);
    s_id.uid_valid = true;
    ESP_LOGI(TAG, "device_uid persistito: %s", s_id.device_uid);
    return core_mrbin_id_save_public();
}

bool core_mrbin_id_has_uid(void) {
    return s_id.uid_valid && s_id.device_uid[0] != 0;
}

bool core_mrbin_id_has_content_key(void) {
    return s_id.content_key_valid && s_id.key_id[0] != 0;
}

static bool x25519_pub_from_priv(const uint8_t priv[32], uint8_t pub[32]) {
    if (!psa_ensure()) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_key_id_t kid = 0;
    if (psa_import_key(&attr, priv, 32, &kid) != PSA_SUCCESS) {
        return false;
    }
    size_t olen = 0;
    psa_status_t st = psa_export_public_key(kid, pub, 32, &olen);
    psa_destroy_key(kid);
    return st == PSA_SUCCESS && olen == 32;
}

bool core_mrbin_id_ensure_x25519(uint8_t pub_out[32]) {
    if (!s_ready && !core_mrbin_id_init()) {
        return false;
    }
    if (!psa_ensure()) {
        return false;
    }
    if (!s_id.x25519_valid) {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_bits(&attr, 255);
        psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
        psa_key_id_t kid = 0;
        if (psa_generate_key(&attr, &kid) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Generazione X25519 fallita");
            return false;
        }
        size_t olen = 0;
        psa_status_t st = psa_export_key(kid, s_id.x25519_priv, 32, &olen);
        psa_destroy_key(kid);
        if (st != PSA_SUCCESS || olen != 32) {
            memset(s_id.x25519_priv, 0, sizeof(s_id.x25519_priv));
            return false;
        }
        if (nvs_set_blob(s_nvs, "mb_xpriv", s_id.x25519_priv, 32) != ESP_OK || !commit_nvs()) {
            memset(s_id.x25519_priv, 0, sizeof(s_id.x25519_priv));
            return false;
        }
        s_id.x25519_valid = true;
        ESP_LOGI(TAG, "Coppia X25519 creata (privata in NVS)");
    }
    if (pub_out && !x25519_pub_from_priv(s_id.x25519_priv, pub_out)) {
        return false;
    }
    return true;
}

bool core_mrbin_id_x25519_shared(const uint8_t peer_pub[32], uint8_t shared_out[32]) {
    if (!peer_pub || !shared_out || !s_id.x25519_valid || !psa_ensure()) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_key_id_t kid = 0;
    if (psa_import_key(&attr, s_id.x25519_priv, 32, &kid) != PSA_SUCCESS) {
        return false;
    }
    size_t olen = 0;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, kid, peer_pub, 32,
                                            shared_out, 32, &olen);
    psa_destroy_key(kid);
    return st == PSA_SUCCESS && olen == 32;
}

bool core_mrbin_id_set_issued(const char *key_id, const uint8_t content_key[32]) {
    if (!s_ready || !key_id || !key_id[0] || !content_key) {
        return false;
    }
    snprintf(s_id.key_id, sizeof(s_id.key_id), "%s", key_id);
    memcpy(s_id.content_key, content_key, sizeof(s_id.content_key));
    s_id.content_key_valid = true;
    s_id.enroll_pending = false;
    if (nvs_set_str(s_nvs, "mb_kid", s_id.key_id) != ESP_OK) return false;
    if (nvs_set_blob(s_nvs, "mb_dkey", s_id.content_key, sizeof(s_id.content_key)) != ESP_OK) {
        return false;
    }
    if (nvs_set_u8(s_nvs, "mb_pend", 0) != ESP_OK) return false;
    ESP_LOGI(TAG, "Enrollment issued key_id=%s", s_id.key_id);
    return commit_nvs();
}

bool core_mrbin_id_set_pending(void) {
    if (!s_ready) {
        return false;
    }
    s_id.enroll_pending = true;
    return nvs_set_u8(s_nvs, "mb_pend", 1) == ESP_OK && commit_nvs();
}

bool core_mrbin_id_clear_content_key(void) {
    if (!s_ready) {
        return false;
    }
    memset(s_id.content_key, 0, sizeof(s_id.content_key));
    s_id.content_key_valid = false;
    s_id.key_id[0] = 0;
    nvs_erase_key(s_nvs, "mb_dkey");
    nvs_erase_key(s_nvs, "mb_kid");
    return commit_nvs();
}

bool core_mrbin_id_make_upload_id(char *out, size_t out_len, uint16_t seq) {
    if (!out || out_len < 24 || !core_mrbin_id_has_uid()) {
        return false;
    }
    const char *ck = s_id.camera_key[0] ? s_id.camera_key : CORE_MRBIN_CAMERA_KEY_DEFAULT;
    uint32_t rnd = esp_random() & 0xFFFF;
    char stamp[24];
    if (core_time_is_valid()) {
        time_t now = time(nullptr);
        struct tm utc;
        gmtime_r(&now, &utc);
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d",
                 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        snprintf(stamp, sizeof(stamp), "unsynced_%04u", (unsigned)seq);
    }
    int n = snprintf(out, out_len, "%s_%s_%s_%04u_%04x",
                     s_id.device_uid, ck, stamp, (unsigned)seq, (unsigned)rnd);
    if (n < 8 || (size_t)n >= out_len || (size_t)n > CORE_MRBIN_UPLOAD_ID_MAX) {
        return false;
    }
    return true;
}
