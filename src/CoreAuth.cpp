#include "CoreAuth.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"
#else
#include "mbedtls/sha256.h"
#endif
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#endif

#ifndef CORE_AUTH_SESSION_PATH
/* Fast rootfs is erofs; persist beside NVS on userdata. */
#define CORE_AUTH_SESSION_PATH CORE_NVS_DIR "/web_session"
#endif

// SHA-256("planetel:12345678") — credenziali non in chiaro nello sketch
const uint8_t CORE_AUTH_SHA256[32] = {
    0xee, 0x45, 0xf2, 0x0b, 0xa3, 0xbe, 0x86, 0xf0,
    0xc6, 0xf3, 0xd1, 0x1b, 0x2a, 0x5e, 0x1f, 0xdd,
    0xb0, 0x31, 0x17, 0xf0, 0xbd, 0x92, 0x32, 0x93,
    0x69, 0x3d, 0x0a, 0xda, 0xdf, 0x90, 0x1a, 0x92,
};

static const char *TAG = "core_auth";
static char s_session_token[65] = {0};

static bool session_token_valid_hex(const char *token) {
    if (!token) {
        return false;
    }
    for (int i = 0; i < 64; ++i) {
        char c = token[i];
        if (c == '\0') {
            return i == 64;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return token[64] == '\0';
}

static void auth_session_load(void) {
#if defined(_WIN32)
    (void)0;
#else
    FILE *fp = fopen(CORE_AUTH_SESSION_PATH, "r");
    if (!fp) {
        return;
    }
    char buf[72];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);
    size_t n = strcspn(buf, "\r\n");
    buf[n] = '\0';
    if (session_token_valid_hex(buf)) {
        strncpy(s_session_token, buf, sizeof(s_session_token) - 1);
        ESP_LOGI(TAG, "Sessione web ripristinata da disco");
    }
#endif
}

static void auth_session_save(void) {
#if defined(_WIN32)
    (void)0;
#else
    if (s_session_token[0] == '\0') {
        return;
    }
    mkdir(CORE_NVS_DIR, 0755);
    FILE *fp = fopen(CORE_AUTH_SESSION_PATH ".tmp", "w");
    if (!fp) {
        ESP_LOGW(TAG, "Salvataggio sessione fallito: %s", strerror(errno));
        return;
    }
    fprintf(fp, "%s\n", s_session_token);
    fflush(fp);
    if (ferror(fp) || fclose(fp) != 0) {
        ESP_LOGW(TAG, "Salvataggio sessione fallito (write)");
        remove(CORE_AUTH_SESSION_PATH ".tmp");
        return;
    }
    if (rename(CORE_AUTH_SESSION_PATH ".tmp", CORE_AUTH_SESSION_PATH) != 0) {
        ESP_LOGW(TAG, "Salvataggio sessione fallito (rename): %s", strerror(errno));
    }
#endif
}

void core_auth_init(void) {
    auth_session_load();
}

static void sha256_concat(const char *user, const char *pass, uint8_t out[32]) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s:%s", user ? user : "", pass ? pass : "");
    mbedtls_sha256((const unsigned char *)buf, strlen(buf), out, 0);
}

bool core_auth_check_credentials(const char *user, const char *pass) {
    uint8_t digest[32];
    sha256_concat(user, pass, digest);
    bool ok = (memcmp(digest, CORE_AUTH_SHA256, 32) == 0);
    if (!ok) ESP_LOGW(TAG, "Login fallito per user=%s", user ? user : "(null)");
    return ok;
}

bool core_auth_create_session_token(char *out_token, size_t out_len) {
    if (!out_token || out_len < 65) return false;
    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 32; ++i) {
        snprintf(out_token + i * 2, 3, "%02x", rnd[i]);
    }
    strncpy(s_session_token, out_token, sizeof(s_session_token) - 1);
    auth_session_save();
    return true;
}

bool core_auth_validate_session_token(const char *token) {
    if (!token || s_session_token[0] == 0) return false;
    return strcmp(token, s_session_token) == 0;
}

void core_auth_clear_session(void) {
    s_session_token[0] = '\0';
#if !defined(_WIN32)
    remove(CORE_AUTH_SESSION_PATH);
    remove(CORE_AUTH_SESSION_PATH ".tmp");
#endif
}
