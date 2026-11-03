#include "hal_nvs.h"
#include "CoreConfig.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#define NVS_MAX 256

typedef struct {
    char key[32];
    char *val;
    size_t len;
    int is_blob;
} nvs_item_t;

typedef struct {
    nvs_item_t items[NVS_MAX];
    int n;
    char path[256];
    int dirty;
} nvs_store_t;

static nvs_store_t s_store;
static int s_ready;

static void mkdir_p(const char *dir) {
    if (!dir || !dir[0]) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return;
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (tmp[0]) (void)MKDIR(tmp);
        tmp[i] = '/';
    }
    (void)MKDIR(tmp);
}

static void ensure_dir(void) {
    mkdir_p(CORE_NVS_DIR);
#ifdef _WIN32
    MKDIR("mrbin_nvs");
#endif
}

static const char *store_path(void) {
#ifdef _WIN32
    return "mrbin_nvs/nvs.bin";
#else
    return CORE_NVS_DIR "/nvs.bin";
#endif
}

static nvs_item_t *find_item(const char *key) {
    for (int i = 0; i < s_store.n; ++i) {
        if (strcmp(s_store.items[i].key, key) == 0) return &s_store.items[i];
    }
    return nullptr;
}

static nvs_item_t *upsert(const char *key) {
    nvs_item_t *it = find_item(key);
    if (it) return it;
    if (s_store.n >= NVS_MAX) return nullptr;
    it = &s_store.items[s_store.n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->key, sizeof(it->key), "%s", key);
    return it;
}

static void load_store(void) {
    FILE *fp = fopen(store_path(), "rb");
    if (!fp) return;
    uint32_t n = 0;
    if (fread(&n, 4, 1, fp) != 1 || n > NVS_MAX) {
        fclose(fp);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        nvs_item_t it = {};
        uint8_t klen = 0, blob = 0;
        uint32_t vlen = 0;
        if (fread(&klen, 1, 1, fp) != 1) break;
        if (klen >= sizeof(it.key)) break;
        if (fread(it.key, 1, klen, fp) != klen) break;
        it.key[klen] = 0;
        if (fread(&blob, 1, 1, fp) != 1) break;
        if (fread(&vlen, 4, 1, fp) != 1) break;
        it.is_blob = blob;
        it.len = vlen;
        if (vlen) {
            it.val = (char *)malloc(vlen + 1);
            if (!it.val) break;
            if (fread(it.val, 1, vlen, fp) != vlen) {
                free(it.val);
                break;
            }
            it.val[vlen] = 0;
        }
        if (s_store.n < NVS_MAX) s_store.items[s_store.n++] = it;
        else free(it.val);
    }
    fclose(fp);
}

static bool save_store(void) {
    ensure_dir();
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", store_path());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return false;
    uint32_t n = (uint32_t)s_store.n;
    bool ok = fwrite(&n, 4, 1, fp) == 1;
    for (int i = 0; ok && i < s_store.n; ++i) {
        nvs_item_t *it = &s_store.items[i];
        uint8_t klen = (uint8_t)strlen(it->key);
        uint8_t blob = (uint8_t)it->is_blob;
        uint32_t vlen = (uint32_t)it->len;
        ok = fwrite(&klen, 1, 1, fp) == 1 &&
             fwrite(it->key, 1, klen, fp) == klen &&
             fwrite(&blob, 1, 1, fp) == 1 &&
             fwrite(&vlen, 4, 1, fp) == 1 &&
             (vlen == 0 || fwrite(it->val, 1, vlen, fp) == vlen);
    }
    if (ok) ok = fflush(fp) == 0;
#ifndef _WIN32
    if (ok) ok = fsync(fileno(fp)) == 0;
#endif
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        return false;
    }
    if (rename(tmp, store_path()) != 0) {
        remove(tmp);
        return false;
    }
#ifndef _WIN32
    int dfd = open(CORE_NVS_DIR, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
#endif
    return true;
}

esp_err_t nvs_flash_init(void) {
    if (s_ready) return ESP_OK;
    memset(&s_store, 0, sizeof(s_store));
    ensure_dir();
    load_store();
    s_ready = 1;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    for (int i = 0; i < s_store.n; ++i) free(s_store.items[i].val);
    memset(&s_store, 0, sizeof(s_store));
    remove(store_path());
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out) {
    (void)ns;
    (void)mode;
    if (!s_ready && nvs_flash_init() != ESP_OK) return ESP_FAIL;
    if (out) *out = &s_store;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return save_store() ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    nvs_item_t *it = find_item(key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    free(it->val);
    *it = s_store.items[--s_store.n];
    return ESP_OK;
}

static esp_err_t set_raw(const char *key, const void *data, size_t len, int blob) {
    nvs_item_t *it = upsert(key);
    if (!it) return ESP_ERR_NO_MEM;
    free(it->val);
    it->val = nullptr;
    it->len = len;
    it->is_blob = blob;
    if (len) {
        it->val = (char *)malloc(len + 1);
        if (!it->val) return ESP_ERR_NO_MEM;
        memcpy(it->val, data, len);
        it->val[len] = 0;
    }
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t, const char *k, uint8_t v) { return set_raw(k, &v, 1, 1); }
esp_err_t nvs_set_u16(nvs_handle_t, const char *k, uint16_t v) { return set_raw(k, &v, 2, 1); }
esp_err_t nvs_set_u32(nvs_handle_t, const char *k, uint32_t v) { return set_raw(k, &v, 4, 1); }
esp_err_t nvs_set_str(nvs_handle_t, const char *k, const char *v) {
    return set_raw(k, v, v ? strlen(v) : 0, 0);
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *k, const void *d, size_t n) {
    return set_raw(k, d, n, 1);
}

esp_err_t nvs_get_u8(nvs_handle_t, const char *k, uint8_t *v) {
    nvs_item_t *it = find_item(k);
    if (!it || it->len < 1) return ESP_ERR_NVS_NOT_FOUND;
    if (v) *v = *(uint8_t *)it->val;
    return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t, const char *k, uint16_t *v) {
    nvs_item_t *it = find_item(k);
    if (!it || it->len < 2) return ESP_ERR_NVS_NOT_FOUND;
    if (v) memcpy(v, it->val, 2);
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t, const char *k, uint32_t *v) {
    nvs_item_t *it = find_item(k);
    if (!it || it->len < 4) return ESP_ERR_NVS_NOT_FOUND;
    if (v) memcpy(v, it->val, 4);
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t, const char *k, char *out, size_t *len) {
    nvs_item_t *it = find_item(k);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = it->len + 1;
    if (!out) {
        if (len) *len = need;
        return ESP_OK;
    }
    if (!len || *len < need) {
        if (len) *len = need;
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out, it->val ? it->val : "", it->len);
    out[it->len] = 0;
    *len = need;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char *k, void *out, size_t *len) {
    nvs_item_t *it = find_item(k);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) {
        if (len) *len = it->len;
        return ESP_OK;
    }
    if (!len || *len < it->len) {
        if (len) *len = it->len;
        return ESP_ERR_INVALID_ARG;
    }
    if (it->len) memcpy(out, it->val, it->len);
    *len = it->len;
    return ESP_OK;
}
