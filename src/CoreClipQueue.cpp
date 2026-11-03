#include "CoreClipQueue.h"
#include "CoreAesUpload.h"
#include "CoreConfig.h"
#include "CoreMrbinId.h"
#include "CoreSD.h"

#include "esp_log.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "core_clipq";

typedef struct {
    char base[192];
    char mp4[192];
    char h264[192];
    char json[192];
    char aes[208];
    char aes_meta[216];
    char state[208];
    char info[208];
} clip_paths_t;

static const char *state_name(core_clip_state_t st) {
    switch (st) {
        case CORE_CLIP_RECORDED_PLAIN: return "RECORDED_PLAIN";
        case CORE_CLIP_SIDECAR_WRITTEN: return "SIDECAR_WRITTEN";
        case CORE_CLIP_ENCRYPTED_LOCAL: return "ENCRYPTED_LOCAL";
        case CORE_CLIP_PENDING_UPLOAD: return "PENDING_UPLOAD";
        case CORE_CLIP_UPLOADING: return "UPLOADING";
        case CORE_CLIP_UPLOADED_VERIFIED: return "UPLOADED_VERIFIED";
        case CORE_CLIP_DELETE_PENDING: return "DELETE_PENDING";
        case CORE_CLIP_DELETED: return "DELETED";
        case CORE_CLIP_ERROR: return "ERROR";
        default: return "NONE";
    }
}

const char *core_clip_state_label(core_clip_state_t st) {
    return state_name(st);
}

static core_clip_state_t state_from_name(const char *s) {
    if (!s) return CORE_CLIP_NONE;
    if (strcmp(s, "RECORDED_PLAIN") == 0) return CORE_CLIP_RECORDED_PLAIN;
    if (strcmp(s, "SIDECAR_WRITTEN") == 0) return CORE_CLIP_SIDECAR_WRITTEN;
    if (strcmp(s, "ENCRYPTED_LOCAL") == 0) return CORE_CLIP_ENCRYPTED_LOCAL;
    if (strcmp(s, "PENDING_UPLOAD") == 0) return CORE_CLIP_PENDING_UPLOAD;
    if (strcmp(s, "UPLOADING") == 0) return CORE_CLIP_UPLOADING;
    if (strcmp(s, "UPLOADED_VERIFIED") == 0) return CORE_CLIP_UPLOADED_VERIFIED;
    if (strcmp(s, "DELETE_PENDING") == 0) return CORE_CLIP_DELETE_PENDING;
    if (strcmp(s, "DELETED") == 0) return CORE_CLIP_DELETED;
    if (strcmp(s, "ERROR") == 0) return CORE_CLIP_ERROR;
    return CORE_CLIP_NONE;
}

static bool paths_from_mp4(const char *mp4, clip_paths_t *p) {
    if (!mp4 || !p) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    snprintf(p->mp4, sizeof(p->mp4), "%s", mp4);
    const char *dot = strrchr(mp4, '.');
    size_t blen = dot && strcmp(dot, ".mp4") == 0 ? (size_t)(dot - mp4) : strlen(mp4);
    if (blen >= sizeof(p->base)) {
        return false;
    }
    memcpy(p->base, mp4, blen);
    p->base[blen] = 0;
    snprintf(p->h264, sizeof(p->h264), "%s.h264", p->base);
    snprintf(p->json, sizeof(p->json), "%s.json", p->base);
    snprintf(p->aes, sizeof(p->aes), "%s.h264.aes", p->base);
    snprintf(p->aes_meta, sizeof(p->aes_meta), "%s.h264.aes.meta", p->base);
    snprintf(p->state, sizeof(p->state), "%s.mrbin.state", p->base);
    snprintf(p->info, sizeof(p->info), "%s.h264.info", p->base);
    return true;
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool write_atomic_text(const char *path, const char *text) {
    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return false;
    }
    fputs(text, fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static bool set_state(const clip_paths_t *p, core_clip_state_t st, const char *err) {
    char buf[160];
    if (err && err[0]) {
        snprintf(buf, sizeof(buf), "%s\nerror=%s\n", state_name(st), err);
    } else {
        snprintf(buf, sizeof(buf), "%s\n", state_name(st));
    }
    return write_atomic_text(p->state, buf);
}

static core_clip_state_t read_state(const clip_paths_t *p) {
    FILE *fp = fopen(p->state, "r");
    if (!fp) {
        if (file_exists(p->aes_meta)) {
            return CORE_CLIP_PENDING_UPLOAD;
        }
        if (file_exists(p->h264) && file_exists(p->json)) {
            return CORE_CLIP_SIDECAR_WRITTEN;
        }
        if (file_exists(p->h264)) {
            return CORE_CLIP_RECORDED_PLAIN;
        }
        return CORE_CLIP_NONE;
    }
    char line[64] = {0};
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return CORE_CLIP_NONE;
    }
    fclose(fp);
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    return state_from_name(line);
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static uint16_t seq_from_mp4(const char *mp4) {
    const char *name = basename_of(mp4);
    unsigned a = 0, b = 0;
    if (sscanf(name, "%u_%u_", &a, &b) == 2) {
        return (uint16_t)b;
    }
    return 0;
}

static bool write_info(const clip_paths_t *p, const core_clip_finish_t *info,
                       const char *upload_id) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "width=%u\nheight=%u\nfps=%d\nfps_source=%d\nfps_target=%d\n"
             "fps_recorded=%d\nduration_ms=%lu\nbitrate=%lu\ngop=%lu\n"
             "qp_min=%lu\nqp_max=%lu\nsequence=%lu\npir=%d\nupload_id=%s\n",
             (unsigned)info->width, (unsigned)info->height, info->fps,
             info->fps_source, info->fps_target, info->fps_recorded,
             (unsigned long)info->duration_ms, (unsigned long)info->bitrate,
             (unsigned long)info->gop, (unsigned long)info->qp_min,
             (unsigned long)info->qp_max, (unsigned long)info->sequence,
             info->pir_enabled ? 1 : 0, upload_id ? upload_id : "");
    return write_atomic_text(p->info, buf);
}

static bool load_info(const clip_paths_t *p, core_clip_finish_t *info, char *upload_id,
                      size_t upload_len) {
    memset(info, 0, sizeof(*info));
    snprintf(info->mp4_path, sizeof(info->mp4_path), "%s", p->mp4);
    FILE *fp = fopen(p->info, "r");
    if (!fp) {
        uint32_t dur = 0, frames = 0, fps = 0;
        (void)core_sd_read_rec_meta(p->mp4, &dur, &frames, &fps);
        info->duration_ms = dur * 1000U;
        info->fps = (int)fps;
        info->fps_recorded = (int)fps;
        return dur > 0 || file_exists(p->h264);
    }
    char line[200];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line;
        const char *v = eq + 1;
        if (strcmp(k, "width") == 0) info->width = (uint16_t)atoi(v);
        else if (strcmp(k, "height") == 0) info->height = (uint16_t)atoi(v);
        else if (strcmp(k, "fps") == 0) info->fps = atoi(v);
        else if (strcmp(k, "fps_source") == 0) info->fps_source = atoi(v);
        else if (strcmp(k, "fps_target") == 0) info->fps_target = atoi(v);
        else if (strcmp(k, "fps_recorded") == 0) info->fps_recorded = atoi(v);
        else if (strcmp(k, "duration_ms") == 0) info->duration_ms = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "bitrate") == 0) info->bitrate = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "gop") == 0) info->gop = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "qp_min") == 0) info->qp_min = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "qp_max") == 0) info->qp_max = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "sequence") == 0) info->sequence = (uint32_t)strtoul(v, nullptr, 10);
        else if (strcmp(k, "pir") == 0) info->pir_enabled = atoi(v) != 0;
        else if (strcmp(k, "upload_id") == 0 && upload_id) {
            snprintf(upload_id, upload_len, "%s", v);
        }
    }
    fclose(fp);
    return true;
}

FILE *core_clip_h264_open(const char *mp4_path) {
    clip_paths_t p;
    if (!paths_from_mp4(mp4_path, &p)) {
        return nullptr;
    }
    FILE *fp = fopen(p.h264, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "Apertura %s fallita (errno=%d)", p.h264, errno);
    }
    return fp;
}

bool core_clip_h264_rename(const char *old_mp4, const char *new_mp4) {
    clip_paths_t a, b;
    if (!paths_from_mp4(old_mp4, &a) || !paths_from_mp4(new_mp4, &b)) {
        return false;
    }
    const char *from[] = { a.h264, a.json, a.aes, a.aes_meta, a.state, a.info };
    const char *to[] = { b.h264, b.json, b.aes, b.aes_meta, b.state, b.info };
    bool ok = true;
    for (size_t i = 0; i < 6; ++i) {
        if (file_exists(from[i])) {
            remove(to[i]);
            if (rename(from[i], to[i]) != 0) {
                ESP_LOGW(TAG, "Rename %s → %s fallito", from[i], to[i]);
                ok = false;
            }
        }
    }
    return ok;
}

static bool ensure_sidecar(const clip_paths_t *p, const core_clip_finish_t *info) {
    if (file_exists(p->json)) {
        return true;
    }
    if (!core_mrbin_id_has_uid()) {
        return false;
    }
    core_sidecar_info_t sc = {};
    snprintf(sc.video_file, sizeof(sc.video_file), "%s", basename_of(p->h264));
    snprintf(sc.sidecar_file, sizeof(sc.sidecar_file), "%s", basename_of(p->json));
    sc.width = info->width;
    sc.height = info->height;
    sc.fps = info->fps;
    sc.fps_source = info->fps_source;
    sc.fps_target = info->fps_target;
    sc.fps_recorded = info->fps_recorded > 0 ? info->fps_recorded : info->fps;
    sc.duration_ms = info->duration_ms;
    sc.bitrate = info->bitrate;
    sc.gop = info->gop;
    sc.qp_min = info->qp_min;
    sc.qp_max = info->qp_max;
    sc.sequence = info->sequence;
    sc.pir_enabled = info->pir_enabled;
    sc.motion_extend = info->pir_enabled;
    sc.osd_enable = true;
    snprintf(sc.preset, sizeof(sc.preset), "%ux%u@%d",
             (unsigned)info->width, (unsigned)info->height, info->fps);
    return core_sidecar_write(p->json, &sc);
}

static bool ensure_encrypted(const clip_paths_t *p, const char *upload_id) {
    if (file_exists(p->aes) && file_exists(p->aes_meta)) {
        return true;
    }
    if (!core_mrbin_id_has_content_key() || !file_exists(p->h264)) {
        return false;
    }
    if (!core_aes_encrypt_clip(p->h264, p->aes, p->aes_meta, upload_id, basename_of(p->h264))) {
        return false;
    }
    if (remove(p->h264) != 0) {
        ESP_LOGW(TAG, "Eliminazione plaintext %s fallita", p->h264);
    }
    return true;
}

bool core_clip_queue_on_segment(const core_clip_finish_t *info) {
    if (!info || !info->mp4_path[0]) {
        return false;
    }
    clip_paths_t p;
    if (!paths_from_mp4(info->mp4_path, &p)) {
        return false;
    }
    if (!file_exists(p.h264)) {
        ESP_LOGW(TAG, "Nessun .h264 per %s — skip adattatore", info->mp4_path);
        return false;
    }

    char upload_id[CORE_MRBIN_UPLOAD_ID_MAX + 4] = {0};
    if (file_exists(p.info)) {
        core_clip_finish_t dummy = {};
        (void)load_info(&p, &dummy, upload_id, sizeof(upload_id));
    }
    if (upload_id[0] == 0 && core_mrbin_id_has_uid()) {
        uint16_t seq = info->sequence ? (uint16_t)info->sequence : seq_from_mp4(info->mp4_path);
        if (!core_mrbin_id_make_upload_id(upload_id, sizeof(upload_id), seq)) {
            ESP_LOGW(TAG, "upload_id non generato");
        }
    }
    (void)write_info(&p, info, upload_id);
    (void)set_state(&p, CORE_CLIP_RECORDED_PLAIN, nullptr);

    if (!ensure_sidecar(&p, info)) {
        ESP_LOGI(TAG, "Clip %s in RECORDED_PLAIN (attesa UID/sidecar)", basename_of(p.mp4));
        return true;
    }
    (void)set_state(&p, CORE_CLIP_SIDECAR_WRITTEN, nullptr);

    if (upload_id[0] && ensure_encrypted(&p, upload_id)) {
        (void)set_state(&p, CORE_CLIP_PENDING_UPLOAD, nullptr);
        ESP_LOGI(TAG, "Clip %s PENDING_UPLOAD", upload_id);
        return true;
    }
    ESP_LOGI(TAG, "Clip %s SIDECAR_WRITTEN (cifratura al prossimo enroll)", basename_of(p.mp4));
    return true;
}

static bool process_one(const clip_paths_t *p, volatile bool *cancel, bool *quota) {
    if (cancel && *cancel) {
        return false;
    }
    core_clip_state_t st = read_state(p);
    if (st == CORE_CLIP_UPLOADED_VERIFIED || st == CORE_CLIP_DELETED ||
        st == CORE_CLIP_DELETE_PENDING || st == CORE_CLIP_NONE) {
        return true;
    }

    core_clip_finish_t info = {};
    char upload_id[CORE_MRBIN_UPLOAD_ID_MAX + 4] = {0};
    (void)load_info(p, &info, upload_id, sizeof(upload_id));
    if (info.mp4_path[0] == 0) {
        snprintf(info.mp4_path, sizeof(info.mp4_path), "%s", p->mp4);
    }

    if (st == CORE_CLIP_RECORDED_PLAIN || st == CORE_CLIP_SIDECAR_WRITTEN) {
        if (!ensure_sidecar(p, &info)) {
            return true;
        }
        (void)set_state(p, CORE_CLIP_SIDECAR_WRITTEN, nullptr);
        if (upload_id[0] == 0 && core_mrbin_id_has_uid()) {
            if (!core_mrbin_id_make_upload_id(upload_id, sizeof(upload_id),
                                              info.sequence ? (uint16_t)info.sequence
                                                            : seq_from_mp4(p->mp4))) {
                return true;
            }
            (void)write_info(p, &info, upload_id);
        }
        if (upload_id[0] && ensure_encrypted(p, upload_id)) {
            (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, nullptr);
            st = CORE_CLIP_PENDING_UPLOAD;
        } else {
            return true;
        }
    }

    if (st == CORE_CLIP_ENCRYPTED_LOCAL) {
        (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, nullptr);
        st = CORE_CLIP_PENDING_UPLOAD;
    }
    if (st != CORE_CLIP_PENDING_UPLOAD && st != CORE_CLIP_UPLOADING && st != CORE_CLIP_ERROR) {
        return true;
    }
    if (*quota) {
        return true;
    }

    core_mrbin_aes_meta_t meta = {};
    if (!core_aes_meta_load(p->aes_meta, &meta)) {
        (void)set_state(p, CORE_CLIP_ERROR, "aes_meta");
        return true;
    }
    (void)set_state(p, CORE_CLIP_UPLOADING, nullptr);
    core_aes_upload_result_t up = core_aes_upload_clip(p->aes, p->json, &meta, cancel);
    switch (up) {
        case CORE_AES_UP_OK:
        case CORE_AES_UP_ALREADY:
            (void)set_state(p, CORE_CLIP_UPLOADED_VERIFIED, nullptr);
            return true;
        case CORE_AES_UP_HTTP403:
            ESP_LOGW(TAG, "HTTP 403 — re-enroll");
            (void)core_mrbin_id_clear_content_key();
            (void)core_aes_enroll(cancel);
            (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, "http403");
            return false;
        case CORE_AES_UP_HTTP409:
            (void)set_state(p, CORE_CLIP_ERROR, "http409");
            return true;
        case CORE_AES_UP_HTTP507:
            *quota = true;
            (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, "http507");
            return false;
        case CORE_AES_UP_INTEGRITY:
            (void)set_state(p, CORE_CLIP_ERROR, "integrity");
            return true;
        case CORE_AES_UP_CANCELLED:
            (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, nullptr);
            return false;
        case CORE_AES_UP_RETRY:
            (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, "retry");
            return false;
        default:
            (void)set_state(p, CORE_CLIP_ERROR, "upload");
            return true;
    }
}

static void scan_day(const char *daypath, void (*fn)(const clip_paths_t *, void *), void *user) {
    DIR *d = opendir(daypath);
    if (!d) {
        return;
    }
    struct dirent *f;
    while ((f = readdir(d)) != nullptr) {
        size_t n = strlen(f->d_name);
        if (n < 10) {
            continue;
        }
        bool is_mp4 = n >= 4 && strcmp(f->d_name + n - 4, ".mp4") == 0;
        bool is_state = n >= 12 && strcmp(f->d_name + n - 12, ".mrbin.state") == 0;
        bool is_h264 = n >= 5 && strcmp(f->d_name + n - 5, ".h264") == 0 &&
                       (n < 9 || strcmp(f->d_name + n - 9, ".aes.h264") != 0);
        bool is_aes_meta = n >= 14 && strcmp(f->d_name + n - 14, ".h264.aes.meta") == 0;
        if (!is_mp4 && !is_state && !is_h264 && !is_aes_meta) {
            continue;
        }
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", daypath, f->d_name);
        clip_paths_t p;
        if (is_mp4) {
            if (!paths_from_mp4(path, &p)) continue;
        } else {
            char mp4[192];
            snprintf(mp4, sizeof(mp4), "%s", path);
            char *dot = nullptr;
            if (is_state) {
                dot = strstr(mp4, ".mrbin.state");
            } else if (is_aes_meta) {
                dot = strstr(mp4, ".h264.aes.meta");
            } else {
                dot = strstr(mp4, ".h264");
            }
            if (!dot) continue;
            strcpy(dot, ".mp4");
            if (!paths_from_mp4(mp4, &p)) continue;
        }
        fn(&p, user);
    }
    closedir(d);
}

typedef struct {
    volatile bool *cancel;
    bool quota;
} proc_ctx_t;

static void process_cb(const clip_paths_t *p, void *user) {
    proc_ctx_t *ctx = (proc_ctx_t *)user;
    (void)process_one(p, ctx->cancel, &ctx->quota);
}

static void scan_all(void (*fn)(const clip_paths_t *, void *), void *user) {
    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        return;
    }
    struct dirent *day;
    while ((day = readdir(root)) != nullptr) {
        if (day->d_name[0] == '.' || strlen(day->d_name) != 8) {
            continue;
        }
        char daypath[64];
        snprintf(daypath, sizeof(daypath), "%s/%s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat st;
        if (stat(daypath, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        scan_day(daypath, fn, user);
    }
    closedir(root);
}

void core_clip_queue_process(volatile bool *cancel) {
    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "SD non montata — skip coda upload");
        return;
    }
    (void)core_mrbin_id_capture_sta_mac();
    core_mrbin_enroll_result_t en = core_aes_enroll(cancel);
    if (en == CORE_MRBIN_ENROLL_NO_NET) {
        ESP_LOGW(TAG, "Enroll: rete assente");
        return;
    }
    if (en == CORE_MRBIN_ENROLL_PENDING) {
        ESP_LOGI(TAG, "Enroll pending — attesa approvazione server");
    }
    proc_ctx_t ctx = { cancel, false };
    scan_all(process_cb, &ctx);
}

typedef struct {
    core_clip_queue_counts_t *c;
} count_ctx_t;

static void count_cb(const clip_paths_t *p, void *user) {
    count_ctx_t *ctx = (count_ctx_t *)user;
    core_clip_state_t st = read_state(p);
    switch (st) {
        case CORE_CLIP_PENDING_UPLOAD:
        case CORE_CLIP_ENCRYPTED_LOCAL:
            ctx->c->pending++;
            break;
        case CORE_CLIP_UPLOADING:
            ctx->c->uploading++;
            break;
        case CORE_CLIP_UPLOADED_VERIFIED:
            ctx->c->verified++;
            break;
        case CORE_CLIP_ERROR:
            ctx->c->error++;
            break;
        case CORE_CLIP_RECORDED_PLAIN:
        case CORE_CLIP_SIDECAR_WRITTEN:
            ctx->c->plaintext++;
            break;
        default:
            break;
    }
}

void core_clip_queue_counts(core_clip_queue_counts_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!core_sd_is_mounted()) {
        return;
    }
    count_ctx_t ctx = { out };
    scan_all(count_cb, &ctx);
}

core_clip_state_t core_clip_queue_state_for_mp4(const char *mp4_path) {
    clip_paths_t p;
    if (!paths_from_mp4(mp4_path, &p)) {
        return CORE_CLIP_NONE;
    }
    return read_state(&p);
}

static void clear_err_cb(const clip_paths_t *p, void *user) {
    (void)user;
    if (read_state(p) == CORE_CLIP_ERROR) {
        (void)set_state(p, CORE_CLIP_PENDING_UPLOAD, nullptr);
    }
}

bool core_clip_queue_clear_errors(void) {
    scan_all(clear_err_cb, nullptr);
    return true;
}
