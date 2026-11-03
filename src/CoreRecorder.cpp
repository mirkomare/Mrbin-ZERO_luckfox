#include "CoreRecorder.h"
#include "CoreClipQueue.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreEncFs.h"
#include "CoreGPIO.h"
#include "CoreH264Buffer.h"
#include "CoreSD.h"
#include "CoreSettings.h"
#include "CoreStatusLed.h"
#include "CoreTime.h"
#include "CoreVideo.h"
#include "CoreDemo.h"
#include "CoreLive.h"
#include "CoreRtsp.h"
#include "hal_capture.h"
#include "CoreIspCal.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_advance.h"
#include "esp_capture_sink.h"
#include "esp_gmf_video_enc.h"
#include "esp_gmf_video_fps_cvt.h"
#include "esp_gmf_video_ppa.h"
#include "esp_gmf_video_types.h"
#include "impl/esp_capture_text_overlay.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"
#include "impl/mp4_muxer.h"
#include "impl/avi_muxer.h"
#include "esp_muxer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "core_recorder";

#define REC_MIN_MP4_BYTES       1024
#define SD_PREP_DONE_BIT        BIT0
#define H264_CODEC_SPEC_MAX     512
#define REC_STATUS_LOG_MS       5000
#define REC_DRAIN_TIMEOUT_MS    1000
#define REC_FINAL_FLUSH_STOP_MS 2000   // max flush residuo (solo se stop non utente)
#define MANUAL_REC_DONE_BIT     BIT0
#define MANUAL_REC_TASK_STACK   12288
#define MANUAL_REC_TASK_PRIO    5

typedef struct {
    char  *ptr;
    size_t cap;
} file_io_buf_t;

static file_io_buf_t file_io_buf_alloc(void) {
    file_io_buf_t b = {};
    b.ptr = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (b.ptr) {
        b.cap = CORE_SD_FILE_BUF_BYTES;
    }
    return b;
}

static void file_io_buf_attach(FILE *fp, file_io_buf_t *b) {
    if (fp && b && b->ptr) {
        setvbuf(fp, b->ptr, _IOFBF, b->cap);
    }
}

static void file_io_buf_free(file_io_buf_t *b) {
    if (b && b->ptr) {
        heap_caps_free(b->ptr);
        b->ptr = nullptr;
        b->cap = 0;
    }
}

// encrypt_mp4_file: recover .tmp orfani e post-encrypt a file chiuso (inline AES off).
static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]);
static bool recorder_post_encrypt_if_needed(const core_settings_t *settings, const char *path);
static bool plain_mp4_has_ftyp(const char *path);

typedef bool (*recorder_should_stop_fn_t)(void);

typedef struct {
    char path[140];
} recorder_muxer_ctx_t;

typedef struct {
    esp_capture_handle_t capture;
    esp_capture_sink_handle_t sink;
    esp_capture_video_src_if_t *vsrc;
    esp_capture_overlay_if_t *overlay;
    uint16_t osd_w;
    uint16_t osd_h;
    uint8_t  osd_font;
    int64_t  osd_last_us;
    bool running;
    uint32_t warmup_left;
    uint32_t kept_frames;
    uint32_t pts_epoch_ms;
    int64_t  pts_origin_us;   // wall clock al primo frame utile
    uint32_t last_out_pts;    // ultimo PTS scritto su ring/muxer
    uint16_t video_width;
    uint16_t video_height;
    uint8_t video_fps;
    uint32_t frame_dur_ms;
    uint32_t video_bitrate;
    uint32_t video_gop;
    int64_t  last_kept_us;    // throttle fps + pace
    bool     need_idr_resync; // dopo gap/drop: tieni solo il prossimo IDR
    bool     from_live;       // VI già in stream: solo swap VENC, niente AE discard
    bool     from_rtsp;       // stessi NAL dello streamer, nessun secondo encoder
    uint8_t *rtsp_frame;
} recorder_capture_t;

#if CORE_REC_OSD_ENABLE
static void recorder_osd_close(esp_capture_overlay_if_t **overlay) {
    if (!overlay || !*overlay) {
        return;
    }
    (*overlay)->close(*overlay);
    *overlay = nullptr;
}

static bool recorder_osd_attach(esp_capture_sink_handle_t sink, const core_settings_t *settings,
                                uint16_t frame_w, uint16_t frame_h,
                                esp_capture_overlay_if_t **out_ov, uint16_t *out_w, uint16_t *out_h,
                                uint8_t *out_font) {
    if (!sink || !out_ov) {
        return false;
    }
    *out_ov = nullptr;
    if (settings && !settings->osd_enable) {
        ESP_LOGI(TAG, "OSD disabilitato in impostazioni");
        return false;
    }
    uint16_t x = 0, y = 0, bw = 0, bh = 0;
    core_settings_osd_pixel_pos(settings, frame_w, frame_h, &x, &y, &bw, &bh);
    uint8_t font = settings ? settings->osd_font : (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    if (!core_settings_osd_font_valid(font)) {
        font = (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    }
    esp_capture_rgn_t text_rgn = {
        .x = x,
        .y = y,
        .width = bw,
        .height = bh,
    };
    esp_capture_overlay_if_t *ov = esp_capture_new_text_overlay(&text_rgn);
    if (!ov) {
        ESP_LOGW(TAG, "OSD: new_text_overlay fallita");
        return false;
    }
    if (ov->open(ov) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "OSD: open fallita");
        ov->close(ov);
        return false;
    }
    ov->set_alpha(ov, 0xE0);
    esp_capture_rgn_t clear_rgn = {
        .x = 0,
        .y = 0,
        .width = text_rgn.width,
        .height = text_rgn.height,
    };
    esp_capture_text_overlay_draw_start(ov);
    esp_capture_text_overlay_clear(ov, &clear_rgn, COLOR_RGB565_BLACK);
    esp_capture_text_overlay_draw_finished(ov);

    if (esp_capture_sink_add_overlay(sink, ov) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "OSD: add_overlay fallita — continuo senza data/ora");
        ov->close(ov);
        return false;
    }
    if (esp_capture_sink_enable_overlay(sink, true) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "OSD: enable_overlay fallita");
    }
    *out_ov = ov;
    if (out_w) {
        *out_w = text_rgn.width;
    }
    if (out_h) {
        *out_h = text_rgn.height;
    }
    if (out_font) {
        *out_font = font;
    }
    ESP_LOGI(TAG, "OSD data/ora attivo font=%u box %ux%u @(%u,%u) frame %ux%u",
             (unsigned)font, (unsigned)text_rgn.width, (unsigned)text_rgn.height,
             (unsigned)text_rgn.x, (unsigned)text_rgn.y,
             (unsigned)frame_w, (unsigned)frame_h);
    return true;
}

static void recorder_osd_refresh(esp_capture_overlay_if_t *ov, uint16_t osd_w, uint16_t osd_h,
                                 uint8_t font, int64_t *last_us) {
    if (!ov || !last_us) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (*last_us > 0 && (now - *last_us) < CORE_REC_OSD_REFRESH_US) {
        return;
    }
    if (!core_settings_osd_font_valid(font)) {
        font = (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    }
    char when[40];
    if (core_time_is_valid()) {
        core_time_format_local(when, sizeof(when));
    } else {
        snprintf(when, sizeof(when), "--/--/---- --:--:--");
    }
    esp_capture_rgn_t clear_rgn = {
        .x = 0,
        .y = 0,
        .width = osd_w,
        .height = osd_h,
    };
    esp_capture_text_overlay_draw_info_t info = {
        .color = COLOR_RGB565_YELLOW,
        .font_size = font,
        .x = 6,
        .y = 6,
    };
    if (esp_capture_text_overlay_draw_start(ov) != ESP_CAPTURE_ERR_OK) {
        return;
    }
    (void)esp_capture_text_overlay_clear(ov, &clear_rgn, COLOR_RGB565_BLACK);
    (void)esp_capture_text_overlay_draw_text_fmt(ov, &info, "%s", when);
    (void)esp_capture_text_overlay_draw_finished(ov);
    *last_us = now;
}
#endif /* CORE_REC_OSD_ENABLE */

static bool recorder_capture_start_psram(recorder_capture_t *rc, uint32_t warmup_frames,
                                         const core_settings_t *settings, bool pir_fast,
                                         bool adopt_vi);
static bool rec_acquire_any(recorder_capture_t *rc, esp_capture_stream_frame_t *frame, bool no_wait);
static void rec_release_got(recorder_capture_t *rc, esp_capture_stream_frame_t *frame);

static bool recorder_capture_warmup(recorder_capture_t *rc) {
    if (rc->warmup_left > 0) {
        rc->warmup_left--;
        return true;
    }
    return false;
}

// PTS = solo orologio reale. Il passo minimo 1/fps allungava il playback
// (25 frame in 1 s di OSD → 2.5 s in timeline). I frame extra si scartano
// in rec_capture_one_frame, non si stirano. NON usare frame.pts GMF.
static uint32_t recorder_capture_assign_pts(recorder_capture_t *rc, uint32_t frame_pts_ms) {
    (void)frame_pts_ms;
    int64_t now_us = esp_timer_get_time();
    if (rc->video_fps <= CORE_VIDEO_FPS_LOW_MAX && rc->frame_dur_ms > 0) {
        if (rc->pts_origin_us == 0) {
            rc->pts_origin_us = now_us;
        }
        uint32_t pts = rc->pts_epoch_ms + rc->kept_frames * rc->frame_dur_ms;
        rc->last_out_pts = pts;
        return pts;
    }
    if (rc->pts_origin_us == 0) {
        rc->pts_origin_us = now_us;
        rc->last_out_pts = rc->pts_epoch_ms;
        return rc->pts_epoch_ms;
    }
    uint32_t wall = rc->pts_epoch_ms + (uint32_t)((now_us - rc->pts_origin_us) / 1000);
    if (wall <= rc->last_out_pts) {
        wall = rc->last_out_pts + 1;
    }
    rc->last_out_pts = wall;
    return wall;
}

static uint8_t recorder_capture_effective_fps(const recorder_capture_t *rc) {
    if (!rc || rc->kept_frames < 2 || rc->pts_origin_us == 0) {
        return rc ? rc->video_fps : (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    }
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - rc->pts_origin_us) / 1000);
    if (elapsed_ms < 1) {
        return rc->video_fps;
    }
    uint32_t fps = (rc->kept_frames * 1000U + elapsed_ms / 2U) / elapsed_ms;
    if (fps < CORE_REC_MUXER_FPS_MIN) {
        fps = CORE_REC_MUXER_FPS_MIN;
    }
    if (fps > 60) {
        fps = 60;
    }
    if (fps > rc->video_fps) {
        fps = rc->video_fps;
    }
    return (uint8_t)fps;
}

// FPS dichiarato nel muxer MP4: solo preset/config.
// Le stime su wall/ring sottostimavano spesso (~10 invece di 20) → playback a metà
// velocità e durata lista ~2× (es. 30 s → 58 s).
static uint8_t recorder_muxer_fps_for_open(const core_h264_buffer_t *buf,
                                           const recorder_capture_t *capture) {
    (void)buf;
    if (!capture || capture->video_fps < CORE_REC_MUXER_FPS_MIN) {
        return (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    }
    uint8_t fps = capture->video_fps;
    if (fps > 60) {
        fps = 60;
    }
    return fps;
}

static bool recorder_muxer_open_ready(const recorder_capture_t *capture, bool on_stop,
                                      bool pir_fast) {
    if (!capture || capture->pts_origin_us == 0) {
        return on_stop;
    }
    if (on_stop) {
        return capture->kept_frames >= 1;
    }
    if (pir_fast) {
        return capture->kept_frames >= CORE_REC_MUXER_OPEN_MIN_FRAMES_PIR;
    }
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - capture->pts_origin_us) / 1000);
    return elapsed >= CORE_REC_MUXER_OPEN_MIN_MS &&
           capture->kept_frames >= CORE_REC_MUXER_OPEN_MIN_FRAMES;
}

static bool recorder_frame_accept(recorder_capture_t *rc, uint32_t frame_pts_ms, uint32_t *out_pts_ms) {
    if (recorder_capture_warmup(rc)) {
        return false;
    }
    if (rc->kept_frames == 0) {
        ESP_LOGI(TAG, "Warmup completato — PTS CBR da 0");
    }
    *out_pts_ms = recorder_capture_assign_pts(rc, frame_pts_ms);
    rc->kept_frames++;
    return true;
}

typedef struct {
    EventGroupHandle_t ev;
    uint32_t           core_id;
    char               tmp_path[140];
    char               final_path[128];
    char               enc_path[160];   // "/enc" + final_path: mux cifra on-the-fly
    char               session_base[128]; // path senza .mp4 (base per _partNN)
    uint32_t           segment_index;   // 0 = primo file senza suffisso; poi 1… = _partNN
    bool               segment_parts;   // rollover possibile (GPIO stop o timer > segmento)
    bool               rec_encrypt;     // effective: enc_writer vs plain (runtime da settings)
    bool               paths_ok;
    bool               sd_mounted;
} sd_prep_ctx_t;

static void rec_mux_temp_path(char *out, size_t out_len, const char *final_mp4);
static bool rec_mux_commit_file(const char *final_mp4);

static bool recorder_segment_path(char *out, size_t out_len, const char *session_base, uint32_t part);

typedef struct {
    esp_muxer_handle_t handle;
    int                stream_index;
    bool               active;
} recorder_muxer_t;

#define CORE_CRYPTO_FILE_HEADER   20   // MRBI (4) + IV (16)

static bool recording_file_is_valid(const char *path);

static bool plain_mp4_has_ftyp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t buf[512];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 8) {
        return false;
    }
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (buf[i + 4] == 'f' && buf[i + 5] == 't' && buf[i + 6] == 'y' && buf[i + 7] == 'p') {
            return true;
        }
    }
    return false;
}

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]) {
    int64_t t0 = esp_timer_get_time();

    struct stat st;
    if (stat(plain_path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "MP4 temporaneo non valido: %s", plain_path);
        return false;
    }
    if (!plain_mp4_has_ftyp(plain_path)) {
        ESP_LOGE(TAG, "MP4 tmp senza header ftyp: %s", plain_path);
        return false;
    }

    FILE *fin = fopen(plain_path, "rb");
    if (!fin) {
        ESP_LOGE(TAG, "Apertura %s fallita (errno=%d)", plain_path, errno);
        return false;
    }

    // Cifratura atomica: scrive su <enc_path>.part e rinomina in .mp4 solo a fine OK.
    // Così il file .mp4 finale non è mai visibile incompleto/0 byte.
    char part_path[160];
    snprintf(part_path, sizeof(part_path), "%s.part", enc_path);
    remove(part_path);

    FILE *fout = fopen(part_path, "wb");
    if (!fout) {
        ESP_LOGE(TAG, "Creazione %s fallita (errno=%d)", part_path, errno);
        fclose(fin);
        return false;
    }

    file_io_buf_t fin_vbuf = file_io_buf_alloc();
    file_io_buf_t fout_vbuf = file_io_buf_alloc();
    file_io_buf_attach(fin, &fin_vbuf);
    file_io_buf_attach(fout, &fout_vbuf);

    size_t io_buf_size = CORE_REC_ENCRYPT_IO_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        io_buf_size = 64 * 1024;
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    core_crypto_ctx_t ctx = {};

    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (fwrite(&magic, 1, sizeof(magic), fout) != sizeof(magic)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    if (fwrite(iv, 1, sizeof(iv), fout) != sizeof(iv)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    if (!core_crypto_init_ctx(&ctx, key, iv)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    uint64_t offset = 0;
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, io_buf_size, fin)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (fwrite(buf, 1, n, fout) != n) {
            ok = false;
            break;
        }
        offset += n;
    }

    core_crypto_deinit_ctx(&ctx);
    fflush(fout);
    heap_caps_free(buf);
    file_io_buf_free(&fin_vbuf);
    file_io_buf_free(&fout_vbuf);
    fclose(fin);
    fclose(fout);

    if (!ok) {
        remove(part_path);
        return false;
    }

    // Rinomina atomica sul path cifrato. Il plaintext resta al chiamante
    // (replace/rename): non cancellarlo qui, altrimenti post-encrypt perde il file.
    remove(enc_path);
    if (rename(part_path, enc_path) != 0) {
        ESP_LOGE(TAG, "Rename %s -> %s fallito (errno=%d)", part_path, enc_path, errno);
        remove(part_path);
        return false;
    }

    ESP_LOGI(TAG, "Cifratura MP4 %ld bytes in %lld ms (buf=%u)",
             (long)st.st_size, (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)io_buf_size);
    return true;
}

static int recorder_process_encrypt_later_in_dir(const char *dirpath, const core_settings_t *settings,
                                                 int *encrypted_out, int *cleared_out) {
    if (!dirpath || !settings) {
        return 0;
    }
    DIR *sub = opendir(dirpath);
    if (!sub) {
        return 0;
    }
    char (*names)[96] = (char (*)[96])calloc(48, 96);
    int n_names = 0;
    if (names) {
        struct dirent *f;
        while (n_names < 48 && (f = readdir(sub)) != nullptr) {
            size_t len = strlen(f->d_name);
            const char *suf = ".encrypt-later";
            const size_t slen = 14;
            if (len > slen && len < 96 && strcmp(f->d_name + len - slen, suf) == 0) {
                snprintf(names[n_names], 96, "%s", f->d_name);
                n_names++;
            }
        }
    }
    closedir(sub);
    if (!names) {
        return 0;
    }

    int done = 0;
    const bool want = settings->rec_encrypt && settings->aes_key_valid;
    for (int i = 0; i < n_names; ++i) {
        char flag_path[220];
        snprintf(flag_path, sizeof(flag_path), "%.120s/%.94s", dirpath, names[i]);
        size_t flen = strlen(flag_path);
        if (flen <= 14) {
            continue;
        }
        char mp4_path[220];
        snprintf(mp4_path, sizeof(mp4_path), "%.*s", (int)(flen - 14), flag_path);

        struct stat st;
        if (stat(mp4_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
            ESP_LOGW(TAG, "encrypt-later: MP4 assente/corto, rimuovo flag %s", flag_path);
            remove(flag_path);
            if (cleared_out) {
                (*cleared_out)++;
            }
            done++;
            continue;
        }

        if (!want) {
            remove(flag_path);
            if (cleared_out) {
                (*cleared_out)++;
            }
            done++;
            continue;
        }

        if (!plain_mp4_has_ftyp(mp4_path)) {
            /* Already MRBI or not a plain MP4 — drop marker. */
            remove(flag_path);
            if (cleared_out) {
                (*cleared_out)++;
            }
            done++;
            continue;
        }

        (void)recorder_post_encrypt_if_needed(settings, mp4_path);
        if (!plain_mp4_has_ftyp(mp4_path)) {
            remove(flag_path);
            if (encrypted_out) {
                (*encrypted_out)++;
            }
            ESP_LOGI(TAG, "encrypt-later: cifrato %s", mp4_path);
            done++;
        } else {
            ESP_LOGW(TAG, "encrypt-later: cifratura fallita, riprovo al prossimo boot: %s",
                     mp4_path);
        }
    }
    free(names);
    return done;
}

int core_recorder_process_pending_encrypt(const core_settings_t *settings) {
    if (!settings) {
        return 0;
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "encrypt-later: SD non montata");
        return 0;
    }

    int encrypted = 0;
    int cleared = 0;
    int total = 0;

    /* PIR flat dir */
    char mrbin_dir[96];
    snprintf(mrbin_dir, sizeof(mrbin_dir), "%s/mrbin", CORE_SD_MOUNT_POINT);
    total += recorder_process_encrypt_later_in_dir(mrbin_dir, settings, &encrypted, &cleared);

    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (root) {
        struct dirent *day;
        while ((day = readdir(root)) != nullptr) {
            if (day->d_name[0] == '.' || strcmp(day->d_name, "mrbin") == 0) {
                continue;
            }
            char daypath[96];
            snprintf(daypath, sizeof(daypath), "%s/%.64s", CORE_SD_MOUNT_POINT, day->d_name);
            struct stat dst;
            if (stat(daypath, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
                continue;
            }
            total += recorder_process_encrypt_later_in_dir(daypath, settings, &encrypted, &cleared);
        }
        closedir(root);
    }

    if (total) {
        ESP_LOGI(TAG, "encrypt-later: %d processati (%d cifrati, %d flag rimossi)",
                 total, encrypted, cleared);
    }
    return total;
}

static bool file_contains_tag(const char *path, const char tag[4]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t buf[4096 + 3];
    size_t carry = 0;
    bool found = false;
    for (;;) {
        size_t n = fread(buf + carry, 1, 4096, fp);
        size_t tot = carry + n;
        if (tot < 4) {
            break;
        }
        for (size_t i = 0; i + 4 <= tot; ++i) {
            if (memcmp(buf + i, tag, 4) == 0) {
                found = true;
                break;
            }
        }
        if (found || n == 0) {
            break;
        }
        memmove(buf, buf + tot - 3, 3);
        carry = 3;
    }
    fclose(fp);
    return found;
}

static bool rec_tmp_still_live(const struct stat *st) {
    /* Il moov si scrive solo in chiusura. Finché la registrazione è attiva,
     * o il file ha meno di CORE_REC_TMP_ZOMBIE_SEC, il .tmp non è uno zombi. */
    if (core_recorder_manual_is_active()) {
        return true;
    }
    const time_t now = time(nullptr);
    if (now < (time_t)1700000000 || !st) {
        return false;
    }
    const int64_t age = (int64_t)now - (int64_t)st->st_mtime;
    return age >= 0 && age < (int64_t)CORE_REC_TMP_ZOMBIE_SEC;
}

static int recover_tmp_in_dir(const char *daypath, const core_settings_t *settings) {
    DIR *sub = opendir(daypath);
    if (!sub) {
        return 0;
    }
    char (*names)[80] = (char (*)[80])calloc(32, 80);
    int n_names = 0;
    if (names) {
        struct dirent *f;
        while (n_names < 32 && (f = readdir(sub)) != nullptr) {
            size_t len = strlen(f->d_name);
            const bool mp4_tmp = len >= 8 && strcmp(f->d_name + len - 8, ".mp4.tmp") == 0;
            const bool part = len >= 5 && strcmp(f->d_name + len - 5, ".part") == 0;
            if ((mp4_tmp || part) && len < 80) {
                snprintf(names[n_names], 80, "%s", f->d_name);
                n_names++;
            }
        }
    }
    closedir(sub);
    if (!names) {
        return 0;
    }

    int acted = 0;
    for (int i = 0; i < n_names; ++i) {
        char tmp_path[180];
        snprintf(tmp_path, sizeof(tmp_path), "%.96s/%.78s", daypath, names[i]);
        struct stat st;
        if (stat(tmp_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (rec_tmp_still_live(&st)) {
            continue;
        }
        size_t nlen = strlen(names[i]);
        const bool part = nlen >= 5 && strcmp(names[i] + nlen - 5, ".part") == 0;
        if (part) {
            ESP_LOGW(TAG, "TMP zombie .part, elimino: %s", tmp_path);
            remove(tmp_path);
            acted++;
            continue;
        }
        char final_path[180];
        snprintf(final_path, sizeof(final_path), "%.*s",
                 (int)(strlen(tmp_path) - 4), tmp_path);
        struct stat st_final;
        if (stat(final_path, &st_final) == 0 && st_final.st_size >= (off_t)REC_MIN_MP4_BYTES) {
            ESP_LOGI(TAG, "TMP zombie già committato, elimino: %s", tmp_path);
            remove(tmp_path);
            acted++;
            continue;
        }
        const bool playable = st.st_size >= (off_t)REC_MIN_MP4_BYTES &&
                              plain_mp4_has_ftyp(tmp_path) &&
                              file_contains_tag(tmp_path, "moov");
        if (!playable) {
            ESP_LOGW(TAG, "TMP zombie senza moov, elimino: %s", tmp_path);
            remove(tmp_path);
            acted++;
            continue;
        }
        if (settings->rec_encrypt && settings->aes_key_valid) {
            if (encrypt_mp4_file(tmp_path, final_path, settings->aes_key)) {
                ESP_LOGI(TAG, "TMP zombie cifrato: %s", final_path);
                remove(tmp_path);
                acted++;
            } else {
                ESP_LOGW(TAG, "Cifratura zombie fallita, il .tmp resta: %s", tmp_path);
            }
            continue;
        }
        if (rename(tmp_path, final_path) == 0) {
            ESP_LOGI(TAG, "TMP zombie promosso: %s", final_path);
            acted++;
        } else {
            ESP_LOGW(TAG, "Promote zombie fallito, elimino: %s", tmp_path);
            remove(tmp_path);
            acted++;
        }
    }
    free(names);
    return acted;
}

int core_recorder_recover_tmp_files(const core_settings_t *settings) {
    if (!settings) {
        return 0;
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "Recupero .tmp: SD non montata");
        return 0;
    }

    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "Recupero .tmp: opendir %s fallito (errno=%d)", CORE_SD_MOUNT_POINT, errno);
        return 0;
    }

    int acted = 0;
    struct dirent *day;
    while ((day = readdir(root)) != nullptr) {
        if (day->d_name[0] == '.' || day->d_name[0] == 0) {
            continue;
        }
        char daypath[96];
        snprintf(daypath, sizeof(daypath), "%s/%.64s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat dst;
        if (stat(daypath, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            continue;
        }
        acted += recover_tmp_in_dir(daypath, settings);
    }
    closedir(root);

    if (acted) {
        ESP_LOGI(TAG, "Recupero .tmp: %d zombie chiusi", acted);
    }
    return acted;
}

static int storage_path_handler(esp_muxer_slice_info_t *info, void *ctx) {
    recorder_muxer_ctx_t *mux_ctx = (recorder_muxer_ctx_t *)ctx;
    snprintf(info->file_path, info->len, "%s", mux_ctx->path);
    return 0;
}

// ---------------------------------------------------------------------------
// File-writer cifrante del muxer: cifra AES-128-CTR on-the-fly mentre esp_muxer
// scrive l'MP4, con pieno controllo di open/write/seek (nessuno stdio, nessun
// VFS, nessun fstat sfasato). Header [MRBI][IV] a offset 0; l'MP4 logico parte
// al byte CORE_CRYPTO_FILE_HEADER. Compatibile in lettura con stream_decrypted_mp4.
// ---------------------------------------------------------------------------
static uint8_t s_mux_key[16];
static bool    s_mux_key_set = false;

typedef struct {
    FILE             *fp;
    char             *vbuf;
    uint64_t          logical_pos;   // 0 = primo byte MP4
    uint64_t          file_end_pos;  // offset fisico dopo ultimo byte scritto
    core_crypto_ctx_t crypto;
    uint8_t          *scratch;
    size_t            scratch_cap;
    size_t            pending_len;   // coalesce write muxer prima di AES+SD
} enc_writer_t;

static int enc_writer_flush_pending(enc_writer_t *w, bool force_fsync) {
    if (!w || w->pending_len == 0) {
        return 0;
    }
    if (!w->fp) {
        return -1;
    }
    uint64_t phys = w->logical_pos + CORE_CRYPTO_FILE_HEADER;
    if (phys != w->file_end_pos) {
        if (fseek(w->fp, (long)phys, SEEK_SET) != 0) {
            return -1;
        }
    }
    if (!core_crypto_crypt_at(&w->crypto, w->scratch, w->pending_len, w->logical_pos)) {
        return -1;
    }
    size_t wr = fwrite(w->scratch, 1, w->pending_len, w->fp);
    if (wr == w->pending_len && wr > 0) {
        static int noted = 0;
        if (!noted) {
            noted = 1;
            (void)fflush(w->fp);
            core_note_first_file_bytes("enc", (unsigned long)wr);
        }
    }
    if (wr != w->pending_len) {
        static int64_t s_last_enc_wr_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_enc_wr_us >= 1000000) {
            s_last_enc_wr_us = now;
            ESP_LOGW(TAG, "enc_writer flush fallita (errno=%d, pending=%u)",
                     errno, (unsigned)w->pending_len);
        }
        return -1;
    }
    w->logical_pos += (uint64_t)wr;
    w->file_end_pos = w->logical_pos + CORE_CRYPTO_FILE_HEADER;
    w->pending_len = 0;
    if (force_fsync) {
        fflush(w->fp);
        fsync(fileno(w->fp));
    }
    return (int)wr;
}

static void *enc_writer_open(char *path) {
    if (!s_mux_key_set) {
        ESP_LOGE(TAG, "Chiave muxer non impostata: apertura %s negata", path);
        return nullptr;
    }
    enc_writer_t *w = (enc_writer_t *)calloc(1, sizeof(*w));
    if (!w) {
        return nullptr;
    }
    w->fp = fopen(path, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "fopen(%s) fallita (errno=%d)", path, errno);
        free(w);
        return nullptr;
    }
    w->vbuf = (char *)heap_caps_malloc(CORE_ENC_SD_FILE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w->vbuf) {
        w->vbuf = (char *)malloc(CORE_ENC_SD_FILE_BUF_BYTES);
    }
    if (w->vbuf) {
        setvbuf(w->fp, w->vbuf, _IOFBF, CORE_ENC_SD_FILE_BUF_BYTES);
    }
    w->scratch_cap = CORE_ENC_WRITER_SCRATCH_BYTES;
    w->scratch = (uint8_t *)heap_caps_malloc(w->scratch_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w->scratch) {
        w->scratch = (uint8_t *)malloc(w->scratch_cap);
    }
    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (!w->scratch ||
        fwrite(&magic, 1, sizeof(magic), w->fp) != sizeof(magic) ||
        fwrite(iv, 1, sizeof(iv), w->fp) != sizeof(iv) ||
        !core_crypto_init_ctx(&w->crypto, s_mux_key, iv)) {
        ESP_LOGE(TAG, "Init writer cifrante fallito: %s", path);
        if (w->scratch) free(w->scratch);
        if (w->vbuf) free(w->vbuf);
        fclose(w->fp);
        free(w);
        return nullptr;
    }
    w->logical_pos = 0;
    w->file_end_pos = CORE_CRYPTO_FILE_HEADER;
    w->pending_len = 0;
    return w;
}

static int enc_writer_write(void *writer, void *buffer, int len) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w || len <= 0) {
        return len == 0 ? 0 : -1;
    }
    const uint8_t *src = (const uint8_t *)buffer;
    int done = 0;
    while (done < len) {
        size_t room = w->scratch_cap - w->pending_len;
        if (room == 0) {
            if (enc_writer_flush_pending(w, false) < 0) {
                return done > 0 ? done : -1;
            }
            room = w->scratch_cap;
        }
        size_t n = (size_t)(len - done);
        if (n > room) {
            n = room;
        }
        memcpy(w->scratch + w->pending_len, src + done, n);
        w->pending_len += n;
        done += (int)n;
        if (w->pending_len >= CORE_ENC_WRITER_FLUSH_BYTES ||
            w->pending_len >= w->scratch_cap) {
            if (enc_writer_flush_pending(w, false) < 0) {
                return done > (int)n ? done - (int)n : -1;
            }
        }
    }
    return done;
}

static int enc_writer_seek(void *writer, uint64_t pos) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w) return -1;
    if (enc_writer_flush_pending(w, false) < 0) {
        return -1;
    }
    w->logical_pos = pos;
    return 0;
}

static int enc_writer_close(void *writer) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w) return -1;
    if (enc_writer_flush_pending(w, false) < 0) {
        if (w->fp) fclose(w->fp);
        core_crypto_deinit_ctx(&w->crypto);
        if (w->scratch) free(w->scratch);
        if (w->vbuf) free(w->vbuf);
        free(w);
        return -1;
    }
    if (w->fp) {
        fflush(w->fp);
        fsync(fileno(w->fp));
        fclose(w->fp);
    }
    core_crypto_deinit_ctx(&w->crypto);
    if (w->scratch) free(w->scratch);
    if (w->vbuf) free(w->vbuf);
    free(w);
    return 0;
}

static void core_recorder_set_mux_key(const uint8_t key[16]) {
    memcpy(s_mux_key, key, 16);
    s_mux_key_set = true;
}

static bool s_session_rec_encrypt = false;

struct rec_dual_job_ctx;
static struct rec_dual_job_ctx *s_active_rec_job = nullptr;

static bool recorder_settings_want_encrypt(const core_settings_t *settings) {
#if CORE_REC_PLAIN_SD_BENCHMARK
    (void)settings;
    return false;
#else
    return settings && settings->rec_encrypt && settings->aes_key_valid;
#endif
}

// Luckfox: AES software in enc_writer (nel thread mux) strozza la rec a ~2 fps
// a 3MP/1080. Mux sempre in chiaro al fps NVS; post-encrypt → MRBI a file chiuso.
// enc_writer resta per export AVI e recover .tmp. Non cancellare l'unica copia.
static bool recorder_want_inline_aes(const core_settings_t *settings) {
    (void)settings;
    return false;
}

static bool recorder_post_encrypt_if_needed(const core_settings_t *settings, const char *path) {
    if (!recorder_settings_want_encrypt(settings) || !path || !path[0]) {
        return true;
    }
    if (!plain_mp4_has_ftyp(path)) {
        return true; /* già MRBI oppure assente */
    }
    char enc_out[180];
    snprintf(enc_out, sizeof(enc_out), "%s.enc", path);
    if (!encrypt_mp4_file(path, enc_out, settings->aes_key)) {
        ESP_LOGE(TAG, "Cifratura post-rec fallita: %s — lascio il plaintext", path);
        remove(enc_out);
        return true;
    }
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "remove(%s) errno=%d — provo comunque il rename", path, errno);
    }
    if (rename(enc_out, path) != 0) {
        ESP_LOGE(TAG, "Sostituzione MP4 cifrato fallita: %s ← %s (errno=%d)",
                 path, enc_out, errno);
        if (rename(enc_out, path) != 0 && access(path, F_OK) != 0) {
            ESP_LOGE(TAG, "Né plaintext né cifrato su %s", path);
            return false;
        }
        return access(path, F_OK) == 0;
    }
    ESP_LOGI(TAG, "MP4 cifrato a fine clip (MRBI+AES-CTR): %s", path);
    return true;
}

/* PIR does not encrypt on the start path and does not wait on AES.
 * encrypt_mp4_file stays for a later batch: a closed plain mp4 with a
 * sibling "<path>.encrypt-later" marker can be encrypted afterwards.
 * Do not call that step before the file is being written. */
static void recorder_flag_plain_for_later_encrypt(const char *path) {
    if (!path || !path[0]) {
        return;
    }
    char flag[200];
    snprintf(flag, sizeof(flag), "%s.encrypt-later", path);
    FILE *fp = fopen(flag, "w");
    if (!fp) {
        ESP_LOGW(TAG, "Flag cifra-dopo non scritto: %s", flag);
        return;
    }
    fputs("# closed plain mp4; batch AES/MRBI later via encrypt_mp4_file\n", fp);
    fprintf(fp, "%s\n", path);
    fflush(fp);
    fclose(fp);
    ESP_LOGI(TAG, "MP4 in chiaro chiuso, cifra dopo (flag %s)", flag);
}

static void recorder_finish_closed_mp4(const core_settings_t *settings, const char *path,
                                       bool pir_fast) {
    if (pir_fast) {
        if (recorder_settings_want_encrypt(settings)) {
            recorder_flag_plain_for_later_encrypt(path);
        }
        return;
    }
    (void)recorder_post_encrypt_if_needed(settings, path);
}

static bool recorder_mux_use_encrypt(void);

// Writer muxer in chiaro (stdio + buffer grande): max fps quando rec_encrypt=0.
typedef struct {
    FILE *fp;
    char *buf;
} plain_writer_t;

static uint32_t s_plain_closed_part_max = 0;

static uint32_t plain_writer_path_part(const char *path) {
    if (!path) {
        return 1;
    }
    const char *part = strstr(path, "_part");
    if (!part || part[5] == '\0') {
        return 1;
    }
    unsigned long n = strtoul(part + 5, nullptr, 10);
    return n > 0 ? (uint32_t)n : 1U;
}

#if CORE_REC_PLAIN_SD_BENCHMARK
static void rec_plain_bench_path(char *path, size_t len) {
    if (!path || len < 16) {
        return;
    }
    char *dot = strrchr(path, '.');
    if (!dot || strcmp(dot, ".mp4") != 0) {
        return;
    }
    *dot = '\0';
    strncat(path, "_bench.mp4", len - strlen(path) - 1);
}
#endif

static void *plain_writer_open(char *path) {
    uint32_t part = plain_writer_path_part(path);
    struct stat st;
    if (part <= s_plain_closed_part_max &&
        stat(path, &st) == 0 && st.st_size >= (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "plain open: rifiuto truncate segmento %lu già chiuso %s (%ld B)",
                 (unsigned long)part, path, (long)st.st_size);
        return nullptr;
    }
    plain_writer_t *w = (plain_writer_t *)calloc(1, sizeof(*w));
    if (!w) {
        return nullptr;
    }
    w->fp = fopen(path, "wb");
    if (!w->fp && errno == EROFS) {
        if (core_sd_ensure_rw()) {
            w->fp = fopen(path, "wb");
        }
    }
    if (!w->fp) {
        ESP_LOGE(TAG, "plain open(%s) fallita (errno=%d)", path, errno);
        free(w);
        return nullptr;
    }
    size_t vbuf_sz = CORE_REC_PLAIN_SD_FILE_BUF_BYTES;
#if CORE_REC_PLAIN_SD_BENCHMARK
    vbuf_sz = CORE_SD_FILE_BUF_BYTES;
#endif
    w->buf = (char *)malloc(vbuf_sz);
    if (w->buf) {
        setvbuf(w->fp, w->buf, _IOFBF, vbuf_sz);
    }
#if CORE_REC_PLAIN_SD_BENCHMARK
    ESP_LOGW(TAG, "BENCHMARK: MP4 in chiaro su %s (cache muxer %u KB, NO AES)",
             path, (unsigned)(CORE_MUXER_RAM_CACHE_PLAIN_BYTES / 1024U));
#else
    ESP_LOGI(TAG, "MP4 in chiaro su %s (cache muxer %u KB, setvbuf %u KB — max fps)",
             path, (unsigned)(CORE_MUXER_RAM_CACHE_PLAIN_BYTES / 1024U),
             (unsigned)(vbuf_sz / 1024U));
#endif
    return w;
}

static int plain_writer_write(void *writer, void *buffer, int len) {
    plain_writer_t *w = (plain_writer_t *)writer;
    if (!w || !w->fp || len <= 0) {
        return len == 0 ? 0 : -1;
    }
    size_t n = fwrite(buffer, 1, (size_t)len, w->fp);
    if (n == (size_t)len && n > 0) {
        static int noted = 0;
        if (!noted) {
            noted = 1;
            (void)fflush(w->fp);
            core_note_first_file_bytes("plain", (unsigned long)n);
        }
    }
    return (n == (size_t)len) ? len : (int)n;
}

static int plain_writer_seek(void *writer, uint64_t pos) {
    plain_writer_t *w = (plain_writer_t *)writer;
    if (!w || !w->fp) {
        return -1;
    }
    /* uClibc setvbuf: seek without flush can drop an unflushed moov. */
    if (fflush(w->fp) != 0) {
        return -1;
    }
    return fseek(w->fp, (long)pos, SEEK_SET) == 0 ? 0 : -1;
}

static int plain_writer_close(void *writer) {
    plain_writer_t *w = (plain_writer_t *)writer;
    if (!w) {
        return -1;
    }
    if (w->fp) {
        fflush(w->fp);
        fsync(fileno(w->fp));
        fclose(w->fp);
    }
    if (w->buf) {
        free(w->buf);
    }
    free(w);
    return 0;
}

static bool recording_file_is_valid(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        return false;
    }
    uint32_t magic = 0;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        (void)fread(&magic, 1, sizeof(magic), fp);
        fclose(fp);
    }
    bool enc_ok = (magic == CORE_CRYPTO_MAGIC);
    ESP_LOGI(TAG, "MP4 %s: %s (%ld bytes, header=%s)", enc_ok ? "OK cifrato" : "SENZA header cifrato",
             path, (long)st.st_size, enc_ok ? "MRBI" : "assente");
    return true;
}

static void rec_ppa_aspect_crop(uint16_t src_w, uint16_t src_h,
                                uint16_t dst_w, uint16_t dst_h,
                                esp_gmf_video_rgn_t *rgn) {
    if (!rgn || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return;
    }
    rgn->x = 0;
    rgn->y = 0;
    rgn->width = src_w;
    rgn->height = src_h;
    const uint32_t src_a = (uint32_t)src_w * (uint32_t)dst_h;
    const uint32_t dst_a = (uint32_t)dst_w * (uint32_t)src_h;
    if (src_a == dst_a) {
        return;
    }
    if (src_a > dst_a) {
        uint32_t crop_w = ((uint32_t)src_h * (uint32_t)dst_w / (uint32_t)dst_h) & ~1u;
        if (crop_w == 0 || crop_w > src_w) {
            return;
        }
        rgn->width = (uint16_t)crop_w;
        rgn->x = (uint16_t)(((src_w - crop_w) / 2u) & ~1u);
    } else {
        uint32_t crop_h = ((uint32_t)src_w * (uint32_t)dst_h / (uint32_t)dst_w) & ~1u;
        if (crop_h == 0 || crop_h > src_h) {
            return;
        }
        rgn->height = (uint16_t)crop_h;
        rgn->y = (uint16_t)(((src_h - crop_h) / 2u) & ~1u);
    }
}

static void rec_apply_ppa_aspect_crop(esp_capture_sink_handle_t sink,
                                      uint16_t dst_w, uint16_t dst_h) {
    uint16_t sw = 0;
    uint16_t sh = 0;
    if (!sink || !core_video_get_sensor_size(&sw, &sh) || sw == 0 || sh == 0) {
        return;
    }
    if (sw == dst_w && sh == dst_h) {
        return;
    }
    esp_gmf_element_handle_t ppa = nullptr;
    if (esp_capture_sink_get_element_by_tag(
            sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_ppa", &ppa) != ESP_CAPTURE_ERR_OK ||
        !ppa) {
        return;
    }
    esp_gmf_video_rgn_t crop = {};
    uint16_t tx = 0, ty = 0, tw = 0, th = 0;
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708 &&
        core_video_imx708_trim_rgn(sw, sh, &tx, &ty, &tw, &th)) {
        crop.x = tx;
        crop.y = ty;
        crop.width = tw;
        crop.height = th;
        rec_ppa_aspect_crop(tw, th, dst_w, dst_h, &crop);
        crop.x = (uint16_t)(tx + crop.x);
        crop.y = (uint16_t)(ty + crop.y);
    } else {
        rec_ppa_aspect_crop(sw, sh, dst_w, dst_h, &crop);
    }
    if (esp_gmf_video_ppa_set_cropped_rgn(ppa, &crop) == ESP_GMF_ERR_OK) {
        ESP_LOGI(TAG, "PPA crop %ux%u+%u+%u → %ux%u (FOV 16:9 del 960, non zoom centrale)",
                 (unsigned)crop.width, (unsigned)crop.height,
                 (unsigned)crop.x, (unsigned)crop.y,
                 (unsigned)dst_w, (unsigned)dst_h);
    }
}

static esp_capture_err_t rec_capture_pipeline_event(esp_capture_event_t event, void *ctx) {
    if (event != ESP_CAPTURE_EVENT_VIDEO_PIPELINE_BUILT || !ctx) {
        return ESP_CAPTURE_ERR_OK;
    }
    recorder_capture_t *rc = (recorder_capture_t *)ctx;
    if (!rc->sink) {
        return ESP_CAPTURE_ERR_OK;
    }

    uint8_t sensor_fps = 0;
    (void)core_video_get_sensor_fps(&sensor_fps);
    esp_gmf_element_handle_t fps_cvt = nullptr;
    if (esp_capture_sink_get_element_by_tag(
            rc->sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_fps_cvt", &fps_cvt) == ESP_CAPTURE_ERR_OK &&
        fps_cvt) {
        if (esp_gmf_video_fps_cvt_set_fps(fps_cvt, rc->video_fps) == ESP_GMF_ERR_OK) {
            ESP_LOGI(TAG, "fps_cvt: sensore %u fps → sink %u fps",
                     (unsigned)sensor_fps, (unsigned)rc->video_fps);
        }
    }

    rec_apply_ppa_aspect_crop(rc->sink, rc->video_width, rc->video_height);

    esp_gmf_element_handle_t venc = nullptr;
    esp_capture_err_t ret = esp_capture_sink_get_element_by_tag(
        rc->sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc);
    if (ret != ESP_CAPTURE_ERR_OK || !venc) {
        ESP_LOGW(TAG, "vid_enc non trovato — GOP/QP default");
        return ESP_CAPTURE_ERR_OK;
    }
    const bool fullhd = CORE_VIDEO_IS_FULLHD(rc->video_width, rc->video_height);
    const uint32_t qp_min = fullhd ? CORE_VIDEO_QP_1080_MIN : CORE_VIDEO_QP_MIN;
    const uint32_t qp_max = fullhd ? CORE_VIDEO_QP_1080_MAX : CORE_VIDEO_QP_MAX;
    esp_gmf_video_enc_set_bitrate(venc, rc->video_bitrate);
    esp_gmf_video_enc_set_gop(venc, rc->video_gop);
    esp_gmf_video_enc_set_qp(venc, qp_min, qp_max);
    ESP_LOGI(TAG, "Encoder H264: %u bps, GOP=%u, QP [%u-%u]",
             (unsigned)rc->video_bitrate, (unsigned)rc->video_gop,
             (unsigned)qp_min, (unsigned)qp_max);
    return ESP_CAPTURE_ERR_OK;
}

static void recorder_capture_cleanup(recorder_capture_t *rc) {
    if (!rc) {
        return;
    }
#if CORE_REC_OSD_ENABLE
    recorder_osd_close(&rc->overlay);
#endif
    if (rc->capture) {
        esp_capture_close(rc->capture);
        rc->capture = nullptr;
    }
    if (rc->vsrc) {
        free(rc->vsrc);
        rc->vsrc = nullptr;
    }
    esp_video_enc_unregister_default();
    rc->running = false;
}

static uint16_t s_last_enc_w;
static uint16_t s_last_enc_h;
static uint8_t s_last_enc_fps;
static char s_last_enc_err[96];

static void rec_note_encode(uint16_t w, uint16_t h, uint8_t fps, const char *note) {
    s_last_enc_w = w;
    s_last_enc_h = h;
    s_last_enc_fps = fps;
    FILE *fp = fopen("/tmp/mrbin-rec-pipeline.log", "w");
    if (fp) {
        fprintf(fp, "encode=%ux%u@%u note=%s\n", (unsigned)w, (unsigned)h, (unsigned)fps,
                note ? note : "");
        fclose(fp);
    }
}

/* Luckfox SC3336: selected size goes to VI+VENC. Never silently swap in 1280×960. */
static void rec_apply_selected_encode_size(uint16_t *w, uint16_t *h, uint8_t *fps) {
    if (!w || !h) {
        return;
    }
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_UNKNOWN) {
        core_video_detect_sensor();
    }
    const uint16_t want_w = *w;
    const uint16_t want_h = *h;
    const uint8_t want_fps = fps ? *fps : 0;
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708) {
        uint16_t ew = *w;
        uint16_t eh = *h;
        core_video_imx708_encode_size(*w, *h, &ew, &eh);
        ESP_LOGI(TAG, "Encode IMX708 %ux%u → %ux%u",
                 (unsigned)*w, (unsigned)*h, (unsigned)ew, (unsigned)eh);
        *w = ew;
        *h = eh;
        rec_note_encode(*w, *h, want_fps, "IMX708");
        return;
    }
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX415) {
        if (*w > CORE_VIDEO_WIDTH_MAX) {
            *w = (uint16_t)CORE_VIDEO_WIDTH_MAX;
        }
        if (*h > CORE_VIDEO_HEIGHT_MAX) {
            *h = (uint16_t)CORE_VIDEO_HEIGHT_MAX;
        }
        /* Sopra ~5 MP l'encode della Pico Zero è 8M@15, non 30. */
        if (fps && *fps > core_video_fps_ceiling(*w, *h)) {
            *fps = core_video_fps_ceiling(*w, *h);
        }
        ESP_LOGI(TAG, "Encode IMX415 %ux%u@%u",
                 (unsigned)*w, (unsigned)*h, (unsigned)(fps ? *fps : want_fps));
        rec_note_encode(*w, *h, fps ? *fps : want_fps, "IMX415");
        return;
    }
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_OV5647) {
        if (*w > 1280 || *h > 960) {
            ESP_LOGW(TAG, "OV5647: %ux%u non nativo → 1280×960 (solo questo sensore)",
                     (unsigned)*w, (unsigned)*h);
            *w = 1280;
            *h = 960;
        }
        rec_note_encode(*w, *h, want_fps, "OV5647");
        return;
    }
    if (*w > CORE_VIDEO_WIDTH_MAX) {
        *w = (uint16_t)CORE_VIDEO_WIDTH_MAX;
    }
    if (*h > CORE_VIDEO_HEIGHT_MAX) {
        *h = (uint16_t)CORE_VIDEO_HEIGHT_MAX;
    }
    if (fps && *fps > CORE_VIDEO_FPS_MAX) {
        *fps = (uint8_t)CORE_VIDEO_FPS_MAX;
    }
    if (want_w != *w || want_h != *h) {
        ESP_LOGW(TAG, "SC3336: %ux%u oltre il max HW → %ux%u (non 1280×960)",
                 (unsigned)want_w, (unsigned)want_h, (unsigned)*w, (unsigned)*h);
        rec_note_encode(*w, *h, fps ? *fps : want_fps, "SC3336 capped at sensor/VENC max");
    } else {
        ESP_LOGI(TAG, "Encode SC3336 %ux%u@%u (rkisp + VENC, selected size, no 1280x960 fallback)",
                 (unsigned)*w, (unsigned)*h, (unsigned)(fps ? *fps : want_fps));
        s_last_enc_err[0] = 0;
        rec_note_encode(*w, *h, fps ? *fps : want_fps, "SC3336 selected");
    }
}

static bool recorder_capture_adopt(recorder_capture_t *rc) {
    esp_video_enc_register_default();
    (void)esp_gmf_video_enc_set_gop(nullptr, rc->video_gop);
    if (esp_capture_retarget_h264((int)rc->video_width, (int)rc->video_height,
                                  (int)rc->video_fps, rc->video_bitrate, rc->video_gop) != 0) {
        core_live_abandon_handoff();
        esp_video_enc_unregister_default();
        snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                 "VENC swap %ux%u@%u failed",
                 (unsigned)rc->video_width, (unsigned)rc->video_height,
                 (unsigned)rc->video_fps);
        return false;
    }
    rc->capture = esp_capture_active_handle();
    rc->sink = rc->capture;
    rc->vsrc = (esp_capture_video_src_if_t *)calloc(1, sizeof(esp_capture_video_src_if_t));
    rc->running = true;
    rc->from_live = true;
    rc->warmup_left = 0;
    core_snap("venc h264");
    ESP_LOGI(TAG, "Handoff H264 %ux%u@%u GOP=%u %u bps — VI non riaperto",
             (unsigned)rc->video_width, (unsigned)rc->video_height,
             (unsigned)rc->video_fps, (unsigned)rc->video_gop,
             (unsigned)rc->video_bitrate);
    return rc->capture != nullptr;
}

static bool recorder_capture_from_rtsp(recorder_capture_t *rc, const core_settings_t *settings) {
    memset(rc, 0, sizeof(*rc));
    core_settings_t vcfg = settings ? *settings : core_settings_t{};
    if (!settings) {
        core_settings_video_set_defaults(&vcfg);
    } else {
        core_settings_video_normalize(&vcfg);
    }
    rc->video_width = vcfg.video_width;
    rc->video_height = vcfg.video_height;
    rc->video_fps = vcfg.video_fps;
    rec_apply_selected_encode_size(&rc->video_width, &rc->video_height, &rc->video_fps);
    vcfg.video_width = rc->video_width;
    vcfg.video_height = rc->video_height;
    vcfg.video_fps = rc->video_fps;
    rc->frame_dur_ms = core_settings_video_frame_ms(&vcfg);
    rc->video_bitrate = core_settings_video_bitrate(&vcfg);
    rc->video_gop = core_settings_video_gop(&vcfg);
    if (rc->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
        rc->video_gop = 1;
    }
    rc->from_rtsp = true;
    rc->running = true;
    rc->warmup_left = 0;
    ESP_LOGI(TAG, "Registrazione sullo streamer H.264 %ux%u@%u",
             (unsigned)rc->video_width, (unsigned)rc->video_height, (unsigned)rc->video_fps);
    return true;
}

static bool recorder_capture_start_psram(recorder_capture_t *rc, uint32_t warmup_frames,
                                         const core_settings_t *settings, bool pir_fast,
                                         bool adopt_vi) {
    memset(rc, 0, sizeof(*rc));
    rc->warmup_left = warmup_frames;
    core_settings_t vcfg = settings ? *settings : core_settings_t{};
    if (!settings) {
        core_settings_video_set_defaults(&vcfg);
    } else {
        core_settings_video_normalize(&vcfg);
    }
    rc->video_width = vcfg.video_width;
    rc->video_height = vcfg.video_height;
    rc->video_fps = vcfg.video_fps;
    rec_apply_selected_encode_size(&rc->video_width, &rc->video_height, &rc->video_fps);
    vcfg.video_width = rc->video_width;
    vcfg.video_height = rc->video_height;
    vcfg.video_fps = rc->video_fps;
    rc->frame_dur_ms = core_settings_video_frame_ms(&vcfg);
    rc->video_bitrate = core_settings_video_bitrate(&vcfg);
    rc->video_gop = core_settings_video_gop(&vcfg);
    if (pir_fast && rc->video_gop > CORE_REC_PIR_GOP) {
        rc->video_gop = CORE_REC_PIR_GOP;
    }
    /* GOP=1 only for 1–3 fps timelapse (software keep + drop P-frames).
     * Forcing it on 25/30 fps made every frame IDR and, when HAL GOP stayed
     * 24, surplus-drop + need_idr_resync kept ~1 IDR/GOP (~3 fps). */
    if (rc->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
        rc->video_gop = 1;
    }
    if (rc->video_fps <= CORE_VIDEO_FPS_LOW_MAX &&
        rc->warmup_left < CORE_REC_WARMUP_DROP_FRAMES_TL) {
        rc->warmup_left = CORE_REC_WARMUP_DROP_FRAMES_TL;
    }

    if (adopt_vi) {
        if (recorder_capture_adopt(rc)) {
            return true;
        }
        ESP_LOGW(TAG, "Swap VENC fallito — riapro VI da spento");
        adopt_vi = false;
    }

    /* PIR uses RKMPI on rkisp_mainpath. Opening /dev/video0 first only
     * delays EnableChn. CONFIG/web still probes the CIF node. */
    if (!core_rec_fast_requested() && !core_video_init()) {
        ESP_LOGW(TAG, "core_video_init fallita — retry dopo deinit");
        core_video_deinit();
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!core_video_init()) {
            ESP_LOGE(TAG, "core_video_init fallita");
            return false;
        }
    }

    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708) {
        const bool keep_720 = (vcfg.video_width == 1280 && vcfg.video_height == 720);
        const bool keep_768 = (vcfg.video_width == 1024 && vcfg.video_height == 768);
        if (!keep_720 && !keep_768 &&
            (vcfg.video_width < 1920 || vcfg.video_height < 1080 ||
             rc->video_width < 1800)) {
            vcfg.video_width = 1920;
            vcfg.video_height = 1080;
            uint16_t ew = CORE_IMX708_CLEAN_W;
            uint16_t eh = CORE_IMX708_CLEAN_H;
            core_video_imx708_encode_size(1920, 1080, &ew, &eh);
            rc->video_width = ew;
            rc->video_height = eh;
            vcfg.video_width = ew;
            vcfg.video_height = eh;
        }
        if (rc->video_fps < 20) {
            rc->video_fps = 28;
            vcfg.video_fps = 28;
        }
        rc->frame_dur_ms = core_settings_video_frame_ms(&vcfg);
        rc->video_bitrate = core_settings_video_bitrate(&vcfg);
        rc->video_gop = 1;
        ESP_LOGI(TAG, "IMX708 rec: %ux%u @ %u fps, %u bps",
                 (unsigned)rc->video_width, (unsigned)rc->video_height,
                 (unsigned)rc->video_fps, (unsigned)rc->video_bitrate);
    }

    if (!core_video_prepare_for_settings(&vcfg)) {
        ESP_LOGW(TAG, "Cambio formato sensore fallito — continuo con modalità corrente");
    }

    // Dopo S_SENSOR_FMT (come in live prepare_for_stream): riapplica profilo/focus.
    // Su AF il VCM deve assestarsi prima dello stream, altrimenti i primi secondi sono neri.
    // PIR e DEMO bloccano l'esposizione con mrbin-isp pir-arm / pir-lock.
    if (!core_rec_fast_requested() && !core_demo_is_active()) {
        (void)core_isp_cal_apply_active();
    }
    if (core_video_module_is_af()) {
        vTaskDelay(pdMS_TO_TICKS(CORE_REC_VCM_SETTLE_MS_AF));
    }

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = CORE_V4L2_BUF_COUNT,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!vsrc) {
        ESP_LOGE(TAG, "esp_capture_new_video_v4l2_src fallita");
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = vsrc,
        .share_overlay = false,
    };

    esp_capture_handle_t capture = nullptr;
    esp_capture_err_t err = esp_capture_open(&cap_cfg, &capture);
    if (err != ESP_CAPTURE_ERR_OK || capture == nullptr) {
        ESP_LOGE(TAG, "esp_capture_open fallita: %d", (int)err);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_H264,
            .width = rc->video_width,
            .height = rc->video_height,
            .fps = rc->video_fps,
        },
    };

    esp_capture_sink_handle_t sink = nullptr;
    err = esp_capture_sink_setup(capture, 0, &sink_cfg, &sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita: %d (%dx%d@%d)",
                 (int)err, rc->video_width, rc->video_height, rc->video_fps);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_set_bitrate(sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, rc->video_bitrate);

    rc->capture = capture;
    rc->sink = sink;
    rc->vsrc = vsrc;

    err = esp_capture_set_event_cb(capture, rec_capture_pipeline_event, rc);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "esp_capture_set_event_cb fallita: %d", (int)err);
    }

    esp_capture_sink_enable(sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    /* HAL CreateChn reads s_cap.gop. Pipeline-built never finds vid_enc on Luckfox. */
    (void)esp_gmf_video_enc_set_gop(nullptr, rc->video_gop);

#if CORE_REC_OSD_ENABLE
    (void)recorder_osd_attach(sink, settings, rc->video_width, rc->video_height,
                              &rc->overlay, &rc->osd_w, &rc->osd_h, &rc->osd_font);
    if (rc->overlay) {
        recorder_osd_refresh(rc->overlay, rc->osd_w, rc->osd_h, rc->osd_font, &rc->osd_last_us);
    }
#endif

    err = esp_capture_start(capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start fallita: %d — richiesto %ux%u@%u (nessun fallback 1280x960)",
                 (int)err, (unsigned)rc->video_width, (unsigned)rc->video_height,
                 (unsigned)rc->video_fps);
        const char *mpi_err = esp_capture_last_mpi_error();
        if (mpi_err && mpi_err[0]) {
            snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                     "VI+VENC %ux%u@%u failed: %s",
                     (unsigned)rc->video_width, (unsigned)rc->video_height,
                     (unsigned)rc->video_fps, mpi_err);
        } else {
            snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                     "VI+VENC %ux%u@%u failed (no 1280x960 fallback)",
                     (unsigned)rc->video_width, (unsigned)rc->video_height,
                     (unsigned)rc->video_fps);
        }
        rec_note_encode(rc->video_width, rc->video_height, rc->video_fps, s_last_enc_err);
#if CORE_REC_OSD_ENABLE
        recorder_osd_close(&rc->overlay);
#endif
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    core_video_reapply_flip();

    rc->running = true;

    uint16_t sw = 0;
    uint16_t sh = 0;
    uint8_t sfps = 0;
    (void)core_video_get_sensor_size(&sw, &sh);
    (void)core_video_get_sensor_fps(&sfps);
    ESP_LOGI(TAG, "Capture H264 %dx%d@%d avviato (sensore %ux%u@%u, GOP=%u, %u bps)",
             rc->video_width, rc->video_height, rc->video_fps,
             (unsigned)sw, (unsigned)sh, (unsigned)sfps,
             (unsigned)rc->video_gop, (unsigned)rc->video_bitrate);
    return true;
}

static void recorder_capture_halt(recorder_capture_t *rc) {
    if (!rc || !rc->running) {
        return;
    }
    if (rc->from_rtsp) {
        rc->running = false;
        return;
    }
    if (!rc->capture) {
        return;
    }
    esp_capture_stop(rc->capture);
    rc->running = false;
}

static void recorder_capture_stop(recorder_capture_t *rc) {
    if (!rc) {
        return;
    }
    if (rc->from_rtsp) {
        free(rc->rtsp_frame);
        rc->rtsp_frame = nullptr;
        rc->running = false;
        return;
    }
    if (!rc->capture) {
        return;
    }
    if (rc->running) {
        esp_capture_stop(rc->capture);
    }
    recorder_capture_cleanup(rc);
}

// Attende il primo frame H264 dalla pipeline. Serve a due cose:
// 1. l'encoder HW e il PPA allocano i buffer di lavoro solo al primo frame
//    (~3-6 MB in PSRAM a seconda della risoluzione): vanno allocati PRIMA
//    di dimensionare il ring, che si prende il 95% della PSRAM residua;
// 2. se l'encoder muore (OOM o errore pipeline), lo scopriamo subito invece
//    di registrare 0 frame per tutta la sessione.
static bool recorder_capture_prime(recorder_capture_t *rc, uint32_t timeout_ms) {
    if (rc && rc->from_rtsp) {
        int64_t until = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
        while (esp_timer_get_time() < until) {
            esp_capture_stream_frame_t frame = {};
            if (rec_acquire_any(rc, &frame, false)) {
                rec_release_got(rc, &frame);
                hal_capture_request_idr();
                ESP_LOGI(TAG, "Pipeline H264 condivisa pronta — stesso flusso dello streamer");
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGE(TAG, "Streamer senza frame entro %lu ms", (unsigned long)timeout_ms);
        return false;
    }
    int64_t until = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < until) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(rc->sink, &frame, true);
        if (err == ESP_CAPTURE_ERR_OK && frame.data && frame.size > 0) {
            core_note_first_encoded_frame("prime", (unsigned long)frame.size);
            if (rc->from_live) {
                /* Keep this IDR. The next acquire replays it into the file
                 * so the GOP is not stuck waiting for the following IDR. */
                esp_capture_replay_next(frame.data, frame.size, frame.pts);
            }
            esp_capture_sink_release_frame(rc->sink, &frame);
            ESP_LOGI(TAG, "Pipeline H264 pronta (primo frame %lu byte) — buffer encoder allocati",
                     (unsigned long)frame.size);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "Pipeline H264 non produce frame entro %lu ms (encoder OOM o pipeline morta)",
             (unsigned long)timeout_ms);
    return false;
}

// Scarta frame H264 per duration_ms (AE/ISP converge; encoder alloca al 1° frame).
static bool recorder_capture_discard_for_ms(recorder_capture_t *rc, uint32_t duration_ms) {
    if (!rc || !rc->running || duration_ms == 0) {
        return false;
    }
    int64_t until = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint32_t count = 0;
    while (esp_timer_get_time() < until) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(rc->sink, &frame, true);
        if (err == ESP_CAPTURE_ERR_OK && frame.data && frame.size > 0) {
            esp_capture_sink_release_frame(rc->sink, &frame);
            count++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    ESP_LOGI(TAG, "Warm-up pipeline: %lu frame scartati in %lu ms (AE, non registrati)",
             (unsigned long)count, (unsigned long)duration_ms);
    return count > 0;
}

static bool recorder_muxer_start_from_buffer(recorder_muxer_t *muxer, const char *tmp_path,
                                             core_h264_buffer_t *buf, uint32_t *out_replayed,
                                             uint16_t width, uint16_t height, uint8_t fps,
                                             bool moov_before_mdat) {
    if (out_replayed) {
        *out_replayed = 0;
    }
    if (!muxer || !tmp_path || !buf || buf->frame_count == 0) {
        return false;
    }

    uint8_t codec_spec[H264_CODEC_SPEC_MAX];
    int spec_len = 0;
    if (!core_h264_buffer_extract_h264_config(buf, codec_spec, sizeof(codec_spec), &spec_len)) {
        ESP_LOGW(TAG, "SPS/PPS non trovati nel buffer — attendo altri frame");
        return false;
    }

    mp4_muxer_register();

    static recorder_muxer_ctx_t mux_ctx;
    snprintf(mux_ctx.path, sizeof(mux_ctx.path), "%s", tmp_path);

    const bool use_enc = recorder_mux_use_encrypt();
    mp4_muxer_config_t mp4_cfg = {
        .base_config = {
            .muxer_type = ESP_MUXER_TYPE_MP4,
            .slice_duration = 3600000,
            .url_pattern = nullptr,
            .url_pattern_ex = storage_path_handler,
            .ctx = &mux_ctx,
            .ram_cache_size = use_enc ? CORE_MUXER_RAM_CACHE_ENC_BYTES : CORE_MUXER_RAM_CACHE_PLAIN_BYTES,
            .no_key_frame_verify = true,
        },
        .display_in_order = true,
        .moov_before_mdat = moov_before_mdat,
    };

    esp_muxer_handle_t handle = esp_muxer_open((esp_muxer_config_t *)&mp4_cfg, sizeof(mp4_cfg));
    if (!handle) {
        ESP_LOGE(TAG, "esp_muxer_open fallita");
        return false;
    }

    esp_muxer_file_writer_t writer;
    if (use_enc) {
        writer = (esp_muxer_file_writer_t){
            .on_open  = enc_writer_open,
            .on_write = enc_writer_write,
            .on_seek  = enc_writer_seek,
            .on_close = enc_writer_close,
        };
    } else {
        writer = (esp_muxer_file_writer_t){
            .on_open  = plain_writer_open,
            .on_write = plain_writer_write,
            .on_seek  = plain_writer_seek,
            .on_close = plain_writer_close,
        };
    }
    if (esp_muxer_set_file_writer(handle, &writer) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        ESP_LOGE(TAG, "esp_muxer_set_file_writer fallita");
        return false;
    }

    esp_muxer_video_stream_info_t vinfo = {
        .codec = ESP_MUXER_VDEC_H264,
        .width = width,
        .height = height,
        .fps = fps,
        .min_packet_duration = fps > 0 ? (uint32_t)(1000 / fps) : 83U,
        .codec_spec_info = codec_spec,
        .spec_info_len = spec_len,
    };

    int stream_index = -1;
    if (esp_muxer_add_video_stream(handle, &vinfo, &stream_index) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        ESP_LOGE(TAG, "esp_muxer_add_video_stream fallita");
        return false;
    }

    muxer->handle = handle;
    muxer->stream_index = stream_index;
    muxer->active = true;
    ESP_LOGI(TAG, "Mux MP4 su SD: %s (moov_%s, buffer %lu KB, ~%u s)",
             tmp_path, moov_before_mdat ? "head" : "tail",
             (unsigned long)(core_h264_buffer_bytes_used(buf) / 1024),
             (unsigned)core_h264_buffer_estimated_sec(buf));
    return true;
}

static void recorder_discard_psram_until_idr(core_h264_buffer_t *buf) {
    if (!buf) {
        return;
    }
    while (buf->head &&
           !core_h264_is_idr_nal(core_h264_frame_data(buf, buf->head), buf->head->size)) {
        core_h264_buffer_pop_head(buf);
    }
}

static uint32_t recorder_drain_psram_to_muxer(recorder_muxer_t *muxer, core_h264_buffer_t *buf,
                                              recorder_should_stop_fn_t should_stop) {
    if (!muxer || !muxer->active || !buf) {
        return 0;
    }
    uint32_t drained = 0;
    while (buf->head) {
        if (should_stop && should_stop()) {
            break;
        }
        core_h264_frame_t *f = buf->head;
        esp_muxer_video_packet_t pkt = {
            .data = (uint8_t *)core_h264_frame_data(buf, f),
            .len = (int)f->size,
            .pts = f->pts_ms,
            .dts = f->pts_ms,
            .key_frame = core_h264_is_idr_nal(core_h264_frame_data(buf, f), f->size),
        };
        if (esp_muxer_add_video_packet(muxer->handle, muxer->stream_index, &pkt) != ESP_MUXER_ERR_OK) {
            break;
        }
        core_h264_buffer_pop_head(buf);
        drained++;
    }
    return drained;
}

static void recorder_muxer_close(recorder_muxer_t *muxer) {
    if (muxer && muxer->active && muxer->handle) {
        esp_muxer_close(muxer->handle);
        muxer->handle = nullptr;
        muxer->active = false;
    }
}

static bool recorder_ensure_muxer_open(recorder_muxer_t *muxer, const char *path,
                                       core_h264_buffer_t *psram_buf,
                                       const recorder_capture_t *capture,
                                       bool moov_before_mdat);

static bool recorder_muxer_write_packet(recorder_muxer_t *muxer, const uint8_t *data, uint32_t size,
                                        uint32_t pts_ms, uint32_t *muxer_frames) {
    if (!muxer || !muxer->active || !data || size == 0) {
        return false;
    }
    esp_muxer_video_packet_t pkt = {
        .data = (uint8_t *)data,
        .len = (int)size,
        .pts = pts_ms,
        .dts = pts_ms,
        .key_frame = core_h264_is_idr_nal(data, size),
    };
    esp_muxer_err_t err = esp_muxer_add_video_packet(muxer->handle, muxer->stream_index, &pkt);
    if (err != ESP_MUXER_ERR_OK) {
        static int64_t s_last_mux_err_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_mux_err_us >= 1000000) {
            s_last_mux_err_us = now;
            ESP_LOGW(TAG, "esp_muxer_add_video_packet fallita: err=%d pts=%lu size=%lu",
                     (int)err, (unsigned long)pts_ms, (unsigned long)size);
        }
        return false;
    }
    if (muxer_frames) {
        (*muxer_frames)++;
    }
    return true;
}

// Apre il muxer e svuota lo staging (plain direct). Ritorna true se muxer attivo.
static bool recorder_try_open_muxer_from_staging(recorder_muxer_t *muxer, const sd_prep_ctx_t *sd_ctx,
                                                 core_h264_buffer_t *staging,
                                                 const recorder_capture_t *capture,
                                                 bool *muxer_was_active, uint32_t *muxer_frames,
                                                 bool moov_before_mdat) {
    if (!muxer || muxer->active || !sd_ctx || !staging || staging->frame_count == 0) {
        return muxer && muxer->active;
    }
    if (!core_h264_buffer_has_idr(staging)) {
        return false;
    }
    recorder_discard_psram_until_idr(staging);
    if (!staging->head) {
        return false;
    }
    char mux_tmp[160];
    rec_mux_temp_path(mux_tmp, sizeof(mux_tmp), sd_ctx->final_path);
    if (!recorder_ensure_muxer_open(muxer, mux_tmp, staging, capture, moov_before_mdat)) {
        static int64_t s_last_open_fail_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_open_fail_us >= 2000000) {
            s_last_open_fail_us = now;
            ESP_LOGW(TAG, "Muxer non pronto (SPS/PPS/IDR, staging %lu frame)",
                     (unsigned long)staging->frame_count);
        }
        return false;
    }
    if (muxer_was_active) {
        *muxer_was_active = true;
    }
    uint32_t n = recorder_drain_psram_to_muxer(muxer, staging, nullptr);
    if (muxer_frames) {
        *muxer_frames += n;
    }
    ESP_LOGI(TAG, "Mux plain direct aperto (%lu frame da staging)", (unsigned long)n);
    return true;
}

// Pre-muxer: pipeline capture può riempire staging prima dell'IDR — evict i P-frame più vecchi.
static bool recorder_staging_push_pre_mux(core_h264_buffer_t *staging, uint32_t pts_ms,
                                          const uint8_t *data, uint32_t size,
                                          uint32_t *dropped_frames) {
    if (!staging || !data || size == 0) {
        return false;
    }
    const bool incoming_idr = core_h264_is_idr_nal(data, size);
    for (uint32_t attempt = 0; attempt < 64; attempt++) {
        if (core_h264_buffer_push(staging, pts_ms, data, size)) {
            return true;
        }
        if (!staging->head) {
            break;
        }
        const uint8_t *head_data = core_h264_frame_data(staging, staging->head);
        const bool head_idr = core_h264_is_idr_nal(head_data, staging->head->size);
        if (incoming_idr || !head_idr) {
            core_h264_buffer_pop_head(staging);
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            continue;
        }
        break;
    }
    if (dropped_frames) {
        (*dropped_frames)++;
    }
    return false;
}

static bool recorder_stream_frame_to_muxer(recorder_muxer_t *muxer, const sd_prep_ctx_t *sd_ctx,
                                             bool sd_ready, core_h264_buffer_t *staging,
                                             const recorder_capture_t *capture,
                                             bool *muxer_was_active, uint32_t *muxer_frames,
                                             uint32_t *dropped_frames, const uint8_t *data,
                                             uint32_t size, uint32_t pts_ms, bool moov_before_mdat) {
    if (!sd_ready || !sd_ctx->paths_ok) {
        if (!recorder_staging_push_pre_mux(staging, pts_ms, data, size, dropped_frames)) {
            ESP_LOGW(TAG, "Staging pieno (SD non pronta) — frame perso");
            return false;
        }
        return true;
    }

    if (!muxer->active) {
        (void)recorder_try_open_muxer_from_staging(muxer, sd_ctx, staging, capture, muxer_was_active,
                                                   muxer_frames, moov_before_mdat);
        if (muxer->active) {
            return recorder_muxer_write_packet(muxer, data, size, pts_ms, muxer_frames);
        }
        if (!recorder_staging_push_pre_mux(staging, pts_ms, data, size, dropped_frames)) {
            if (recorder_try_open_muxer_from_staging(muxer, sd_ctx, staging, capture, muxer_was_active,
                                                     muxer_frames, moov_before_mdat)) {
                return recorder_muxer_write_packet(muxer, data, size, pts_ms, muxer_frames);
            }
            return false;
        }
        if (recorder_try_open_muxer_from_staging(muxer, sd_ctx, staging, capture, muxer_was_active,
                                                 muxer_frames, moov_before_mdat)) {
            return true;
        }
        return true;
    }

    if (!recorder_muxer_write_packet(muxer, data, size, pts_ms, muxer_frames)) {
        if (!core_h264_buffer_push(staging, pts_ms, data, size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            ESP_LOGW(TAG, "Scrittura SD fallita e staging pieno — frame perso");
        }
        return false;
    }

    if (staging->frame_count > 0) {
        uint32_t n = recorder_drain_psram_to_muxer(muxer, staging, nullptr);
        if (muxer_frames) {
            *muxer_frames += n;
        }
    }
    return true;
}

static bool recorder_process_frame_stream(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                          const sd_prep_ctx_t *sd_ctx, bool sd_ready,
                                          core_h264_buffer_t *staging, bool *muxer_was_active,
                                          uint32_t *muxer_frames, uint32_t *dropped_frames,
                                          bool no_wait) {
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, no_wait);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    uint32_t pts_ms = 0;
    if (!recorder_frame_accept(capture, frame.pts, &pts_ms)) {
        rec_release_got(capture, &frame);
        return true;
    }

    (void)recorder_stream_frame_to_muxer(muxer, sd_ctx, sd_ready, staging, capture,
                                         muxer_was_active, muxer_frames, dropped_frames,
                                         frame.data, (uint32_t)frame.size, pts_ms, true);
    rec_release_got(capture, &frame);
    return true;
}

static void recorder_drain_capture_to_muxer(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                            const sd_prep_ctx_t *sd_ctx, bool sd_ready,
                                            core_h264_buffer_t *staging, bool *muxer_was_active,
                                            uint32_t *muxer_frames, uint32_t *dropped_frames) {
    (void)capture;
    (void)muxer;
    (void)sd_ctx;
    (void)sd_ready;
    (void)staging;
    (void)muxer_was_active;
    (void)muxer_frames;
    (void)dropped_frames;
}

typedef struct rec_dual_job_ctx {
    recorder_capture_t       *capture;
    recorder_muxer_t         *muxer;
    sd_prep_ctx_t            *sd_ctx;
    core_h264_buffer_t       *ring;
    SemaphoreHandle_t         ring_mtx;
    recorder_should_stop_fn_t should_stop;
    volatile bool             capture_done;
    volatile bool             sd_done;
    volatile bool             sd_prep_done;
    volatile bool             user_stop;   // stop D2/web: poi drain encoder→ring, flush SD
    bool                     *muxer_was_active;
    volatile uint32_t         muxer_frames;
    volatile uint32_t         dropped_frames;
    volatile uint32_t         producer_pts_max;
    int64_t                   last_ring_warn_us;
    bool                      pir_fast;
    bool                      rec_encrypt;   // enc_writer se true, plain_writer se false
    int64_t                   ae_discard_until_us;  // PIR: scarta frame in JOB1 fino a questo istante
    uint32_t                  sd_lag_ms;
    uint32_t                  sd_burst;
    uint32_t                  segment_max_sec;
    uint32_t                  segment_start_pts_ms;   // legacy / log
    uint32_t                  segment_pts_base;       // PTS rebased a 0 per segmento MP4
    int64_t                   segment_start_wall_us;  // durata segmento su wall clock
    volatile bool             segment_closing;        // chiusura muxer: JOB1 scarta frame
    volatile uint32_t         segments_closed;
    volatile uint32_t         sd_write_fails;         // scritture muxer fallite (SD morta?)
    uint8_t                  *sd_frame_copy;          // scratch JOB2: copia NAL sotto mutex ring
    uint32_t                  sd_frame_copy_cap;
    bool                      ignore_gpio_stop;   // timed mode: STOP ignorato
    uint32_t                  session_limit_sec;  // 0 = solo GPIO stop
    int64_t                   session_start_us;
    const core_settings_t    *settings;
    FILE                     *h264_fp;
    uint32_t                  h264_pts_first;
    uint32_t                  h264_pts_last;
    uint32_t                  h264_frames;
    volatile bool             muxer_closed_on_sd;
} rec_dual_job_ctx_t;

#define REC_SD_WRITE_FAILS_MAX  3   // oltre: SD considerata inutilizzabile, niente retry

static bool recorder_mux_use_encrypt(void) {
#if CORE_REC_PLAIN_SD_BENCHMARK
    return false;
#else
    if (s_active_rec_job) {
        return s_active_rec_job->rec_encrypt;
    }
    return s_session_rec_encrypt;
#endif
}

static uint32_t rec_muxer_pts(const rec_dual_job_ctx_t *ctx, uint32_t abs_pts_ms) {
    if (!ctx) {
        return abs_pts_ms;
    }
    if (abs_pts_ms >= ctx->segment_pts_base) {
        return abs_pts_ms - ctx->segment_pts_base;
    }
    return 0;
}

static bool rec_session_stop_requested(const rec_dual_job_ctx_t *ctx) {
    return ctx && (ctx->user_stop || ctx->should_stop() || ctx->capture_done);
}

static uint32_t rec_ring_fill_pct(rec_dual_job_ctx_t *ctx) {
    uint32_t pct = 0;
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    pct = core_h264_buffer_fill_percent(ctx->ring);
    xSemaphoreGive(ctx->ring_mtx);
    return pct;
}

static uint32_t rec_ring_pause_pct_for(const rec_dual_job_ctx_t *ctx) {
    if (!ctx || !ctx->pir_fast) {
        return CORE_RING_PAUSE_PCT;
    }
    return ctx->rec_encrypt ? CORE_RING_PAUSE_PCT_PIR_ENC : CORE_RING_PAUSE_PCT_PIR_PLAIN;
}

static uint32_t rec_sd_lag_catchup_pct_for(const rec_dual_job_ctx_t *ctx) {
    if (!ctx || !ctx->pir_fast) {
        return CORE_REC_SD_LAG_CATCHUP_PCT_ENC;
    }
    return ctx->rec_encrypt ? CORE_REC_SD_LAG_CATCHUP_PCT_ENC : CORE_REC_SD_LAG_CATCHUP_PCT_PLAIN;
}

static uint32_t rec_sd_lag_urgent_pct_for(const rec_dual_job_ctx_t *ctx) {
    if (!ctx || !ctx->pir_fast) {
        return CORE_REC_SD_LAG_URGENT_PCT_ENC;
    }
    return ctx->rec_encrypt ? CORE_REC_SD_LAG_URGENT_PCT_ENC : CORE_REC_SD_LAG_URGENT_PCT_PLAIN;
}

static uint32_t rec_sd_burst_limit(rec_dual_job_ctx_t *ctx) {
    if (ctx->should_stop() || ctx->user_stop) {
        return CORE_REC_SD_BURST_STOP;
    }
    if (ctx->rec_encrypt && ctx->pir_fast) {
        uint32_t pct = rec_ring_fill_pct(ctx);
        if (pct >= rec_sd_lag_urgent_pct_for(ctx)) {
            return CORE_REC_SD_BURST_FRAMES_PIR_ENC_URGENT;
        }
        if (pct >= rec_sd_lag_catchup_pct_for(ctx)) {
            return CORE_REC_SD_BURST_FRAMES_PIR_ENC;
        }
    }
    if (!ctx->rec_encrypt && ctx->pir_fast) {
        uint32_t pct = rec_ring_fill_pct(ctx);
        if (pct >= rec_sd_lag_catchup_pct_for(ctx)) {
            return CORE_REC_SD_BURST_STOP;
        }
    }
    return ctx->sd_burst;
}

static void rec_sd_job_yield_if_needed(rec_dual_job_ctx_t *ctx, uint32_t burst_index) {
    if (ctx && !ctx->rec_encrypt) {
        return;  // plain: burst continuo, SD non cifrata regge meglio
    }
    if ((burst_index & 63U) == 63U) {
        taskYIELD();
    }
}

static bool rec_capture_ring_paused(rec_dual_job_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    const uint32_t ring_pause_pct = rec_ring_pause_pct_for(ctx);
    return rec_ring_fill_pct(ctx) >= ring_pause_pct;
}

static bool rec_capture_push_ring(rec_dual_job_ctx_t *ctx, const uint8_t *data, uint32_t size,
                                  uint32_t pts_ms) {
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    bool ok = core_h264_buffer_push(ctx->ring, pts_ms, data, size);
    if (ok) {
        ctx->producer_pts_max = pts_ms;
    } else {
        ctx->dropped_frames++;
        int64_t now = esp_timer_get_time();
        if (now - ctx->last_ring_warn_us >= 1000000) {
            ctx->last_ring_warn_us = now;
            ESP_LOGW(TAG, "Ring PSRAM pieno (%u%%) — frame perso",
                     (unsigned)core_h264_buffer_fill_percent(ctx->ring));
        }
    }
    xSemaphoreGive(ctx->ring_mtx);
    return ok;
}

static void rec_capture_flush_sink(rec_dual_job_ctx_t *ctx) {
    if (!ctx || !ctx->capture || !ctx->capture->sink) {
        return;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < 64; i++) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        if (esp_capture_sink_acquire_frame(ctx->capture->sink, &frame, true) != ESP_CAPTURE_ERR_OK ||
            !frame.data) {
            break;
        }
        esp_capture_sink_release_frame(ctx->capture->sink, &frame);
        n++;
    }
    if (n > 0) {
        ESP_LOGI(TAG, "Coda encoder scartata dopo gap: %lu frame (resync IDR)",
                 (unsigned long)n);
    }
}

static bool rec_acquire_any(recorder_capture_t *rc, esp_capture_stream_frame_t *frame, bool no_wait) {
    if (!rc || !frame) {
        return false;
    }
    if (rc->from_rtsp) {
        uint8_t *p = nullptr;
        int n = 0;
        uint32_t pts = 0;
        int wait = no_wait ? 0 : 200;
        if (core_rtsp_rec_take(&p, &n, &pts, wait) != 0 || !p || n <= 0) {
            free(p);
            return false;
        }
        free(rc->rtsp_frame);
        rc->rtsp_frame = p;
        frame->stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO;
        frame->data = p;
        frame->size = n;
        frame->pts = pts;
        return true;
    }
    esp_capture_err_t err = esp_capture_sink_acquire_frame(rc->sink, frame, no_wait);
    return err == ESP_CAPTURE_ERR_OK && frame->data && frame->size > 0;
}

static void rec_release_got(recorder_capture_t *rc, esp_capture_stream_frame_t *frame) {
    if (!rc) {
        return;
    }
    if (rc->from_rtsp) {
        free(rc->rtsp_frame);
        rc->rtsp_frame = nullptr;
        if (frame) {
            frame->data = nullptr;
        }
        return;
    }
    if (frame) {
        esp_capture_sink_release_frame(rc->sink, frame);
    }
}

static bool rec_capture_one_frame(rec_dual_job_ctx_t *ctx, bool no_wait) {
    recorder_capture_t *capture = ctx->capture;
    if (!no_wait && rec_capture_ring_paused(ctx)) {
        return false;
    }

#if CORE_REC_OSD_ENABLE
    if (capture->overlay) {
        recorder_osd_refresh(capture->overlay, capture->osd_w, capture->osd_h,
                             capture->osd_font, &capture->osd_last_us);
    }
#endif

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    if (!rec_acquire_any(capture, &frame, no_wait)) {
        return false;
    }

    // Chiusura segmento / moov: svuota pipeline senza riempire il ring.
    if (ctx->segment_closing) {
        rec_release_got(capture, &frame);
        return true;
    }

    if (ctx->ae_discard_until_us > 0 &&
        esp_timer_get_time() < ctx->ae_discard_until_us) {
        rec_release_got(capture, &frame);
        return true;
    }

    if (recorder_capture_warmup(capture)) {
        rec_release_got(capture, &frame);
        return true;
    }

    // Tieni al massimo N fps (wall). Il sensore è 30/45: senza questo
    // (e con PTS CBR) 1 s OSD diventa 2–3 s di playback.
    {
        int64_t now_us = esp_timer_get_time();
        int64_t min_interval_us = capture->frame_dur_ms > 0
                                      ? ((int64_t)capture->frame_dur_ms * 1000)
                                      : (1000000LL / (int64_t)(capture->video_fps ? capture->video_fps : 10));
        if (capture->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
            min_interval_us = (min_interval_us * 7) / 10;
        }
        if (capture->last_kept_us > 0 &&
            (now_us - capture->last_kept_us) < min_interval_us) {
            /* Timelapse: dropped surplus must not join a broken GOP. 25/30:
             * keep every VENC NAL — dropping P-frames punches holes and
             * playback looks striped / blocky ("zigrinato"). pace_fps spaces
             * acquires; extra NALs stay in the file. */
            if (capture->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
                capture->need_idr_resync = true;
                rec_release_got(capture, &frame);
                return true;
            }
        }
        if (capture->video_fps <= CORE_VIDEO_FPS_LOW_MAX &&
            !core_h264_is_idr_nal(frame.data, (uint32_t)frame.size)) {
            rec_release_got(capture, &frame);
            return true;
        }
    }

    // Primo frame / dopo un gap: solo IDR. Un P-frame senza riferimento
    // (ring pieno, drop, coda encoder) fa saltare il decoder in playback.
    const bool is_idr = core_h264_is_idr_nal(frame.data, (uint32_t)frame.size);
    if (capture->last_kept_us > 0 && capture->frame_dur_ms > 0) {
        int64_t gap_us = esp_timer_get_time() - capture->last_kept_us;
        /* 2×frame was 66 ms at 30 fps. GetStream(200 ms) always exceeded that,
         * so every P-frame was dropped until the next IDR (~2 fps with GOP 30).
         * Timelapse still resyncs after a missed keep; 25/30 only after a real stall. */
        const int64_t resync_us = (capture->video_fps <= CORE_VIDEO_FPS_LOW_MAX)
                                      ? ((int64_t)capture->frame_dur_ms * 2000)
                                      : 500000;
        if (gap_us > resync_us) {
            capture->need_idr_resync = true;
        }
    }
    if (capture->kept_frames == 0 || capture->need_idr_resync) {
        if (!is_idr) {
            rec_release_got(capture, &frame);
            return true;
        }
        capture->need_idr_resync = false;
    }

    if (capture->kept_frames == 0) {
        core_note_first_encoded_frame("ring", (unsigned long)frame.size);
        ESP_LOGI(TAG, "Registrazione utile da PTS 0 a %lld ms dal boot",
                 (long long)core_boot_elapsed_ms());
    }
    uint32_t pts_ms = recorder_capture_assign_pts(capture, frame.pts);

    bool pushed = false;
    const bool stopping = ctx->user_stop || ctx->should_stop();
    int push_attempts = 0;
    while (true) {
            if (rec_capture_push_ring(ctx, frame.data, (uint32_t)frame.size, pts_ms)) {
            capture->kept_frames++;
            pushed = true;
            capture->last_kept_us = esp_timer_get_time();
            if (ctx->pir_fast && capture->kept_frames == 1) {
                core_status_led_set_mode(CORE_LED_RECORDING);
                ESP_LOGI(TAG, "PIR: 1° frame in ring a %lld ms dal boot",
                         (long long)core_boot_elapsed_ms());
            }
            break;
        }
        if (stopping) {
            if (++push_attempts >= 200) {
                ctx->dropped_frames++;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (no_wait) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(capture->frame_dur_ms / 4));
    }
    if (!pushed) {
        capture->need_idr_resync = true;
        if (no_wait && !stopping) {
            ctx->dropped_frames++;
        }
    }

    rec_release_got(capture, &frame);
    return true;
}

// Limita l'acquire al fps NVS: altrimenti la coda encoder si riempie al rate del
// sensore e allo stop/drain i PTS a raffica deformano la velocità.
static void rec_capture_pace_fps(const recorder_capture_t *capture) {
    if (!capture || capture->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
        return;
    }
    if (capture->last_kept_us <= 0 || capture->frame_dur_ms == 0) {
        return;
    }
    int64_t due_us = capture->last_kept_us + ((int64_t)capture->frame_dur_ms * 1000);
    int64_t now_us = esp_timer_get_time();
    if (due_us > now_us + 1000) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((due_us - now_us) / 1000)));
    }
}

static void rec_capture_job_task(void *arg) {
    rec_dual_job_ctx_t *ctx = (rec_dual_job_ctx_t *)arg;
    ESP_LOGI(TAG, "JOB1 capture→PSRAM (%dfps, sequenza H264 intatta)", ctx->capture->video_fps);
    bool was_paused = false;
    while (!ctx->should_stop() && !ctx->user_stop) {
        if (rec_capture_ring_paused(ctx)) {
            was_paused = true;
            if (ctx->should_stop() || ctx->user_stop) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ctx->capture->frame_dur_ms / 2));
            continue;
        }
        if (was_paused) {
            was_paused = false;
            ctx->capture->need_idr_resync = true;
            rec_capture_flush_sink(ctx);
        }
        rec_capture_pace_fps(ctx->capture);
        const bool stopping = ctx->should_stop() || ctx->user_stop;
        if (!rec_capture_one_frame(ctx, stopping)) {
            if (stopping) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Stop: svuota solo la coda V4L2+encoder già pronta (no_wait). Con acquire
    // bloccante si registravano altri N secondi live e i PTS a wall-clock
    // comprimevano i frame buffered → metà video lenta, metà veloce.
    if (ctx->user_stop || ctx->should_stop()) {
        const uint32_t drain_ms = ctx->pir_fast ? CORE_REC_STOP_CAPTURE_DRAIN_MS_PIR
                                                : CORE_REC_STOP_CAPTURE_DRAIN_MS;
        int64_t drain_until = esp_timer_get_time() + ((int64_t)drain_ms * 1000);
        uint32_t drained = 0;
        uint32_t idle = 0;
        // ~2 s senza frame: coda vuota (GOP/gap). Budget max = drain_ms.
        /* Frames keep arriving until DisableChn, so idle never used to
         * trip and every Stop burned the whole drain budget (~3.5 s).
         * Take what is already queued, then halt. */
        while (esp_timer_get_time() < drain_until && idle < 3) {
            if (rec_capture_one_frame(ctx, true)) {
                drained++;
                idle = 0;
            } else {
                idle++;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        ESP_LOGI(TAG, "Stop: drain coda → ring: %lu frame (%u ms budget%s)",
                 (unsigned long)drained, (unsigned)drain_ms,
                 ctx->pir_fast ? ", PIR" : "");
    }

    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    uint32_t ring_frames = ctx->ring->frame_count;
    xSemaphoreGive(ctx->ring_mtx);
    ESP_LOGI(TAG, "JOB1 capture terminato (%lu frame in ring, %lu in coda SD)",
             (unsigned long)ctx->capture->kept_frames, (unsigned long)ring_frames);
    ctx->capture_done = true;
    vTaskDelete(nullptr);
}

static bool rec_sd_ensure_frame_copy(rec_dual_job_ctx_t *ctx, uint32_t need) {
    if (!ctx || need == 0 || need > CORE_REC_SD_FRAME_COPY_MAX) {
        return false;
    }
    if (ctx->sd_frame_copy && ctx->sd_frame_copy_cap >= need) {
        return true;
    }
    if (ctx->sd_frame_copy) {
        heap_caps_free(ctx->sd_frame_copy);
        ctx->sd_frame_copy = nullptr;
        ctx->sd_frame_copy_cap = 0;
    }
    ctx->sd_frame_copy = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->sd_frame_copy) {
        ctx->sd_frame_copy = (uint8_t *)malloc(need);
    }
    if (!ctx->sd_frame_copy) {
        return false;
    }
    ctx->sd_frame_copy_cap = need;
    return true;
}

static void rec_sd_free_frame_copy(rec_dual_job_ctx_t *ctx) {
    if (!ctx || !ctx->sd_frame_copy) {
        return;
    }
    heap_caps_free(ctx->sd_frame_copy);
    ctx->sd_frame_copy = nullptr;
    ctx->sd_frame_copy_cap = 0;
}

static void rec_mrbin_open_h264(rec_dual_job_ctx_t *ctx, const char *mp4_path) {
    if (!ctx || !mp4_path) {
        return;
    }
    if (ctx->h264_fp) {
        fclose(ctx->h264_fp);
        ctx->h264_fp = nullptr;
    }
    ctx->h264_fp = core_clip_h264_open(mp4_path);
    ctx->h264_pts_first = 0;
    ctx->h264_pts_last = 0;
    ctx->h264_frames = 0;
    if (ctx->h264_fp) {
        ESP_LOGI(TAG, "Annex-B H264 parallelo: %s", mp4_path);
    }
}

static void rec_mrbin_finalize(rec_dual_job_ctx_t *ctx, const char *mp4_path) {
    if (!ctx) {
        return;
    }
    if (ctx->h264_fp) {
        fflush(ctx->h264_fp);
        fclose(ctx->h264_fp);
        ctx->h264_fp = nullptr;
    }
    if (!mp4_path || !mp4_path[0] || ctx->h264_frames == 0) {
        return;
    }
    core_clip_finish_t fin = {};
    snprintf(fin.mp4_path, sizeof(fin.mp4_path), "%s", mp4_path);
    if (ctx->capture) {
        fin.width = ctx->capture->video_width;
        fin.height = ctx->capture->video_height;
        fin.fps_target = ctx->capture->video_fps;
        fin.fps = recorder_capture_effective_fps(ctx->capture);
    }
    uint8_t sensor_fps = 0;
    (void)core_video_get_sensor_fps(&sensor_fps);
    fin.fps_source = sensor_fps;
    fin.fps_recorded = fin.fps;
    uint32_t span = 0;
    if (ctx->h264_pts_last > ctx->h264_pts_first) {
        span = ctx->h264_pts_last - ctx->h264_pts_first;
    }
    fin.duration_ms = span + (fin.fps > 0 ? (1000U / (uint32_t)fin.fps) : 0);
    if (ctx->settings) {
        fin.bitrate = core_settings_video_bitrate(ctx->settings);
        fin.gop = core_settings_video_gop(ctx->settings);
        const bool fullhd = CORE_VIDEO_IS_FULLHD(ctx->settings->video_width, ctx->settings->video_height) ||
                            (ctx->capture && CORE_VIDEO_IS_FULLHD(ctx->capture->video_width,
                                                                  ctx->capture->video_height));
        fin.qp_min = fullhd ? CORE_VIDEO_QP_1080_MIN : CORE_VIDEO_QP_MIN;
        fin.qp_max = fullhd ? CORE_VIDEO_QP_1080_MAX : CORE_VIDEO_QP_MAX;
    }
    fin.sequence = 0;
    fin.pir_enabled = ctx->pir_fast;
    (void)core_clip_queue_on_segment(&fin);
}

static bool rec_sd_write_head_frame(rec_dual_job_ctx_t *ctx) {
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (!ctx->ring->head) {
        xSemaphoreGive(ctx->ring_mtx);
        return false;
    }
    core_h264_frame_t *f = ctx->ring->head;
    uint32_t raw_pts = f->pts_ms;
    uint32_t pts = rec_muxer_pts(ctx, raw_pts);
    uint32_t size = f->size;

    if (size == 0) {
        core_h264_buffer_pop_head(ctx->ring);
        xSemaphoreGive(ctx->ring_mtx);
        ESP_LOGW(TAG, "Frame SD scartato (size=0)");
        ctx->dropped_frames++;
        return true;
    }

    // Plain e cifrato: copia sotto mutex, mux fuori mutex (evita race pool_compact vs puntatore ring).
    if (size > CORE_REC_SD_FRAME_COPY_MAX) {
        core_h264_buffer_pop_head(ctx->ring);
        xSemaphoreGive(ctx->ring_mtx);
        ESP_LOGW(TAG, "Frame SD troppo grande (%lu B, max %u) — scartato",
                 (unsigned long)size, (unsigned)CORE_REC_SD_FRAME_COPY_MAX);
        ctx->dropped_frames++;
        return true;
    }

    if (!rec_sd_ensure_frame_copy(ctx, size)) {
        xSemaphoreGive(ctx->ring_mtx);
        ESP_LOGW(TAG, "Copia frame SD fallita (%lu B)", (unsigned long)size);
        return false;
    }
    memcpy(ctx->sd_frame_copy, core_h264_frame_data(ctx->ring, f), size);
    xSemaphoreGive(ctx->ring_mtx);

    if (!recorder_muxer_write_packet(ctx->muxer, ctx->sd_frame_copy, size, pts, nullptr)) {
        ctx->sd_write_fails++;
        return false;
    }
    if (ctx->h264_fp) {
        if (fwrite(ctx->sd_frame_copy, 1, size, ctx->h264_fp) != size) {
            ESP_LOGW(TAG, "Scrittura .h264 fallita (%lu B)", (unsigned long)size);
        } else {
            if (ctx->h264_frames == 0) {
                ctx->h264_pts_first = raw_pts;
            }
            ctx->h264_pts_last = raw_pts;
            ctx->h264_frames++;
        }
    }

    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (ctx->ring->head && ctx->ring->head->pts_ms == raw_pts) {
        core_h264_buffer_pop_head(ctx->ring);
        ctx->muxer_frames++;
    }
    xSemaphoreGive(ctx->ring_mtx);
    return true;
}

static bool recorder_segment_path(char *out, size_t out_len, const char *session_base, uint32_t part) {
    if (!out || !session_base || part == 0) {
        return false;
    }
    int n = snprintf(out, out_len, "%s_part%02lu.mp4", session_base, (unsigned long)part);
    return n > 0 && (size_t)n < out_len;
}

// _partNN solo se serve davvero un secondo segmento:
// - stop GPIO (sessione potenzialmente lunga oltre un segmento), oppure
// - timer con durata sessione > durata segmento configurata.
static bool recorder_want_segment_parts(const core_settings_t *settings) {
    if (!settings) {
        return false;
    }
    if (!core_settings_rec_stop_is_timer(settings)) {
        return true;
    }
    return settings->rec_session_sec > settings->rec_max_duration_sec;
}

// Primo file è {base}.mp4; al primo rollover diventa {base}_part01.mp4.
static bool rec_sd_promote_first_to_part01(sd_prep_ctx_t *sd) {
    if (!sd || sd->segment_index != 0 || !sd->session_base[0]) {
        return true;
    }
    char part1[128];
    if (!recorder_segment_path(part1, sizeof(part1), sd->session_base, 1)) {
        ESP_LOGE(TAG, "Path _part01 troppo lungo");
        return false;
    }
    if (rename(sd->final_path, part1) != 0) {
        ESP_LOGE(TAG, "Rename %s → %s fallito (errno=%d)", sd->final_path, part1, errno);
        return false;
    }
    (void)core_clip_h264_rename(sd->final_path, part1);
    ESP_LOGI(TAG, "Sessione oltre un segmento — %s → %s", sd->final_path, part1);
    sd->segment_index = 1;
    snprintf(sd->final_path, sizeof(sd->final_path), "%s", part1);
    return true;
}

// Dopo chiusura di un segmento, punta final_path al _partNN successivo PRIMA di
// riaprire il muxer. Evita di riaprire _part01 in "wb" e cancellare il file chiuso.
static bool rec_sd_segment_advance_path(rec_dual_job_ctx_t *ctx) {
    sd_prep_ctx_t *sd = ctx->sd_ctx;
    if (!sd || !sd->segment_parts) {
        return true;
    }
    if (sd->segment_index == 0 && !rec_sd_promote_first_to_part01(sd)) {
        return false;
    }
    uint32_t next_part = sd->segment_index + 1;
    char next_path[128];
    if (!recorder_segment_path(next_path, sizeof(next_path), sd->session_base, next_part)) {
        ESP_LOGE(TAG, "Path segmento %lu troppo lungo", (unsigned long)next_part);
        return false;
    }
    sd->segment_index = next_part;
    snprintf(sd->final_path, sizeof(sd->final_path), "%s", next_path);
    return true;
}

static void rec_mux_temp_path(char *out, size_t out_len, const char *final_mp4) {
    if (!out || out_len == 0 || !final_mp4) {
        return;
    }
    snprintf(out, out_len, "%s.tmp", final_mp4);
}

static bool rec_mux_commit_file(const char *final_mp4) {
    if (!final_mp4 || !final_mp4[0]) {
        return false;
    }
    char tmp[160];
    rec_mux_temp_path(tmp, sizeof(tmp), final_mp4);
    struct stat st_final;
    struct stat st_tmp;
    if (stat(final_mp4, &st_final) == 0 && st_final.st_size >= (off_t)REC_MIN_MP4_BYTES) {
        remove(tmp);
        return true;
    }
    if (stat(tmp, &st_tmp) != 0 || st_tmp.st_size < (off_t)REC_MIN_MP4_BYTES) {
        remove(tmp);
        return false;
    }
    remove(final_mp4);
    if (rename(tmp, final_mp4) != 0) {
        ESP_LOGE(TAG, "MP4 rename %s → %s fallito (errno=%d)", tmp, final_mp4, errno);
        return false;
    }
    (void)core_clip_h264_rename(tmp, final_mp4);
    ESP_LOGI(TAG, "MP4 committato: %s (%ld B)", final_mp4, (long)st_tmp.st_size);
    return true;
}

static bool rec_sd_open_muxer_on_ring(rec_dual_job_ctx_t *ctx, const char *final_path,
                                      uint32_t head_pts) {
    char tmp[160];
    rec_mux_temp_path(tmp, sizeof(tmp), final_path);
    if (!recorder_ensure_muxer_open(ctx->muxer, tmp, ctx->ring, ctx->capture, !ctx->pir_fast)) {
        return false;
    }
    rec_mrbin_open_h264(ctx, final_path);
    if (ctx->muxer_was_active) {
        *ctx->muxer_was_active = true;
    }
    ctx->segment_pts_base = head_pts;
    ctx->segment_start_pts_ms = head_pts;
    ctx->segment_start_wall_us = esp_timer_get_time();
    for (uint32_t i = 0; i < ctx->sd_burst; i++) {
        if (!rec_sd_write_head_frame(ctx)) {
            break;
        }
    }
    return true;
}

static bool rec_sd_drain_ring_to_muxer(rec_dual_job_ctx_t *ctx) {
    uint32_t n = 0;
    while (ctx->muxer->active) {
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        bool empty = (ctx->ring->head == nullptr);
        xSemaphoreGive(ctx->ring_mtx);
        if (empty) {
            break;
        }
        if (!rec_sd_write_head_frame(ctx)) {
            break;
        }
        n++;
    }
    return n > 0;
}

static bool rec_sd_segment_elapsed(rec_dual_job_ctx_t *ctx) {
    if (!ctx->pir_fast || ctx->segment_max_sec == 0 || !ctx->muxer->active) {
        return false;
    }
    if (rec_session_stop_requested(ctx) || ctx->segment_closing) {
        return false;
    }
    if (ctx->segment_start_wall_us == 0) {
        return false;
    }
    int64_t elapsed_us = esp_timer_get_time() - ctx->segment_start_wall_us;
    return elapsed_us >= ((int64_t)ctx->segment_max_sec * 1000000LL);
}

static bool rec_sd_flush_pending_segment(rec_dual_job_ctx_t *ctx) {
    if (!ctx->sd_ctx->segment_parts || ctx->segments_closed == 0 || ctx->muxer->active) {
        return false;
    }
    const char *open_path = ctx->sd_ctx->final_path;
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    recorder_discard_psram_until_idr(ctx->ring);
    bool has_idr = core_h264_buffer_has_idr(ctx->ring);
    uint32_t head_pts = ctx->ring->head ? ctx->ring->head->pts_ms : 0;
    xSemaphoreGive(ctx->ring_mtx);
    if (!has_idr) {
        return false;
    }
    if (!rec_sd_open_muxer_on_ring(ctx, open_path, head_pts)) {
        return false;
    }
    ESP_LOGI(TAG, "Stop: flush segmento %lu (%s)", (unsigned long)ctx->sd_ctx->segment_index, open_path);
    return rec_sd_drain_ring_to_muxer(ctx);
}

static bool rec_sd_segment_rollover(rec_dual_job_ctx_t *ctx) {
    if (!ctx->sd_ctx->segment_parts || !ctx->muxer->active) {
        return false;
    }
    if (rec_session_stop_requested(ctx)) {
        ESP_LOGI(TAG, "Stop pin attivo — nessun nuovo segmento");
        return false;
    }

    ctx->segment_closing = true;

    uint32_t pre_drain = 0;
    while (rec_sd_write_head_frame(ctx)) {
        pre_drain++;
        if (pre_drain >= 20000) {
            ESP_LOGW(TAG, "Segmento: drain pre-close troncato a %lu frame", (unsigned long)pre_drain);
            break;
        }
    }

    sd_prep_ctx_t *sd = ctx->sd_ctx;
    recorder_muxer_close(ctx->muxer);
    (void)rec_mux_commit_file(sd->final_path);
    ctx->segments_closed++;

    recorder_finish_closed_mp4(ctx->settings, sd->final_path, ctx->pir_fast);
    rec_mrbin_finalize(ctx, sd->final_path);
    if (!recorder_mux_use_encrypt()) {
        s_plain_closed_part_max = (sd->segment_index == 0) ? 1 : sd->segment_index;
    }
    ESP_LOGI(TAG, "Segmento %lu chiuso (%s, %lu frame drain) — %lu segmenti su SD",
             (unsigned long)sd->segment_index, sd->final_path, (unsigned long)pre_drain,
             (unsigned long)ctx->segments_closed);

    ctx->segment_closing = false;

    if (rec_session_stop_requested(ctx)) {
        ESP_LOGI(TAG, "Stop pin durante chiusura segmento — fine registrazione");
        return false;
    }

    if (!rec_sd_segment_advance_path(ctx)) {
        return false;
    }

    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    recorder_discard_psram_until_idr(ctx->ring);
    bool has_idr = core_h264_buffer_has_idr(ctx->ring);
    uint32_t pts = (has_idr && ctx->ring->head) ? ctx->ring->head->pts_ms : 0;
    xSemaphoreGive(ctx->ring_mtx);

    if (!has_idr) {
        ESP_LOGI(TAG, "Segmento %lu: attendo IDR per %s",
                 (unsigned long)sd->segment_index, sd->final_path);
        return true;
    }

    if (rec_session_stop_requested(ctx)) {
        ESP_LOGI(TAG, "Stop pin prima di aprire segmento %lu — abort",
                 (unsigned long)sd->segment_index);
        return false;
    }

    if (!rec_sd_open_muxer_on_ring(ctx, sd->final_path, pts)) {
        ESP_LOGE(TAG, "Apertura segmento %lu fallita: %s",
                 (unsigned long)sd->segment_index, sd->final_path);
        return false;
    }
    ESP_LOGI(TAG, "Segmento %lu avviato: %s (durata max %lu min, PTS base %lu)",
             (unsigned long)sd->segment_index, sd->final_path,
             (unsigned long)(ctx->segment_max_sec / 60U), (unsigned long)pts);
    return true;
}

static bool rec_sd_may_write_head(rec_dual_job_ctx_t *ctx) {
    if (!ctx->sd_prep_done || !ctx->sd_ctx->paths_ok ||
        ctx->sd_write_fails >= REC_SD_WRITE_FAILS_MAX) {
        return false;
    }
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (!ctx->ring->head) {
        xSemaphoreGive(ctx->ring_mtx);
        return false;
    }
    uint32_t head_pts = ctx->ring->head->pts_ms;
    uint32_t tail_pts = ctx->producer_pts_max;
    uint32_t ring_pct = core_h264_buffer_fill_percent(ctx->ring);
    xSemaphoreGive(ctx->ring_mtx);

    uint32_t lag_ms = ctx->sd_lag_ms;
    if (ctx->should_stop() || ctx->user_stop) {
        lag_ms = 0;
    } else if (ring_pct >= rec_sd_lag_urgent_pct_for(ctx)) {
        lag_ms = 0;
    } else if (ring_pct >= rec_sd_lag_catchup_pct_for(ctx)) {
        lag_ms = ctx->sd_lag_ms / 2;
    }

    if (!ctx->capture_done &&
        (tail_pts < head_pts || (tail_pts - head_pts) < lag_ms)) {
        return false;
    }

    if (!ctx->muxer->active) {
        const bool on_stop = rec_session_stop_requested(ctx);
        if (!recorder_muxer_open_ready(ctx->capture, on_stop, ctx->pir_fast)) {
            return false;
        }
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        bool has_idr = core_h264_buffer_has_idr(ctx->ring);
        xSemaphoreGive(ctx->ring_mtx);
        if (!has_idr) {
            return false;
        }
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        recorder_discard_psram_until_idr(ctx->ring);
        if (!ctx->ring->head) {
            xSemaphoreGive(ctx->ring_mtx);
            return false;
        }
        head_pts = ctx->ring->head->pts_ms;
        xSemaphoreGive(ctx->ring_mtx);
        if (!rec_sd_open_muxer_on_ring(ctx, ctx->sd_ctx->final_path, head_pts)) {
            return false;
        }
        ESP_LOGI(TAG, "Mux SD aperto a %lld ms dal boot (IDR head PTS=%lu, rel=0, %s)",
                 (long long)core_boot_elapsed_ms(), (unsigned long)head_pts, ctx->sd_ctx->final_path);
    }
    return ctx->muxer->active;
}

static void rec_sd_job_task(void *arg) {
    rec_dual_job_ctx_t *ctx = (rec_dual_job_ctx_t *)arg;
    ESP_LOGI(TAG, "JOB2 PSRAM→SD [%s] (lag %u ms, burst %u, mux %s→SD)",
             ctx->rec_encrypt ? "AES" : "plain",
             (unsigned)ctx->sd_lag_ms, (unsigned)ctx->sd_burst,
             ctx->rec_encrypt ? "enc_writer" : "plain_writer");

    while (!ctx->sd_prep_done && !ctx->capture_done) {
        if (ctx->sd_ctx->ev && (xEventGroupGetBits(ctx->sd_ctx->ev) & SD_PREP_DONE_BIT)) {
            ctx->sd_prep_done = true;
            ESP_LOGI(TAG, "JOB2 SD: percorso registrazione pronto");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ctx->sd_prep_done && ctx->sd_ctx->ev) {
        ctx->sd_prep_done = (xEventGroupGetBits(ctx->sd_ctx->ev) & SD_PREP_DONE_BIT) != 0;
    }

    bool muxer_logged = false;
    for (;;) {
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        bool empty = (ctx->ring->head == nullptr);
        bool done = ctx->capture_done;
        xSemaphoreGive(ctx->ring_mtx);
        if (done && empty) {
            break;
        }

        if (rec_session_stop_requested(ctx) && done && !ctx->muxer->active) {
            if (ctx->sd_prep_done && ctx->sd_ctx->paths_ok &&
                ctx->sd_write_fails < REC_SD_WRITE_FAILS_MAX) {
                if (!rec_sd_flush_pending_segment(ctx)) {
                    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
                    bool has_idr = core_h264_buffer_has_idr(ctx->ring);
                    xSemaphoreGive(ctx->ring_mtx);
                    if (has_idr) {
                        (void)rec_sd_may_write_head(ctx);
                        rec_sd_drain_ring_to_muxer(ctx);
                    }
                }
            }
            xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
            if (!ctx->muxer->active && ctx->ring->head) {
                ESP_LOGW(TAG, "Stop: scarto %lu frame (muxer non aperto)",
                         (unsigned long)ctx->ring->frame_count);
                core_h264_buffer_clear(ctx->ring);
            }
            xSemaphoreGive(ctx->ring_mtx);
            break;
        }

        if (rec_sd_segment_elapsed(ctx)) {
            rec_sd_segment_rollover(ctx);
            if (rec_session_stop_requested(ctx)) {
                break;
            }
            taskYIELD();
            continue;
        }

        if (!rec_sd_may_write_head(ctx)) {
            if (ctx->should_stop() || ctx->user_stop ||
                rec_ring_fill_pct(ctx) >= rec_sd_lag_catchup_pct_for(ctx)) {
                taskYIELD();
            } else if (ctx->rec_encrypt) {
                vTaskDelay(pdMS_TO_TICKS(2));
            } else {
                taskYIELD();
            }
            continue;
        }

        if (!muxer_logged) {
            muxer_logged = true;
            ESP_LOGI(TAG, "JOB2 SD: scrittura MP4 %s", ctx->sd_ctx->final_path);
        }

        uint32_t burst_limit = rec_sd_burst_limit(ctx);
        uint32_t burst = 0;
        if (!ctx->pir_fast) {
            core_status_led_notify_rec_sd_writing(true);
        }
        for (uint32_t i = 0; i < burst_limit; i++) {
            if (!rec_sd_may_write_head(ctx)) {
                break;
            }
            if (!rec_sd_write_head_frame(ctx)) {
                break;
            }
            burst++;
            rec_sd_job_yield_if_needed(ctx, i);
        }
        if (!ctx->pir_fast) {
            core_status_led_notify_rec_sd_writing(false);
        }
        if (burst == 0) {
            if (ctx->should_stop() || ctx->user_stop ||
                rec_ring_fill_pct(ctx) >= rec_sd_lag_catchup_pct_for(ctx)) {
                taskYIELD();
            } else if (ctx->rec_encrypt) {
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                taskYIELD();
            }
        }
    }

    if (!ctx->pir_fast) {
        core_status_led_notify_rec_sd_writing(false);
    }
    if (ctx->capture_done && ctx->sd_prep_done && ctx->sd_ctx->paths_ok &&
        ctx->sd_write_fails < REC_SD_WRITE_FAILS_MAX) {
        (void)rec_sd_drain_ring_to_muxer(ctx);
    }
    if (ctx->muxer && ctx->muxer->active) {
        ESP_LOGI(TAG, "JOB2 SD: chiusura muxer %s (%lu frame mux)",
                 ctx->sd_ctx->final_path, (unsigned long)ctx->muxer_frames);
        recorder_muxer_close(ctx->muxer);
        (void)rec_mux_commit_file(ctx->sd_ctx->final_path);
        recorder_finish_closed_mp4(ctx->settings, ctx->sd_ctx->final_path, ctx->pir_fast);
        rec_mrbin_finalize(ctx, ctx->sd_ctx->final_path);
        ctx->muxer_closed_on_sd = true;
    }
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    uint32_t ring_left = ctx->ring->frame_count;
    xSemaphoreGive(ctx->ring_mtx);
    ESP_LOGI(TAG, "JOB2 SD terminato (%lu frame scritti, %lu ancora in ring)",
             (unsigned long)ctx->muxer_frames, (unsigned long)ring_left);
    rec_sd_free_frame_copy(ctx);
    ctx->sd_done = true;
    vTaskDelete(nullptr);
}

static bool recorder_process_frame_psram(recorder_capture_t *capture, core_h264_buffer_t *psram_buf,
                                         uint32_t *dropped_frames, bool no_wait) {
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, no_wait);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    uint32_t pts_ms = 0;
    if (!recorder_frame_accept(capture, frame.pts, &pts_ms)) {
        rec_release_got(capture, &frame);
        return true;
    }

    if (!core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size)) {
        if (dropped_frames) {
            (*dropped_frames)++;
        }
    }

    rec_release_got(capture, &frame);
    return true;
}

static bool recorder_ensure_muxer_open(recorder_muxer_t *muxer, const char *path,
                                       core_h264_buffer_t *psram_buf,
                                       const recorder_capture_t *capture,
                                       bool moov_before_mdat) {
    if (muxer->active) {
        return true;
    }
    if (!capture || !core_h264_buffer_has_idr(psram_buf)) {
        return false;
    }
    uint8_t mux_fps = recorder_muxer_fps_for_open(psram_buf, capture);
    ESP_LOGI(TAG, "Mux fps = %u (preset/config)", (unsigned)mux_fps);
    return recorder_muxer_start_from_buffer(muxer, path, psram_buf, nullptr,
                                            capture->video_width, capture->video_height,
                                            mux_fps, moov_before_mdat);
}

static bool recorder_flush_psram_to_sd(recorder_muxer_t *muxer, recorder_capture_t *capture,
                                       const sd_prep_ctx_t *sd_ctx, core_h264_buffer_t *psram_buf,
                                       bool capture_ok, bool sd_prep_done, bool *muxer_was_active,
                                       uint32_t *dropped_frames, uint32_t *flush_cycles,
                                       recorder_should_stop_fn_t should_stop, bool *capture_running) {
    if (!muxer || !sd_ctx || !psram_buf || !sd_prep_done || !sd_ctx->paths_ok ||
        psram_buf->frame_count == 0) {
        return false;
    }
    if (!core_h264_buffer_should_flush(psram_buf)) {
        return false;
    }

    if (!recorder_ensure_muxer_open(muxer, sd_ctx->final_path, psram_buf, capture, true)) {
        ESP_LOGW(TAG, "Flush SD: muxer non pronto (IDR/SPS mancanti)");
        return false;
    }
    if (!muxer->active) {
        return false;
    }
    if (muxer_was_active) {
        *muxer_was_active = true;
    }

    if (*flush_cycles == 0) {
        recorder_discard_psram_until_idr(psram_buf);
    }

    int64_t t0 = esp_timer_get_time();
    uint32_t drained = 0;
    uint32_t stall = 0;

    ESP_LOGI(TAG, "Flush SD ciclo %lu: pool %u%% (%lu KB, %lu frame) → svuotamento...",
             (unsigned long)(*flush_cycles + 1),
             (unsigned)core_h264_buffer_fill_percent(psram_buf),
             (unsigned long)(core_h264_buffer_bytes_used(psram_buf) / 1024),
             (unsigned long)psram_buf->frame_count);

    while (psram_buf->frame_count > 0) {
        if (should_stop && should_stop()) {
            ESP_LOGI(TAG, "Flush SD interrotto da stop (%lu frame già scritti, %lu in pool)",
                     (unsigned long)drained, (unsigned long)psram_buf->frame_count);
            if (capture && capture->running) {
                recorder_capture_stop(capture);
                if (capture_running) {
                    *capture_running = false;
                }
            }
            break;
        }
        uint32_t n = recorder_drain_psram_to_muxer(muxer, psram_buf, should_stop);
        drained += n;
        if (n == 0) {
            if (++stall >= 400) {
                ESP_LOGE(TAG, "Flush SD bloccato dopo %lu frame — memoria muxer/SD esaurita",
                         (unsigned long)drained);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            stall = 0;
        }
    }

    if (psram_buf->frame_count > 0) {
        ESP_LOGW(TAG, "Flush incompleto: %lu frame rimasti in pool", (unsigned long)psram_buf->frame_count);
    }

    (*flush_cycles)++;

    ESP_LOGI(TAG, "Flush SD ciclo %lu completato: %lu frame in %lld ms (pool 0%%)",
             (unsigned long)*flush_cycles, (unsigned long)drained,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static bool recorder_final_psram_flush(recorder_muxer_t *muxer, recorder_capture_t *capture,
                                       const sd_prep_ctx_t *sd_ctx, core_h264_buffer_t *psram_buf,
                                       bool capture_ok, bool sd_prep_done, bool *muxer_was_active,
                                       uint32_t *dropped_frames, bool stop_requested,
                                       recorder_should_stop_fn_t should_stop) {
    if (!psram_buf || psram_buf->frame_count == 0 || !sd_prep_done || !sd_ctx->paths_ok) {
        return false;
    }
    if (!recorder_ensure_muxer_open(muxer, sd_ctx->final_path, psram_buf, capture, true)) {
        return false;
    }
    if (muxer_was_active && muxer->active) {
        *muxer_was_active = true;
    }

    ESP_LOGI(TAG, "Flush finale PSRAM: %lu frame (~%u s)",
             (unsigned long)psram_buf->frame_count,
             (unsigned)core_h264_buffer_estimated_sec(psram_buf));

    uint32_t flush_limit_ms = stop_requested ? REC_FINAL_FLUSH_STOP_MS
                                             : (REC_DRAIN_TIMEOUT_MS * 3000U);
    int64_t until = esp_timer_get_time() + ((int64_t)flush_limit_ms * 1000);
    while (psram_buf->frame_count > 0 && esp_timer_get_time() < until) {
        if (stop_requested) {
            break;
        }
        if (recorder_drain_psram_to_muxer(muxer, psram_buf, nullptr) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (stop_requested && psram_buf->frame_count > 0) {
        ESP_LOGW(TAG, "Flush finale abbreviato da stop: %lu frame non scritti",
                 (unsigned long)psram_buf->frame_count);
    }
    return psram_buf->frame_count == 0;
}

static bool recorder_process_frame(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                   core_h264_buffer_t *psram_buf, uint32_t *dropped_frames,
                                   bool no_wait) {
    (void)muxer;
    return recorder_process_frame_psram(capture, psram_buf, dropped_frames, no_wait);
}

static void recorder_drain_capture(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                   core_h264_buffer_t *psram_buf, uint32_t timeout_ms) {
    if (!capture || !capture->running) {
        return;
    }
    int64_t until = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < until) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, false);
        if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint32_t pts_ms = 0;
        if (!recorder_frame_accept(capture, frame.pts, &pts_ms)) {
            rec_release_got(capture, &frame);
            continue;
        }
        if (!core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size)) {
            ESP_LOGW(TAG, "Pool pieno durante drain capture");
        }
        rec_release_got(capture, &frame);
    }
}

static void recorder_drain_pending_capture(recorder_capture_t *capture, core_h264_buffer_t *psram_buf,
                                           uint32_t *dropped_frames) {
    if (!capture || !capture->running || !psram_buf) {
        return;
    }
    uint32_t recovered = 0;
    for (;;) {
        if (!recorder_process_frame_psram(capture, psram_buf, dropped_frames, true)) {
            break;
        }
        recovered++;
    }
    if (recovered > 0) {
        ESP_LOGI(TAG, "Recuperati %lu frame in coda capture", (unsigned long)recovered);
    }
}

static void sd_prep_task(void *arg) {
    sd_prep_ctx_t *ctx = (sd_prep_ctx_t *)arg;
    int64_t t0 = esp_timer_get_time();

    core_time_init();

    ctx->sd_mounted = core_sd_init_pir();
    if (ctx->sd_mounted) {
        if (core_sd_ensure_space(CORE_SD_MIN_FREE_BYTES)) {
            char day_dir[64];
            if (core_sd_make_day_dir(day_dir, sizeof(day_dir))) {
                if (core_sd_make_recording_path(day_dir, ctx->core_id, ctx->final_path, sizeof(ctx->final_path))) {
#if CORE_REC_PLAIN_SD_BENCHMARK
                    rec_plain_bench_path(ctx->final_path, sizeof(ctx->final_path));
#endif
                    snprintf(ctx->tmp_path, sizeof(ctx->tmp_path), "%s.tmp", ctx->final_path);
                    if (ctx->segment_parts) {
                        char *dot = strrchr(ctx->final_path, '.');
                        if (dot) {
                            size_t base_len = (size_t)(dot - ctx->final_path);
                            if (base_len >= sizeof(ctx->session_base)) {
                                base_len = sizeof(ctx->session_base) - 1;
                            }
                            memcpy(ctx->session_base, ctx->final_path, base_len);
                            ctx->session_base[base_len] = '\0';
                        } else {
                            snprintf(ctx->session_base, sizeof(ctx->session_base), "%s", ctx->final_path);
                        }
                        ctx->segment_index = 0;
                        ctx->paths_ok = true;
                    } else {
                        ctx->paths_ok = true;
                    }
                    if (ctx->paths_ok) {
#if CORE_REC_PLAIN_SD_BENCHMARK
                        ESP_LOGI(TAG, "Path registrazione: %s (MP4 in chiaro — benchmark, no AES)",
                                 ctx->final_path);
#else
                        if (ctx->rec_encrypt) {
                            ESP_LOGI(TAG, "Path registrazione: %s (MP4 cifrato on-the-fly dal writer del muxer)",
                                     ctx->final_path);
                        } else {
                            ESP_LOGI(TAG, "Path registrazione: %s (MP4 in chiaro — max fluidità)",
                                     ctx->final_path);
                        }
#endif
                        core_status_led_notify_sd_ready();
                    }
                } else {
                    ESP_LOGE(TAG, "Generazione path file fallita");
                }
            } else {
                ESP_LOGE(TAG, "Creazione directory giorno fallita");
            }
        } else {
            ESP_LOGE(TAG, "Spazio SD insufficiente (min %llu MB)",
                     (unsigned long long)(CORE_SD_MIN_FREE_BYTES / (1024ULL * 1024ULL)));
        }
    } else {
        ESP_LOGE(TAG, "Mount SD fallito in sd_prep");
    }

    ESP_LOGI(TAG, "SD prep completata in %lld ms (mount=%d paths=%d)",
             (long long)((esp_timer_get_time() - t0) / 1000), ctx->sd_mounted, ctx->paths_ok);
    xEventGroupSetBits(ctx->ev, SD_PREP_DONE_BIT);
    vTaskDelete(nullptr);
}

static volatile bool s_manual_stop = false;
static volatile bool s_manual_active = false;
static volatile bool s_manual_starting = false;
static volatile bool s_manual_stopping = false;
/* Capture and camera are down; moov/fsync may still be running. */
static volatile bool s_manual_finalizing = false;
static volatile bool s_manual_ready = false;
static volatile bool s_manual_last_saved = false;
static volatile uint32_t s_manual_frame_count = 0;
static TaskHandle_t s_manual_task = nullptr;
static EventGroupHandle_t s_manual_ev = nullptr;
static char s_manual_last_path[128] = {0};
static char s_manual_error[96] = {0};

static recorder_capture_t s_demo_prep_cap;
static core_h264_buffer_t s_demo_prep_preroll;
static volatile bool s_demo_prep_ready;
static volatile bool s_demo_prep_busy;
static volatile bool s_demo_prep_abort;
static volatile bool s_demo_prep_take;
static volatile bool s_demo_prep_preroll_active;
static TaskHandle_t s_demo_prep_task;
static core_settings_t s_demo_prep_settings;

static void demo_prep_release_all(void) {
    s_demo_prep_take = true;
    if (s_demo_prep_preroll_active) {
        core_h264_buffer_deinit(&s_demo_prep_preroll);
        memset(&s_demo_prep_preroll, 0, sizeof(s_demo_prep_preroll));
        s_demo_prep_preroll_active = false;
    }
    recorder_capture_stop(&s_demo_prep_cap);
    memset(&s_demo_prep_cap, 0, sizeof(s_demo_prep_cap));
    s_demo_prep_ready = false;
}

static void demo_prep_wait_idle(uint32_t timeout_ms) {
    const int64_t until_us =
        esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while ((s_demo_prep_busy || s_demo_prep_task) && esp_timer_get_time() < until_us) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool demo_prep_wait_ready(uint32_t timeout_ms) {
    const int64_t until_us =
        esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < until_us) {
        if (s_demo_prep_ready) {
            return true;
        }
        if (!s_demo_prep_busy && !s_demo_prep_task) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_demo_prep_ready;
}

void core_demo_rec_prep_cancel(void) {
    s_demo_prep_abort = true;
    s_demo_prep_take = true;
    demo_prep_wait_idle(4000);
    if (s_demo_prep_ready || s_demo_prep_cap.capture || s_demo_prep_preroll_active) {
        demo_prep_release_all();
        core_rtsp_release();
        ESP_LOGI(TAG, "DEMO prep encoder annullato");
    }
    s_demo_prep_abort = false;
    s_demo_prep_take = false;
}

static void demo_prep_ae_settle(recorder_capture_t *cap) {
    if (!cap || !cap->sink) {
        return;
    }
    const uint32_t ae_settle_ms = core_rec_fast_requested() ? 0u : (uint32_t)CORE_DEMO_PREP_AE_SETTLE_MS;
    if (ae_settle_ms == 0) {
        return;
    }
    const int64_t ae_until_us = esp_timer_get_time() + ((int64_t)ae_settle_ms * 1000LL);
    ESP_LOGI(TAG, "DEMO prep: settle AE %lu ms (scarto frame, come PIR)",
             (unsigned long)ae_settle_ms);
    while (esp_timer_get_time() < ae_until_us) {
        esp_capture_stream_frame_t discard = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        if (esp_capture_sink_acquire_frame(cap->sink, &discard, true) == ESP_CAPTURE_ERR_OK &&
            discard.data && discard.size > 0) {
            esp_capture_sink_release_frame(cap->sink, &discard);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    (void)core_isp_cal_startup_wb_check();
}

static bool demo_prep_splice_preroll(core_h264_buffer_t *ring, recorder_capture_t *cap,
                                     SemaphoreHandle_t ring_mtx) {
    if (!ring || !cap || !s_demo_prep_preroll_active || s_demo_prep_preroll.frame_count == 0) {
        return false;
    }
    recorder_discard_psram_until_idr(&s_demo_prep_preroll);
    if (!s_demo_prep_preroll.head) {
        core_h264_buffer_deinit(&s_demo_prep_preroll);
        s_demo_prep_preroll_active = false;
        return false;
    }
    const uint32_t step = cap->frame_dur_ms > 0 ? cap->frame_dur_ms : 33U;
    uint32_t seq_pts = 0;
    uint32_t moved = 0;
    xSemaphoreTake(ring_mtx, portMAX_DELAY);
    while (s_demo_prep_preroll.head) {
        core_h264_frame_t *f = s_demo_prep_preroll.head;
        const uint8_t *data = core_h264_frame_data(&s_demo_prep_preroll, f);
        if (!data || !core_h264_buffer_push(ring, seq_pts, data, f->size)) {
            break;
        }
        seq_pts += step;
        core_h264_buffer_pop_head(&s_demo_prep_preroll);
        moved++;
    }
    xSemaphoreGive(ring_mtx);
    core_h264_buffer_deinit(&s_demo_prep_preroll);
    s_demo_prep_preroll_active = false;
    cap->warmup_left = 0;
    cap->need_idr_resync = false;
    cap->kept_frames = ring->frame_count;
    const int64_t now_us = esp_timer_get_time();
    cap->last_kept_us = now_us;
    cap->pts_epoch_ms = 0;
    cap->pts_origin_us = now_us;
    cap->last_out_pts = moved > 0 ? (seq_pts - step) : 0;
    ESP_LOGI(TAG, "DEMO preroll in clip: %lu frame H264 (~%u s, PTS fino a %lu ms)",
             (unsigned long)moved, (unsigned)core_h264_buffer_estimated_sec(ring),
             (unsigned long)cap->last_out_pts);
    return moved > 0;
}

static bool demo_prep_consume(recorder_capture_t *out) {
    if (!out || !s_demo_prep_ready) {
        return false;
    }
    s_demo_prep_take = true;
    demo_prep_wait_idle(1500);
    *out = s_demo_prep_cap;
    memset(&s_demo_prep_cap, 0, sizeof(s_demo_prep_cap));
    s_demo_prep_ready = false;
    s_demo_prep_take = false;
    ESP_LOGI(TAG, "DEMO prep encoder pronto — clip senza reopen VI");
    return true;
}

static void demo_prep_task(void *arg) {
    (void)arg;
    core_settings_t settings = s_demo_prep_settings;
    core_rtsp_hold();
    core_live_force_release();
    (void)core_live_wait_idle(800);
    core_video_isp_pir_arm();
    recorder_capture_t cap = {};
    bool ok = recorder_capture_start_psram(&cap, 0, &settings, false, false) &&
              recorder_capture_prime(&cap, CORE_REC_PRIME_TIMEOUT_MS);
    if (ok && !s_demo_prep_abort) {
        core_video_isp_rec_after_vi();
        core_video_isp_pir_lock();
        demo_prep_ae_settle(&cap);
        memset(&s_demo_prep_preroll, 0, sizeof(s_demo_prep_preroll));
        if (core_h264_buffer_init_recording(&s_demo_prep_preroll, true, false)) {
            s_demo_prep_preroll_active = true;
        }
        s_demo_prep_cap = cap;
        s_demo_prep_ready = true;
        ESP_LOGI(TAG, "DEMO prep: VI+H264 pronti — preroll attivo");
        uint32_t prep_kept = 0;
        while (!s_demo_prep_abort && !s_demo_prep_take && cap.running) {
            esp_capture_stream_frame_t frame = {
                .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
            };
            esp_capture_err_t err =
                esp_capture_sink_acquire_frame(cap.sink, &frame, true);
            if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const bool idr = core_h264_is_idr_nal(frame.data, (uint32_t)frame.size);
            if (prep_kept == 0 && !idr) {
                esp_capture_sink_release_frame(cap.sink, &frame);
                continue;
            }
            if (s_demo_prep_preroll_active) {
                bool trimmed = false;
                while (s_demo_prep_preroll.frame_count >= CORE_DEMO_PREP_PREROLL_MAX_FRAMES &&
                       s_demo_prep_preroll.head) {
                    core_h264_buffer_pop_head(&s_demo_prep_preroll);
                    trimmed = true;
                }
                if (trimmed) {
                    while (s_demo_prep_preroll.head &&
                           !core_h264_is_idr_nal(
                               core_h264_frame_data(&s_demo_prep_preroll, s_demo_prep_preroll.head),
                               s_demo_prep_preroll.head->size)) {
                        core_h264_buffer_pop_head(&s_demo_prep_preroll);
                    }
                }
                const uint32_t pts =
                    cap.frame_dur_ms > 0 ? prep_kept * cap.frame_dur_ms : prep_kept * 33U;
                if (core_h264_buffer_push(&s_demo_prep_preroll, pts, frame.data,
                                          (uint32_t)frame.size)) {
                    prep_kept++;
                }
            }
            esp_capture_sink_release_frame(cap.sink, &frame);
        }
        if (prep_kept > 0) {
            ESP_LOGI(TAG, "DEMO prep preroll: %lu frame (~%u s)",
                     (unsigned long)s_demo_prep_preroll.frame_count,
                     (unsigned)core_h264_buffer_estimated_sec(&s_demo_prep_preroll));
        }
    } else {
        recorder_capture_stop(&cap);
        core_rtsp_release();
        if (!ok) {
            ESP_LOGW(TAG, "DEMO prep: apertura encoder fallita");
        }
    }
    s_demo_prep_busy = false;
    s_demo_prep_task = nullptr;
    s_demo_prep_abort = false;
    vTaskDelete(nullptr);
}

bool core_demo_rec_prep_is_ready(void) {
    return s_demo_prep_ready;
}

bool core_demo_rec_prep_wait_warm(uint32_t timeout_ms) {
    if (s_demo_prep_ready) {
        return true;
    }
    if (s_demo_prep_busy || s_demo_prep_task) {
        return demo_prep_wait_ready(timeout_ms);
    }
    return false;
}

void core_demo_rec_prep_kick(const core_settings_t *settings) {
    if (!settings || !core_demo_is_active()) {
        return;
    }
    if (s_manual_active || s_manual_starting || s_manual_task) {
        return;
    }
    if (s_demo_prep_ready || s_demo_prep_busy || s_demo_prep_task) {
        return;
    }
    s_demo_prep_settings = *settings;
    s_demo_prep_abort = false;
    s_demo_prep_take = false;
    s_demo_prep_busy = true;
    if (xTaskCreate(demo_prep_task, "demo_prep", 12288, nullptr, 3, &s_demo_prep_task) != pdPASS) {
        s_demo_prep_busy = false;
        s_demo_prep_task = nullptr;
        ESP_LOGW(TAG, "DEMO prep: task non creato");
    }
}

typedef struct {
    core_settings_t settings;
} manual_rec_arg_t;

static bool manual_should_stop(void) {
    return s_manual_stop;
}

static bool rec_stop_should_stop(void) {
    return core_gpio_is_rec_stop_triggered();
}

static int pir_bench_limit_sec(void) {
    const char *e = getenv("MRBIN_PIR_BENCH_SEC");
    if (!e || !e[0]) {
        return 0;
    }
    int n = atoi(e);
    if (n < 1) {
        return 0;
    }
    if (n > 15) {
        return 15;
    }
    return n;
}

static bool rec_pir_job_should_stop(void) {
    const int bench = pir_bench_limit_sec();
    if (bench > 0) {
        static int64_t bench_t0 = 0;
        if (bench_t0 == 0) {
            bench_t0 = esp_timer_get_time();
        }
        if ((esp_timer_get_time() - bench_t0) >= ((int64_t)bench * 1000000LL)) {
            if (s_active_rec_job) {
                s_active_rec_job->user_stop = true;
            }
            return true;
        }
        return false;
    }
    if (s_active_rec_job && s_active_rec_job->ignore_gpio_stop) {
        return false;
    }
    if (core_gpio_is_rec_stop_triggered()) {
        if (s_active_rec_job) {
            s_active_rec_job->user_stop = true;
        }
        return true;
    }
    return false;
}

typedef struct {
    esp_capture_handle_t        capture;
    esp_capture_sink_handle_t   sink;
    esp_capture_video_src_if_t *vsrc;
    esp_capture_overlay_if_t   *overlay;
    uint16_t                    osd_w;
    uint16_t                    osd_h;
    uint8_t                     osd_font;
    int64_t                     osd_last_us;
    uint16_t                    width;
    uint16_t                    height;
} photo_capture_t;

static esp_capture_err_t photo_pipeline_event(esp_capture_event_t event, void *ctx) {
    if (event != ESP_CAPTURE_EVENT_VIDEO_PIPELINE_BUILT || !ctx) {
        return ESP_CAPTURE_ERR_OK;
    }
    photo_capture_t *pc = (photo_capture_t *)ctx;
    rec_apply_ppa_aspect_crop(pc->sink, pc->width, pc->height);
    return ESP_CAPTURE_ERR_OK;
}

static bool photo_capture_start(photo_capture_t *pc, const core_settings_t *settings) {
    if (!pc || !settings) {
        return false;
    }
    memset(pc, 0, sizeof(*pc));
    pc->width = settings->video_width;
    pc->height = settings->video_height;
    uint8_t photo_fps = settings->video_fps;
    rec_apply_selected_encode_size(&pc->width, &pc->height, &photo_fps);

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = 4,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    pc->vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!pc->vsrc) {
        ESP_LOGE(TAG, "photo: v4l2 src fallita (buf_count=%u)", (unsigned)v4l2_cfg.buf_count);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .video_src = pc->vsrc,
    };
    if (esp_capture_open(&cap_cfg, &pc->capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "photo: esp_capture_open fallita");
        free(pc->vsrc);
        pc->vsrc = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }

    // Sink > 1 fps: a 1 fps AE/ISP restano neri. Il 1 fps reale è nel loop.
    // Sopra 5 MP il sensore/encode è 15, non 25.
    uint8_t sink_fps = 10;
    if ((uint32_t)pc->width * (uint32_t)pc->height > CORE_VIDEO_PIXELS_5M) {
        sink_fps = (uint8_t)CORE_VIDEO_FPS_ABOVE_5M;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_MJPEG,
            .width = pc->width,
            .height = pc->height,
            .fps = sink_fps,
        },
    };
    if (esp_capture_sink_setup(pc->capture, 0, &sink_cfg, &pc->sink) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "photo: sink MJPEG fallita (%ux%u @%u)",
                 (unsigned)pc->width, (unsigned)pc->height, (unsigned)sink_fps);
        esp_capture_close(pc->capture);
        pc->capture = nullptr;
        free(pc->vsrc);
        pc->vsrc = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }

    (void)esp_capture_set_event_cb(pc->capture, photo_pipeline_event, pc);
    esp_capture_sink_enable(pc->sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
#if CORE_REC_OSD_ENABLE
    (void)recorder_osd_attach(pc->sink, settings, pc->width, pc->height,
                              &pc->overlay, &pc->osd_w, &pc->osd_h, &pc->osd_font);
    if (pc->overlay) {
        recorder_osd_refresh(pc->overlay, pc->osd_w, pc->osd_h, pc->osd_font, &pc->osd_last_us);
    }
#endif
    if (esp_capture_start(pc->capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "photo: esp_capture_start fallita");
#if CORE_REC_OSD_ENABLE
        recorder_osd_close(&pc->overlay);
#endif
        esp_capture_close(pc->capture);
        pc->capture = nullptr;
        free(pc->vsrc);
        pc->vsrc = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }
    core_video_reapply_flip();
    rec_note_encode(pc->width, pc->height, 1, "photo JPEG MJPEG sink");
    ESP_LOGI(TAG, "Photo MJPEG %ux%u sink@%u fps (pacing software 1 fps)",
             (unsigned)pc->width, (unsigned)pc->height, (unsigned)sink_fps);
    return true;
}

static void photo_capture_stop(photo_capture_t *pc) {
    if (!pc || !pc->capture) {
        return;
    }
    esp_capture_stop(pc->capture);
#if CORE_REC_OSD_ENABLE
    recorder_osd_close(&pc->overlay);
#endif
    esp_capture_close(pc->capture);
    pc->capture = nullptr;
    pc->sink = nullptr;
    if (pc->vsrc) {
        free(pc->vsrc);
        pc->vsrc = nullptr;
    }
    esp_video_enc_unregister_default();
}

static bool photo_session_should_stop(const core_settings_t *settings, uint32_t photo_count) {
    if (s_manual_stop) {
        return true;
    }
    if (s_manual_active || s_manual_starting) {
        if (core_settings_rec_stop_is_timer(settings)) {
            return photo_count >= settings->rec_session_sec;
        }
        return false;
    }
    if (core_settings_rec_stop_is_timer(settings)) {
        // A 1 fps: N secondi → esattamente N JPEG (non wall-clock: altrimenti acquire lento → meno foto).
        return photo_count >= settings->rec_session_sec;
    }
    return core_gpio_is_rec_stop_triggered();
}

// Salva JPEG in chiaro oppure [MRBI][IV][AES-CTR] (timelapse fisso a 1 fps).
static bool photo_save_jpeg_file(const char *path, const uint8_t *data, size_t len,
                                 const uint8_t *aes_key_or_null) {
    if (!path || !data || len == 0) {
        return false;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "Photo: fopen %s fallita (errno=%d)", path, errno);
        return false;
    }
    if (!aes_key_or_null) {
        size_t wr = fwrite(data, 1, len, fp);
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        if (wr != len) {
            ESP_LOGW(TAG, "Photo: scrittura incompleta %s (%u/%u B)",
                     path, (unsigned)wr, (unsigned)len);
            return false;
        }
        return true;
    }

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (fwrite(&magic, 1, sizeof(magic), fp) != sizeof(magic) ||
        fwrite(iv, 1, sizeof(iv), fp) != sizeof(iv)) {
        fclose(fp);
        remove(path);
        ESP_LOGW(TAG, "Photo: header MRBI fallito %s", path);
        return false;
    }
    core_crypto_ctx_t ctx;
    if (!core_crypto_init_ctx(&ctx, aes_key_or_null, iv)) {
        fclose(fp);
        remove(path);
        return false;
    }
    bool ok = core_crypto_write_encrypted_chunk(&ctx, fp, data, len, 0);
    core_crypto_deinit_ctx(&ctx);
    if (ok) {
        fflush(fp);
        fsync(fileno(fp));
    }
    fclose(fp);
    if (!ok) {
        remove(path);
        ESP_LOGW(TAG, "Photo: cifratura fallita %s", path);
        return false;
    }
    return true;
}

struct RtspCameraHold {
    RtspCameraHold() { core_rtsp_hold(); }
    ~RtspCameraHold() { core_rtsp_release(); }
};

static bool recorder_run_photo_session(const core_settings_t *settings) {
    if (!settings) {
        return false;
    }
    RtspCameraHold rtsp_hold;
    (void)rtsp_hold;
    const bool encrypt = recorder_settings_want_encrypt(settings);
    const uint8_t *aes_key = encrypt ? settings->aes_key : nullptr;

    core_gpio_rec_stop_session_begin(core_settings_rec_stop_ignores_gpio(settings));

    core_time_init();
    if (!core_sd_init_pir()) {
        ESP_LOGE(TAG, "Photo: mount SD fallito");
        core_gpio_rec_stop_session_end();
        return false;
    }
    if (!core_sd_ensure_space(CORE_SD_MIN_FREE_BYTES)) {
        ESP_LOGE(TAG, "Photo: spazio SD insufficiente");
        core_gpio_rec_stop_session_end();
        return false;
    }

    char day_dir[64];
    char session_mp4[128];
    if (!core_sd_make_day_dir(day_dir, sizeof(day_dir)) ||
        !core_sd_make_recording_path(day_dir, settings->core_id, session_mp4, sizeof(session_mp4))) {
        ESP_LOGE(TAG, "Photo: path sessione fallito");
        core_gpio_rec_stop_session_end();
        return false;
    }

    char session_base[128];
    const char *dot = strrchr(session_mp4, '.');
    if (dot) {
        size_t blen = (size_t)(dot - session_mp4);
        if (blen >= sizeof(session_base)) {
            blen = sizeof(session_base) - 1;
        }
        memcpy(session_base, session_mp4, blen);
        session_base[blen] = '\0';
    } else {
        snprintf(session_base, sizeof(session_base), "%s", session_mp4);
    }

    if (!core_video_is_ready() && !core_video_init()) {
        ESP_LOGE(TAG, "Photo: init camera fallita");
        core_gpio_rec_stop_session_end();
        return false;
    }

    if (!core_video_prepare_for_settings(settings)) {
        ESP_LOGW(TAG, "Photo: cambio formato sensore fallito — continuo con modalità corrente");
    } else {
        uint16_t sw = 0;
        uint16_t sh = 0;
        if (core_video_get_sensor_size(&sw, &sh)) {
            ESP_LOGI(TAG, "Photo: sensore %ux%u, richiesto %ux%u, FullHD nativo=%s",
                     (unsigned)sw, (unsigned)sh,
                     (unsigned)settings->video_width, (unsigned)settings->video_height,
                     core_settings_wants_native_1080(settings) ? "sì" : "no");
        }
    }

    (void)core_isp_cal_apply_active();
    if (core_video_module_is_af()) {
        vTaskDelay(pdMS_TO_TICKS(CORE_REC_VCM_SETTLE_MS_AF));
    }

    photo_capture_t pc = {};
    if (!photo_capture_start(&pc, settings)) {
        snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                 "Photo JPEG %ux%u start failed",
                 (unsigned)settings->video_width, (unsigned)settings->video_height);
        rec_note_encode(settings->video_width, settings->video_height, 1, s_last_enc_err);
        core_gpio_rec_stop_session_end();
        return false;
    }

    if (s_manual_active || s_manual_starting) {
        s_manual_ready = true;
        s_manual_starting = false;
    }

    // LED subito: il settle sotto non deve sembrare "morto".
    core_status_led_set_mode(CORE_LED_RECORDING);

    // AE fuori dal timer sessione (no_wait: acquire bloccante a 1 fps allungava il settle).
    const uint32_t ae_settle_ms = core_rec_fast_requested()
                                      ? 0u
                                      : (core_video_module_is_af()
                                             ? (uint32_t)CORE_REC_AE_SETTLE_MS_PIR_AF
                                             : (uint32_t)CORE_REC_AE_SETTLE_MS_PIR);
    {
        const int64_t ae_until_us = esp_timer_get_time() + ((int64_t)ae_settle_ms * 1000LL);
        ESP_LOGI(TAG, "Photo: settle AE %lu ms prima del timer sessione%s",
                 (unsigned long)ae_settle_ms,
                 core_rec_fast_requested() ? " (rec-fast skip)"
                                           : (core_video_module_is_af() ? " (AF)" : ""));
        while (esp_timer_get_time() < ae_until_us) {
            esp_capture_stream_frame_t discard = {
                .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
            };
            if (esp_capture_sink_acquire_frame(pc.sink, &discard, true) == ESP_CAPTURE_ERR_OK &&
                discard.data && discard.size > 0) {
                esp_capture_sink_release_frame(pc.sink, &discard);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        (void)core_isp_cal_startup_wb_check();
    }

    // Timer: N secondi @ 1 fps → N JPEG. Pacing SOLO wall-clock (non fidarsi del sink fps).
    int64_t session_start_us = esp_timer_get_time();
    uint32_t photo_count = 0;
    uint32_t acquire_fail_streak = 0;
    int64_t last_shot_us = 0;

    const uint8_t photo_fps = 1;
    const int64_t photo_interval_us = (int64_t)CORE_REC_PHOTO_INTERVAL_MS * 1000LL;
    const bool timer_mode = core_settings_rec_stop_is_timer(settings);
    const uint32_t target_photos = timer_mode ? settings->rec_session_sec : 0;

    if (timer_mode) {
        ESP_LOGI(TAG, "Photo timelapse: 1 fps → %lu JPEG in ~%lu s wall (STOP ignorato, AES=%d) — poi DONE",
                 (unsigned long)target_photos, (unsigned long)settings->rec_session_sec, encrypt ? 1 : 0);
    } else if (s_manual_active || s_manual_starting) {
        ESP_LOGI(TAG, "Photo timelapse: 1 fps (%lu ms) fino a Stop Web (AES=%d)",
                 (unsigned long)CORE_REC_PHOTO_INTERVAL_MS, encrypt ? 1 : 0);
    } else {
        ESP_LOGI(TAG, "Photo timelapse: 1 fps (%lu ms) fino a pin STOP GPIO%d (AES=%d) — poi DONE",
                 (unsigned long)CORE_REC_PHOTO_INTERVAL_MS,
                 (int)settings->rec_gpio_stop, encrypt ? 1 : 0);
    }

    while (!photo_session_should_stop(settings, photo_count)) {
        // Slot assoluto + gap minimo dall'ultimo scatto (doppio vincolo anti-raffica).
        const int64_t slot_due_us = session_start_us + ((int64_t)photo_count * photo_interval_us);
        const int64_t gap_due_us = (last_shot_us > 0) ? (last_shot_us + photo_interval_us) : slot_due_us;
        const int64_t next_photo_us = (slot_due_us > gap_due_us) ? slot_due_us : gap_due_us;

        // Attendi lo slot scartando frame in coda (così lo scatto è "fresco").
        for (;;) {
            int64_t now_wait = esp_timer_get_time();
            if (now_wait >= next_photo_us) {
                break;
            }
            esp_capture_stream_frame_t discard = {
                .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
            };
            if (esp_capture_sink_acquire_frame(pc.sink, &discard, true) == ESP_CAPTURE_ERR_OK &&
                discard.data && discard.size > 0) {
                esp_capture_sink_release_frame(pc.sink, &discard);
            }
            int64_t remain_ms = (next_photo_us - now_wait) / 1000LL;
            if (remain_ms < 1) {
                remain_ms = 1;
            } else if (remain_ms > 50) {
                remain_ms = 50;
            }
            vTaskDelay(pdMS_TO_TICKS((uint32_t)remain_ms));
            core_gpio_log_pin_edges();
        }

#if CORE_REC_OSD_ENABLE
        if (pc.overlay) {
            recorder_osd_refresh(pc.overlay, pc.osd_w, pc.osd_h, pc.osd_font, &pc.osd_last_us);
        }
#endif

        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(pc.sink, &frame, false);
        if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
            acquire_fail_streak++;
            if (err != ESP_CAPTURE_ERR_OK && acquire_fail_streak <= 3) {
                ESP_LOGW(TAG, "Photo: acquire fallita (%d)", (int)err);
            }
            if (acquire_fail_streak >= 10) {
                ESP_LOGE(TAG, "Photo: pipeline fermata dopo %lu errori — fine sessione",
                         (unsigned long)acquire_fail_streak);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        acquire_fail_streak = 0;

        const int64_t shot_us = esp_timer_get_time();
        const int64_t dt_ms = (last_shot_us > 0) ? ((shot_us - last_shot_us) / 1000LL) : 0;
        last_shot_us = shot_us;

        photo_count++;
        char path[160];
        snprintf(path, sizeof(path), "%s_foto%04lu.jpg", session_base, (unsigned long)photo_count);
        if (photo_count == 1) {
            core_note_first_encoded_frame("photo-jpeg", (unsigned long)frame.size);
            ESP_LOGI(TAG, "Photo: primo JPEG %lu byte %ux%u",
                     (unsigned long)frame.size, (unsigned)pc.width, (unsigned)pc.height);
        }
        bool saved = photo_save_jpeg_file(path, frame.data, (size_t)frame.size, aes_key);
        esp_capture_sink_release_frame(pc.sink, &frame);
        if (s_manual_active || s_manual_starting) {
            s_manual_frame_count = photo_count;
            if (saved) {
                snprintf(s_manual_last_path, sizeof(s_manual_last_path), "%s", path);
            }
        }

        uint32_t elapsed_s = (uint32_t)((shot_us - session_start_us) / 1000000LL);
        if (photo_count <= 3 || (photo_count % 5U) == 0U ||
            (timer_mode && photo_count == target_photos)) {
            ESP_LOGI(TAG, "Photo: %lu/%lu dt=%lld ms elapsed=%u s%s",
                     (unsigned long)photo_count,
                     (unsigned long)(timer_mode ? target_photos : photo_count),
                     (long long)dt_ms, (unsigned)elapsed_s,
                     saved ? "" : " (save fail)");
        }
    }

    photo_capture_stop(&pc);
    core_status_led_end_recording(false);
    core_gpio_rec_stop_session_end();

    const uint32_t wall_s =
        (uint32_t)((esp_timer_get_time() - session_start_us) / 1000000LL);
    // Meta: durata wall-clock reale; fps dichiarato 1 (export AVI).
    uint32_t elapsed_s = wall_s;
    if (photo_count > 0) {
        (void)core_sd_write_rec_meta(session_base, elapsed_s, photo_count, photo_fps);
        ESP_LOGI(TAG, "Photo sessione completata: %lu JPEG in %u s wall (target 1 fps) base %s",
                 (unsigned long)photo_count, (unsigned)elapsed_s, session_base);
        return true;
    }
    ESP_LOGW(TAG, "Photo sessione senza immagini salvate");
    return false;
}

static bool recorder_finalize_save(const core_settings_t *settings, const sd_prep_ctx_t *sd_ctx,
                                   bool capture_ok, bool muxer_was_active, uint32_t muxer_frames,
                                   uint32_t dropped_frames, uint32_t duration_sec, bool async_encrypt,
                                   char *out_path, size_t out_path_len) {
    bool saved = false;

    if (!sd_ctx->paths_ok) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d frames=%lu drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)muxer_frames, (unsigned long)dropped_frames);
        return false;
    }

    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGE(TAG, "SD non montata in finalize");
        return false;
    }

    (void)async_encrypt;  // inline AES off: post-encrypt già fatto a muxer_close

    const bool encrypted = recorder_settings_want_encrypt(settings);

    char enc_alt[160];
    snprintf(enc_alt, sizeof(enc_alt), "%s.enc", sd_ctx->final_path);
    struct stat st;
    if (stat(sd_ctx->final_path, &st) != 0) {
        if (stat(enc_alt, &st) == 0 && st.st_size >= (off_t)(CORE_CRYPTO_FILE_HEADER + REC_MIN_MP4_BYTES)) {
            if (rename(enc_alt, sd_ctx->final_path) == 0) {
                ESP_LOGI(TAG, "Recuperato MP4 cifrato %s ← %s", sd_ctx->final_path, enc_alt);
            }
        }
    }
    if (stat(sd_ctx->final_path, &st) != 0) {
        ESP_LOGW(TAG, "File registrazione assente: %s (mux=%d frames=%lu)",
                 sd_ctx->final_path, muxer_was_active, (unsigned long)muxer_frames);
    } else if (muxer_frames == 0 ||
               st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGW(TAG, "File registrazione invalido: %s (%ld byte, mux_frames=%lu) — elimino",
                 sd_ctx->final_path, (long)st.st_size, (unsigned long)muxer_frames);
        remove(sd_ctx->final_path);
    } else {
        saved = true;
        (void)core_sd_write_rec_meta(sd_ctx->final_path, duration_sec, muxer_frames,
                                     settings ? settings->video_fps : 0);
        if (encrypted) {
            ESP_LOGI(TAG, "File salvato cifrato (dual-job PSRAM→SD): %s (%ld bytes, %lu frame mux, %u s)",
                     sd_ctx->final_path, (long)st.st_size, (unsigned long)muxer_frames,
                     (unsigned)duration_sec);
        } else {
            ESP_LOGI(TAG, "File salvato in chiaro: %s (%ld bytes, %lu frame mux, %u s)",
                     sd_ctx->final_path, (long)st.st_size, (unsigned long)muxer_frames,
                     (unsigned)duration_sec);
        }
        if (out_path && out_path_len > 0) {
            snprintf(out_path, out_path_len, "%s", sd_ctx->final_path);
        }
        /* One line the PIR script can trust, including an 8 MiB clip. */
        ESP_LOGI(TAG, "PIR_CLOSED %s %ld", sd_ctx->final_path, (long)st.st_size);
    }

    if (!saved) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d frames=%lu drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)muxer_frames, (unsigned long)dropped_frames);
    }
    return saved;
}

/* VI is down and 3A is gone. UI may go idle and Live may reopen.
 * Muxer moov/fsync continues after this and must not deinit again. */
static void manual_note_capture_halted(void) {
    if (!s_manual_task && !s_manual_active && !s_manual_stopping) {
        return;
    }
    core_snap("capture halt");
    /* DEMO tiene la luma sul selfpath. stop_3A / deinit spengono anche quella
     * e DisableChn non torna: il processo esce e il .tmp resta senza moov. */
    if (core_demo_is_active()) {
        s_manual_finalizing = true;
        s_manual_stopping = false;
        core_snap("ui idle demo");
        return;
    }
    /* Short stop used to return while VI/3A still owned rkisp. The next
     * Registra then saw the sensor busy and Live had nothing to show. */
    core_video_isp_stop_3a();
    esp_capture_release_leftover();
    core_video_isp_rec_end();
    core_video_deinit();
    s_manual_finalizing = true;
    s_manual_stopping = false;
    core_snap("ui idle");
}

// Registrazione in chiaro: capture → muxer → SD (stesso percorso collaudato ~30 fps, no ring dual-job).
static bool recorder_run_core(const core_settings_t *settings, recorder_should_stop_fn_t should_stop,
                              bool async_encrypt, const char *stop_label,
                              char *out_path, size_t out_path_len, uint32_t warmup_frames,
                              bool pir_fast) {
    if (!settings || !should_stop) {
        return false;
    }
    struct RecLink {
        bool share = false;
        bool held = false;
        ~RecLink() {
            if (share) {
                core_rtsp_rec_detach();
            }
            if (held) {
                core_rtsp_release();
            }
        }
    } link;

    sd_prep_ctx_t sd_ctx = {
        .ev = xEventGroupCreate(),
        .core_id = settings->core_id,
        .segment_parts = pir_fast && recorder_want_segment_parts(settings),
    };
    if (!sd_ctx.ev) {
        ESP_LOGE(TAG, "EventGroup SD fallito");
        core_video_isp_rec_end();
        return false;
    }

    core_gpio_rec_stop_session_begin(pir_fast && core_settings_rec_stop_ignores_gpio(settings));

    const bool rec_encrypt = recorder_want_inline_aes(settings);
    s_session_rec_encrypt = rec_encrypt;
    sd_ctx.rec_encrypt = rec_encrypt;
    if (recorder_settings_want_encrypt(settings) && !rec_encrypt) {
        if (pir_fast) {
            ESP_LOGI(TAG, "PIR: niente AES sul primo frame — mp4 in chiaro, flag per cifra batch dopo");
        } else {
            ESP_LOGI(TAG, "AES on-the-fly OFF — mux in chiaro al fps NVS, cifra MRBI a file chiuso");
        }
    }

#if CORE_REC_PLAIN_SD_BENCHMARK
    ESP_LOGW(TAG, ">>> MODALITÀ BENCHMARK: registrazione MP4 in chiaro (no AES) <<<");
    s_plain_closed_part_max = 0;
#else
    if (settings->rec_encrypt && !settings->aes_key_valid) {
        ESP_LOGW(TAG, "Cifratura richiesta ma chiave AES assente — registrazione in chiaro");
    }
    if (rec_encrypt) {
        core_recorder_set_mux_key(settings->aes_key);
        ESP_LOGI(TAG, "Registrazione cifrata: dual-job AES (ring→enc_writer, MRBI+CTR)");
        if (!core_encfs_register()) {
            ESP_LOGW(TAG, "VFS /enc non disponibile — continuo con enc_writer diretto");
        }
    } else {
        ESP_LOGI(TAG, "Registrazione: dual-job plain (ring→plain_writer)%s",
                 recorder_settings_want_encrypt(settings) ? ", AES MRBI a file chiuso" : "");
        s_plain_closed_part_max = 0;
    }
#endif

    const bool rec_fast = core_rec_fast_requested();
    const bool demo_rec = core_demo_is_active();
    bool demo_prep_used = false;
    recorder_capture_t capture = {};
    bool capture_ok = false;

    if (!pir_fast && demo_rec) {
        core_demo_rec_prep_cancel();
        core_rtsp_hold();
        link.held = true;
        ESP_LOGI(TAG, "DEMO: encoder H264 con parametri PIR (GOP<=%u), primi frame tenuti",
                 (unsigned)CORE_REC_PIR_GOP);
    } else if (!pir_fast && !demo_rec) {
        if (core_rtsp_rec_attach(settings) != 0) {
            ESP_LOGE(TAG, "Streamer H.264 non disponibile — registrazione non avviata");
            snprintf(s_manual_error, sizeof(s_manual_error), "Streamer non pronto");
            core_gpio_rec_stop_session_end();
            vEventGroupDelete(sd_ctx.ev);
            return false;
        }
        link.share = true;
    } else {
        core_rtsp_hold();
        link.held = true;
    }
    /* GUI record shares the streamer. PIR still takes the camera alone. */
    bool adopt_vi = false;
    if (!link.share && !pir_fast && !rec_fast) {
        core_settings_t vcfg = *settings;
        core_settings_video_normalize(&vcfg);
        uint16_t hw = vcfg.video_width;
        uint16_t hh = vcfg.video_height;
        uint8_t hf = vcfg.video_fps;
        rec_apply_selected_encode_size(&hw, &hh, &hf);
        if (core_live_take_for_record(hw, hh, hf)) {
            adopt_vi = true;
            core_snap("rec vi kept");
        }
    }
    if (!adopt_vi && !link.share && !demo_prep_used) {
        /* Live MJPEG + 3A must be off rkisp before this EnableChn. DEMO luma ch1 resta up. */
        core_live_force_release();
        if (!core_live_wait_idle(CORE_LIVE_RELEASE_WAIT_MS)) {
            ESP_LOGW(TAG, "Live/VI ancora attivo — secondo rilascio prima dell'avvio");
            core_live_force_release();
        }
        if (!core_live_wait_idle(800)) {
            ESP_LOGE(TAG, "Live/VI ancora attivo — non apro la registrazione");
            snprintf(s_manual_error, sizeof(s_manual_error), "Camera occupata — riprova");
            esp_capture_release_leftover();
            core_video_isp_stop_3a();
            core_video_deinit();
            core_gpio_rec_stop_session_end();
            vEventGroupDelete(sd_ctx.ev);
            return false;
        }
        if (CORE_REC_AFTER_LIVE_MS > 0) {
            ESP_LOGI(TAG, "Pausa %u ms dopo Live idle, poi apro VI",
                     (unsigned)CORE_REC_AFTER_LIVE_MS);
            vTaskDelay(pdMS_TO_TICKS(CORE_REC_AFTER_LIVE_MS));
        }
        core_snap("rec open vi");
        /* rec-begin / un secondo pir-arm bloccano la DEMO per decine di secondi.
         * La luma ha già il 3A. */
        if (!rec_fast && !demo_rec) {
            core_video_isp_rec_begin();
        }
    }
    /* Mount exFAT in the background. The encoder writes the RAM ring as
     * soon as it has a frame and does not wait for the card. */
    xTaskCreate(sd_prep_task, "sd_prep", CORE_SD_PREP_TASK_STACK, &sd_ctx, CORE_SD_PREP_TASK_PRIO, nullptr);
    if (pir_fast) {
        ESP_LOGI(TAG, "PIR: cattura nel ring H.264 (%u KB) senza attendere il mount exFAT",
                 (unsigned)(CORE_PIR_H264_RING_BYTES / 1024U));
    }

    int64_t ae_discard_until_us = 0;
    if (!capture_ok) {
        capture_ok = link.share ? recorder_capture_from_rtsp(&capture, settings)
                                : recorder_capture_start_psram(&capture, warmup_frames, settings,
                                                               pir_fast || demo_rec, adopt_vi);
    }
    core_snap(capture_ok ? "capture start ok" : "capture start fail");
    const uint32_t ae_settle_ms =
        (rec_fast || capture.from_live || capture.from_rtsp || demo_prep_used)
            ? 0u
            : (demo_rec ? 0u
                        : (pir_fast ? ((core_video_module_is_af())
                                           ? (uint32_t)CORE_REC_AE_SETTLE_MS_PIR_AF
                                           : (uint32_t)CORE_REC_AE_SETTLE_MS_PIR)
                                    : (uint32_t)CORE_REC_AE_SETTLE_MS));

    if (capture_ok && !pir_fast) {
        core_status_led_set_mode(CORE_LED_RECORDING);
        ESP_LOGI(TAG, "Capture avviata in %lld ms dal boot (warmup=%lu frame, SD in parallelo)",
                 (long long)core_boot_elapsed_ms(), (unsigned long)warmup_frames);
    } else if (capture_ok) {
        ESP_LOGI(TAG, "PIR capture in %lld ms (%ux%u GOP=%u, AE %lu ms%s, segmento %lu min)",
                 (long long)core_boot_elapsed_ms(), capture.video_width, capture.video_height,
                 (unsigned)capture.video_gop, (unsigned long)ae_settle_ms,
                 rec_fast ? ", rec-fast skip settle" : (core_video_module_is_af() ? ", AF" : ""),
                 (unsigned long)(settings->rec_max_duration_sec / 60U));
    }

    // Prime obbligatorio: alloca buffer encoder (~6 MB) PRIMA del ring PSRAM.
    if (capture_ok && demo_prep_used) {
        capture.warmup_left = 0;
    } else if (capture_ok && !recorder_capture_prime(&capture, CORE_REC_PRIME_TIMEOUT_MS)) {
        recorder_capture_stop(&capture);
        capture_ok = false;
    } else if (capture_ok) {
        (void)core_isp_cal_startup_wb_check();
        if (ae_settle_ms > 0) {
            ae_discard_until_us = esp_timer_get_time() + ((int64_t)ae_settle_ms * 1000LL);
            ESP_LOGI(TAG, "AE settle %lu ms prima dei frame utili (fps=%u%s)",
                     (unsigned long)ae_settle_ms, (unsigned)capture.video_fps,
                     pir_fast ? ", PIR" : ", web");
        }
    }

    if (!capture_ok) {
        ESP_LOGE(TAG, "Capture non avviato — %ux%u@%u (nessun fallback 1280x960)",
                 (unsigned)capture.video_width, (unsigned)capture.video_height,
                 (unsigned)capture.video_fps);
        if (!s_last_enc_err[0]) {
            snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                     "Capture %ux%u@%u failed (no 1280x960 fallback)",
                     (unsigned)(capture.video_width ? capture.video_width : s_last_enc_w),
                     (unsigned)(capture.video_height ? capture.video_height : s_last_enc_h),
                     (unsigned)(capture.video_fps ? capture.video_fps : s_last_enc_fps));
        }
        snprintf(s_manual_error, sizeof(s_manual_error), "%s", s_last_enc_err);
        rec_note_encode(s_last_enc_w, s_last_enc_h, s_last_enc_fps, s_last_enc_err);
        core_status_led_end_recording(core_gpio_is_mode_config());
        xEventGroupWaitBits(sd_ctx.ev, SD_PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        if (!capture.from_rtsp) {
            esp_capture_release_leftover();
            core_video_isp_stop_3a();
            core_video_deinit();
            core_video_isp_rec_end();
        }
        return false;
    }

    if ((s_manual_active || s_manual_starting) && !capture.from_live) {
        s_manual_ready = true;
        s_manual_starting = false;
        core_snap("avvio ready");
    }
    /* Backup 3A after the POST can return. HAL already started it
     * before EnableChn; this must not sit on Avvio. A Live handoff
     * already has that 3A — do not start another one. */
    if (capture_ok && !rec_fast && !demo_rec && !capture.from_live && !capture.from_rtsp &&
        !demo_prep_used) {
        core_video_isp_rec_after_vi();
    }
    if (capture_ok && demo_rec && !demo_prep_used && !capture.from_rtsp) {
        core_video_isp_pir_lock();
    }
    core_h264_buffer_t ring_buf = {};
    SemaphoreHandle_t ring_mtx = xSemaphoreCreateMutex();
    if (!ring_mtx) {
        ESP_LOGE(TAG, "Mutex ring non disponibile");
        recorder_capture_stop(&capture);
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        core_video_isp_rec_end();
        return false;
    }

    /* Ring first even when the card is already mounted, so the opening
     * seconds are not dropped while the mp4 is created. JOB2 flushes the
     * ring in order once the path exists, then keeps writing the file. */
    bool sd_prep_done = false;
    const bool enc_pir = pir_fast || demo_rec;
    const uint32_t sd_lag_ms = enc_pir ? CORE_REC_SD_LAG_MS_PIR : CORE_REC_SD_LAG_MS;
    const uint32_t sd_burst = enc_pir
                                  ? (rec_encrypt ? CORE_REC_SD_BURST_FRAMES_PIR_ENC
                                                 : CORE_REC_SD_BURST_FRAMES_PIR)
                                  : CORE_REC_SD_BURST_FRAMES;

    if (!core_h264_buffer_init_recording(&ring_buf, enc_pir, rec_encrypt)) {
        ESP_LOGE(TAG, "Ring PSRAM non disponibile");
        recorder_capture_stop(&capture);
        core_status_led_end_recording(core_gpio_is_mode_config());
        vSemaphoreDelete(ring_mtx);
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        core_video_isp_rec_end();
        return false;
    }

    if (demo_prep_used) {
        (void)demo_prep_splice_preroll(&ring_buf, &capture, ring_mtx);
    }

    ESP_LOGI(TAG, "Registrazione dual-job [%s]: %dx%d@%dfps → ring → SD (lag %u ms, burst %u)",
             rec_encrypt ? "AES" : "plain",
             capture.video_width, capture.video_height, capture.video_fps,
             (unsigned)sd_lag_ms, (unsigned)sd_burst);

    recorder_muxer_t muxer = {};
    bool muxer_was_active = false;

    rec_dual_job_ctx_t job = {
        .capture = &capture,
        .muxer = &muxer,
        .sd_ctx = &sd_ctx,
        .ring = &ring_buf,
        .ring_mtx = ring_mtx,
        .should_stop = should_stop,
        .muxer_was_active = &muxer_was_active,
        .pir_fast = pir_fast,
        .rec_encrypt = rec_encrypt,
        .ae_discard_until_us = ae_discard_until_us,
        .sd_lag_ms = sd_lag_ms,
        .sd_burst = sd_burst,
        .segment_max_sec = pir_fast ? settings->rec_max_duration_sec : 0,
        .ignore_gpio_stop = pir_fast && core_settings_rec_stop_ignores_gpio(settings),
        .session_limit_sec = (pir_fast && core_settings_rec_stop_is_timer(settings))
                                  ? settings->rec_session_sec
                                  : 0U,
        .settings = settings,
    };
    job.sd_prep_done = sd_prep_done;
    if (demo_prep_used && capture.last_out_pts > 0) {
        job.producer_pts_max = capture.last_out_pts;
    }

    TaskHandle_t cap_task = nullptr;
    TaskHandle_t sd_task = nullptr;
    s_active_rec_job = &job;
    if (xTaskCreatePinnedToCore(rec_capture_job_task, "rec_cap", CORE_REC_CAPTURE_TASK_STACK, &job,
                                CORE_REC_CAPTURE_TASK_PRIO, &cap_task, 0) != pdPASS ||
        xTaskCreatePinnedToCore(rec_sd_job_task, "rec_sd", CORE_REC_SD_TASK_STACK, &job,
                                CORE_REC_SD_TASK_PRIO, &sd_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Avvio task dual-job fallito");
        s_active_rec_job = nullptr;
        recorder_capture_stop(&capture);
        core_h264_buffer_deinit(&ring_buf);
        vSemaphoreDelete(ring_mtx);
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        core_video_isp_rec_end();
        return false;
    }

    if (capture.from_live) {
        for (int i = 0; i < 40 && capture.kept_frames == 0; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_manual_frame_count = capture.kept_frames;
        if (s_manual_active || s_manual_starting) {
            s_manual_ready = true;
            s_manual_starting = false;
            core_snap("avvio ready");
        }
        ESP_LOGI(TAG, "Handoff pronto, frame=%lu", (unsigned long)capture.kept_frames);
    }

    if (pir_fast) {
        core_status_led_set_mode(CORE_LED_RECORDING);
        if (core_settings_rec_stop_is_timer(settings)) {
            ESP_LOGI(TAG, "Stop sessione: timer %lu s (pin STOP ignorato) — poi DONE",
                     (unsigned long)settings->rec_session_sec);
        } else {
            ESP_LOGI(TAG, "Stop sessione: pin STOP GPIO%d — poi DONE", (int)settings->rec_gpio_stop);
        }
        if (settings->video_fps <= CORE_VIDEO_FPS_LOW_MAX) {
            ESP_LOGI(TAG, "Timelapse video: max %u fps @ %ux%u",
                     (unsigned)settings->video_fps,
                     (unsigned)settings->video_width, (unsigned)settings->video_height);
        }
    }

    int64_t last_status_log = esp_timer_get_time();
    int64_t rec_start_us = esp_timer_get_time();
    job.session_start_us = rec_start_us;

    for (;;) {
        int64_t now_loop = esp_timer_get_time();
        if (job.session_limit_sec > 0) {
            int64_t elapsed_sec = (now_loop - job.session_start_us) / 1000000LL;
            if (elapsed_sec >= (int64_t)job.session_limit_sec) {
                job.user_stop = true;
                ESP_LOGI(TAG, "Durata sessione %lu s raggiunta — fine registrazione",
                         (unsigned long)job.session_limit_sec);
                break;
            }
        }
        if (should_stop()) {
            job.user_stop = true;
            break;
        }
        core_gpio_log_pin_edges();

        if (!sd_prep_done && (xEventGroupGetBits(sd_ctx.ev) & SD_PREP_DONE_BIT)) {
            sd_prep_done = true;
            job.sd_prep_done = true;
        }

        int64_t now = esp_timer_get_time();
        s_manual_frame_count = capture.kept_frames;
        if (now - last_status_log >= ((int64_t)REC_STATUS_LOG_MS * 1000)) {
            last_status_log = now;
            uint32_t rec_ms = (uint32_t)((now - rec_start_us) / 1000);
            uint32_t ring_pct = 0;
            xSemaphoreTake(ring_mtx, portMAX_DELAY);
            ring_pct = core_h264_buffer_fill_percent(&ring_buf);
            xSemaphoreGive(ring_mtx);
            uint32_t dur_sec = 0;
            if (capture.pts_origin_us > 0) {
                dur_sec = (uint32_t)((esp_timer_get_time() - capture.pts_origin_us) / 1000000);
            } else if (capture.frame_dur_ms > 0) {
                dur_sec = capture.kept_frames * capture.frame_dur_ms / 1000U;
            }
            float mux_fps = rec_ms > 0 ? (job.muxer_frames * 1000.0f) / (float)rec_ms : 0.0f;
            ESP_LOGI(TAG, "Stato rec [%s]: kept=%lu mux=%lu seg=%lu ring=%u%% mux_fps~%.1f ~%u s drop=%lu",
                     rec_encrypt ? "AES" : "plain",
                     (unsigned long)capture.kept_frames,
                     (unsigned long)job.muxer_frames,
                     (unsigned long)job.segments_closed,
                     (unsigned)ring_pct,
                     mux_fps,
                     (unsigned)dur_sec,
                     (unsigned long)job.dropped_frames);
            if (job.dropped_frames > 0) {
                ESP_LOGW(TAG, "Frame persi (ring pieno): %lu — rischio salti nel video",
                         (unsigned long)job.dropped_frames);
            }
            if (pir_fast && capture.kept_frames == 0 && rec_ms >= 8000) {
                ESP_LOGE(TAG, "PIR: pipeline morta (0 frame in %u ms) — stop sessione",
                         (unsigned)rec_ms);
                job.user_stop = true;
                break;
            }
            // SD mai pronta: mount/percorsi bloccati (FAT corrotta?) — inutile continuare.
            if (!sd_prep_done && rec_ms >= 30000) {
                ESP_LOGE(TAG, "SD prep bloccata da %u ms (mount appeso?) — abort sessione",
                         (unsigned)rec_ms);
                job.user_stop = true;
                break;
            }
            if (sd_prep_done && !sd_ctx.paths_ok) {
                ESP_LOGE(TAG, "SD/percorsi non disponibili — abort sessione (nessun file possibile)");
                job.user_stop = true;
                break;
            }
            // Muxer mai aperto con SD pronta e frame in ring: scrittura SD bloccata.
            if (sd_prep_done && sd_ctx.paths_ok && job.muxer_frames == 0 &&
                capture.kept_frames > 0 && rec_ms >= 60000) {
                ESP_LOGE(TAG, "Muxer mai aperto in %u ms (SD bloccata in scrittura?) — abort sessione",
                         (unsigned)rec_ms);
                job.user_stop = true;
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Stop registrazione (%s) — drain capture + flush SD",
             stop_label ? stop_label : "?");

    const bool stopped_by_user = should_stop();
    int64_t stop_at_us = stopped_by_user ? esp_timer_get_time() : 0;

    core_status_led_notify_rec_stop();
    job.user_stop = true;

    const uint32_t cap_wait_ms = pir_fast ? CORE_REC_STOP_CAPTURE_WAIT_MS_PIR
                                          : CORE_REC_STOP_CAPTURE_WAIT_MS;
    const int cap_wait_iters = (int)(cap_wait_ms / 10);
    for (int i = 0; i < cap_wait_iters && !job.capture_done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!job.capture_done) {
        ESP_LOGW(TAG, "JOB1 capture: timeout uscita (%u ms) — cleanup forzato",
                 (unsigned)cap_wait_ms);
    }

    const uint32_t sd_wait_ms = pir_fast ? CORE_REC_STOP_SD_WAIT_MS_PIR : CORE_REC_STOP_SD_WAIT_MS;
    const int64_t sd_wait_deadline = esp_timer_get_time() + ((int64_t)sd_wait_ms * 1000);
    int64_t last_flush_log = esp_timer_get_time();
    while (!job.sd_done && esp_timer_get_time() < sd_wait_deadline) {
        int64_t now = esp_timer_get_time();
        if (stopped_by_user && (now - last_flush_log) >= 1000000) {
            last_flush_log = now;
            uint32_t ring_pct = 0;
            xSemaphoreTake(ring_mtx, portMAX_DELAY);
            ring_pct = core_h264_buffer_fill_percent(&ring_buf);
            xSemaphoreGive(ring_mtx);
            ESP_LOGI(TAG, "Flush SD: ring=%u%% mux=%lu (%lld ms da stop)",
                     (unsigned)ring_pct, (unsigned long)job.muxer_frames,
                     (long long)((now - stop_at_us) / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!job.sd_done) {
        ESP_LOGW(TAG, "JOB2 SD: timeout drain (%u ms) — attesa chiusura muxer",
                 (unsigned)sd_wait_ms);
        const int64_t extra_deadline =
            esp_timer_get_time() + ((int64_t)CORE_REC_STOP_SD_WAIT_MS * 1000LL);
        while (!job.sd_done && esp_timer_get_time() < extra_deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!job.sd_done) {
            ESP_LOGE(TAG, "JOB2 SD ancora attivo — rischio MP4 senza moov");
        }
    }

    if (!job.sd_prep_done) {
        xEventGroupWaitBits(sd_ctx.ev, SD_PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        job.sd_prep_done = true;
    }

    if (muxer.active) {
        recorder_muxer_close(&muxer);
        (void)rec_mux_commit_file(sd_ctx.final_path);
        recorder_finish_closed_mp4(settings, sd_ctx.final_path, pir_fast);
        rec_mrbin_finalize(&job, sd_ctx.final_path);
    } else if (!job.muxer_closed_on_sd && muxer_was_active) {
        ESP_LOGI(TAG, "Muxer già chiuso da JOB2 SD");
    }
    if (stopped_by_user) {
        ESP_LOGI(TAG, "Muxer chiuso (%lld ms da stop)",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000));
    }

    /* Moov è già su disco. Solo ora si spegne l'encoder: se il VI resta
     * appeso, il file è comunque un MP4. */
    recorder_capture_stop(&capture);
    if (stopped_by_user) {
        ESP_LOGI(TAG, "Pipeline capture fermata (%lld ms da stop, kept=%lu)",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000),
                 (unsigned long)capture.kept_frames);
        if (capture.from_rtsp) {
            s_manual_finalizing = true;
            s_manual_stopping = false;
            core_snap("ui idle");
        } else {
            manual_note_capture_halted();
        }
    }

    // Durata .meta = timeline PTS muxata (non frames/fps: gonfiava la lista e
    // non coincideva con la durata reale dell'MP4).
    uint32_t timeline_sec = 0;
    if (job.producer_pts_max > 0) {
        timeline_sec = (job.producer_pts_max + 500U) / 1000U;
    } else if (capture.last_out_pts > 0) {
        timeline_sec = (capture.last_out_pts + 500U) / 1000U;
    } else if (capture.frame_dur_ms > 0 && capture.kept_frames > 0) {
        timeline_sec = capture.kept_frames * capture.frame_dur_ms / 1000U;
    }
    if (job.session_limit_sec > 0 && timeline_sec > job.session_limit_sec) {
        timeline_sec = job.session_limit_sec;
    }
    ESP_LOGI(TAG, "Durata utile: %u s (mux=%lu kept=%lu pts_max=%lu ms limit=%lu)",
             (unsigned)timeline_sec, (unsigned long)job.muxer_frames,
             (unsigned long)capture.kept_frames, (unsigned long)job.producer_pts_max,
             (unsigned long)job.session_limit_sec);
    if (job.muxer_frames > 0) {
        ESP_LOGI(TAG, "Durata timeline: %lu frame mux, fps ~%u → %u s",
                 (unsigned long)job.muxer_frames,
                 (unsigned)recorder_capture_effective_fps(&capture),
                 (unsigned)timeline_sec);
    }
    if (job.muxer_frames == 0 && muxer_was_active) {
        ESP_LOGW(TAG, "Muxer attivo ma 0 frame scritti su SD — file probabilmente assente");
    }
    // Se JOB2 è ancora vivo (appeso su SD), liberare ring/mutex causerebbe
    // use-after-free e panic al suo risveglio: meglio un leak, tanto segue DONE.
    if (job.sd_done && job.capture_done) {
        core_h264_buffer_deinit(&ring_buf);
        vSemaphoreDelete(ring_mtx);
        vEventGroupDelete(sd_ctx.ev);
    } else {
        ESP_LOGW(TAG, "Task registrazione ancora attivi (cap=%d sd=%d) — skip cleanup ring (evito panic)",
                 (int)job.capture_done, (int)job.sd_done);
    }

    core_gpio_rec_stop_session_end();
    s_active_rec_job = nullptr;
    rec_sd_free_frame_copy(&job);

    ESP_LOGI(TAG, "Registrazione chiusa: %lu catturati (~%u s utili), %lu su SD, seg=%lu drop=%lu (fps config %u)",
             (unsigned long)capture.kept_frames, (unsigned)timeline_sec,
             (unsigned long)job.muxer_frames, (unsigned long)job.segments_closed,
             (unsigned long)job.dropped_frames,
             (unsigned)capture.video_fps);
    if (capture.kept_frames > job.muxer_frames) {
        ESP_LOGW(TAG, "Delta capture→SD: %lu frame in ring non scritti su SD (salti probabili a fine clip)",
                 (unsigned long)(capture.kept_frames - job.muxer_frames));
    }
    if (job.dropped_frames > 0) {
        ESP_LOGW(TAG, "ATTENZIONE: %lu frame persi — video con buchi/salti probabili",
                 (unsigned long)job.dropped_frames);
    }
    if (job.segments_closed > 0) {
        ESP_LOGI(TAG, "Sessione multi-segmento: file _part01 … _part%02lu su SD (apri tutti in ordine)",
                 (unsigned long)(job.segments_closed + 1));
    }

    if (stopped_by_user) {
        ESP_LOGI(TAG, "Teardown stop utente in %lld ms",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000));
    }

    bool saved = recorder_finalize_save(settings, &sd_ctx, true, muxer_was_active, job.muxer_frames,
                                        job.dropped_frames, timeline_sec, async_encrypt,
                                        out_path, out_path_len);
    core_snap("muxer saved");
    core_status_led_end_recording(core_gpio_is_mode_config());
    /* User stop already dropped 3A and VI so Live could reopen.
     * A second rec-end here would kill that new 3A. */
    if (!s_manual_finalizing && !capture.from_rtsp) {
        core_video_isp_rec_end();
    }
    return saved;
}

static void manual_rec_task(void *arg) {
    manual_rec_arg_t *ctx = (manual_rec_arg_t *)arg;
    char saved_path[128] = {0};
    bool saved;
    if (ctx->settings.rec_media_mode == CORE_REC_MEDIA_PHOTO) {
        ESP_LOGI(TAG, "Web rec: timelapse foto JPEG %ux%u @ 1 fps (nessun fallback H264)",
                 (unsigned)ctx->settings.video_width, (unsigned)ctx->settings.video_height);
        saved = recorder_run_photo_session(&ctx->settings);
        if (saved && s_manual_last_path[0]) {
            snprintf(saved_path, sizeof(saved_path), "%s", s_manual_last_path);
        } else if (!saved && !s_last_enc_err[0]) {
            snprintf(s_last_enc_err, sizeof(s_last_enc_err),
                     "Timelapse foto JPEG %ux%u non salvato",
                     (unsigned)ctx->settings.video_width,
                     (unsigned)ctx->settings.video_height);
        }
    } else {
        ESP_LOGI(TAG, "Web rec: video H264 %ux%u @ %u fps",
                 (unsigned)ctx->settings.video_width, (unsigned)ctx->settings.video_height,
                 (unsigned)ctx->settings.video_fps);
        const uint32_t warmup =
            core_demo_is_active() ? CORE_REC_WARMUP_DROP_FRAMES_PIR : CORE_REC_WARMUP_DROP_FRAMES;
        saved = recorder_run_core(&ctx->settings, manual_should_stop, true, "web stop",
                                  saved_path, sizeof(saved_path), warmup, false);
    }
    s_manual_last_saved = saved;
    if (saved) {
        snprintf(s_manual_last_path, sizeof(s_manual_last_path), "%s", saved_path);
        s_manual_error[0] = 0;
    } else if (s_last_enc_err[0]) {
        snprintf(s_manual_error, sizeof(s_manual_error), "%s", s_last_enc_err);
    } else {
        snprintf(s_manual_error, sizeof(s_manual_error), "Registrazione non salvata");
    }
    if (!s_manual_finalizing && !core_demo_is_active()) {
        core_video_deinit();
    }
    core_snap("rec task exit");
    free(ctx);
    s_manual_task = nullptr;
    s_manual_active = false;
    s_manual_starting = false;
    s_manual_stopping = false;
    s_manual_finalizing = false;
    s_manual_ready = false;
    if (s_manual_ev) {
        xEventGroupSetBits(s_manual_ev, MANUAL_REC_DONE_BIT);
    }
    vTaskDelete(nullptr);
}

bool core_recorder_manual_mark_starting(void) {
    if (s_manual_active || s_manual_task) {
        return false;
    }
    s_manual_stop = false;
    s_manual_stopping = false;
    s_manual_finalizing = false;
    s_manual_starting = true;
    s_manual_ready = false;
    s_manual_last_saved = false;
    s_manual_frame_count = 0;
    s_manual_last_path[0] = 0;
    s_manual_error[0] = 0;
    s_last_enc_err[0] = 0;
    return true;
}

void core_recorder_manual_clear_starting(void) {
    if (s_manual_task || s_manual_active) {
        return;
    }
    s_manual_starting = false;
    s_manual_stopping = false;
    s_manual_ready = false;
}

bool core_recorder_manual_start(const core_settings_t *settings) {
    if (!settings) {
        return false;
    }
    if (s_manual_task) {
        ESP_LOGW(TAG, "Registrazione manuale già attiva");
        return false;
    }
    if (s_manual_active && !s_manual_starting) {
        ESP_LOGW(TAG, "Registrazione manuale già attiva");
        return false;
    }
    if (!s_manual_ev) {
        s_manual_ev = xEventGroupCreate();
        if (!s_manual_ev) {
            return false;
        }
    }

    manual_rec_arg_t *arg = (manual_rec_arg_t *)malloc(sizeof(manual_rec_arg_t));
    if (!arg) {
        return false;
    }
    memcpy(&arg->settings, settings, sizeof(*settings));

    if (!s_manual_starting) {
        s_manual_stop = false;
        s_manual_last_saved = false;
        s_manual_frame_count = 0;
        s_manual_last_path[0] = 0;
        s_manual_error[0] = 0;
        s_last_enc_err[0] = 0;
    }
    s_manual_stopping = false;
    s_manual_ready = false;
    xEventGroupClearBits(s_manual_ev, MANUAL_REC_DONE_BIT);

    /* active BEFORE create: task can finish (fail) first; never set active after death. */
    s_manual_active = true;
    s_manual_starting = true;
    if (xTaskCreate(manual_rec_task, "web_rec", MANUAL_REC_TASK_STACK, arg,
                    MANUAL_REC_TASK_PRIO, &s_manual_task) != pdPASS) {
        s_manual_task = nullptr;
        s_manual_active = false;
        s_manual_starting = false;
        snprintf(s_manual_error, sizeof(s_manual_error), "Avvio task registrazione fallito");
        free(arg);
        return false;
    }
    ESP_LOGI(TAG, "Registrazione manuale avviata da Web");
    return true;
}

bool core_recorder_manual_stop(void) {
    if (!s_manual_active && !s_manual_starting && !s_manual_task) {
        return false;
    }
    s_manual_stop = true;
    s_manual_stopping = true;
    return true;
}

bool core_recorder_manual_wait_done(uint32_t timeout_ms) {
    if (!s_manual_ev) {
        return !s_manual_active;
    }
    if (s_manual_active) {
        xEventGroupWaitBits(s_manual_ev, MANUAL_REC_DONE_BIT, pdTRUE, pdTRUE,
                            pdMS_TO_TICKS(timeout_ms));
    }
    return !s_manual_active;
}

bool core_recorder_manual_is_active(void) {
    return s_manual_active || s_manual_starting || s_manual_task;
}

void core_recorder_manual_get_status(core_recorder_manual_status_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->finalizing = s_manual_finalizing;
    /* While moov runs, the task is still alive but capture is already down.
     * Report idle so Stop returns; is_active() still blocks a second start. */
    out->active = !s_manual_finalizing &&
                  (s_manual_active || s_manual_starting || s_manual_task);
    out->starting = s_manual_starting && !s_manual_ready && !s_manual_finalizing;
    out->stopping = s_manual_stopping && !s_manual_finalizing;
    out->last_saved = s_manual_last_saved;
    out->frame_count = s_manual_frame_count;
    snprintf(out->last_path, sizeof(out->last_path), "%s", s_manual_last_path);
    snprintf(out->error, sizeof(out->error), "%s", s_manual_error);
    out->enc_w = s_last_enc_w;
    out->enc_h = s_last_enc_h;
    out->enc_fps = s_last_enc_fps;
}

bool core_recorder_run_session(const core_settings_t *settings) {
    if (!settings) {
        return false;
    }

    if (settings->rec_media_mode == CORE_REC_MEDIA_PHOTO) {
        return recorder_run_photo_session(settings);
    }

    const char *stop_label = core_settings_rec_stop_is_timer(settings) ? "timer" : "stop GPIO";
    return recorder_run_core(settings, rec_pir_job_should_stop, false, stop_label,
                             nullptr, 0, CORE_REC_WARMUP_DROP_FRAMES_PIR, true);
}

static bool jpeg_parse_sof_size(const uint8_t *data, size_t len, uint16_t *out_w, uint16_t *out_h) {
    if (!data || len < 20 || !out_w || !out_h) {
        return false;
    }
    size_t i = 0;
    while (i + 9 < len) {
        if (data[i] != 0xFF) {
            ++i;
            continue;
        }
        while (i < len && data[i] == 0xFF) {
            ++i;
        }
        if (i >= len) {
            break;
        }
        uint8_t marker = data[i++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        uint16_t seg_len = ((uint16_t)data[i] << 8) | data[i + 1];
        if (seg_len < 2 || i + seg_len > len) {
            break;
        }
        if ((marker == 0xC0 || marker == 0xC1 || marker == 0xC2) && seg_len >= 7) {
            *out_h = ((uint16_t)data[i + 3] << 8) | data[i + 4];
            *out_w = ((uint16_t)data[i + 5] << 8) | data[i + 6];
            return *out_w > 0 && *out_h > 0;
        }
        i += seg_len;
    }
    return false;
}

typedef struct {
    char path[192];
} export_mux_ctx_t;

static int export_mux_path_handler(esp_muxer_slice_info_t *info, void *ctx) {
    export_mux_ctx_t *m = (export_mux_ctx_t *)ctx;
    if (!info || !m) {
        return -1;
    }
    snprintf(info->file_path, info->len, "%s", m->path);
    return 0;
}

typedef struct {
    FILE *fp;
    char *buf;
} export_plain_writer_t;

static void *export_plain_open(char *path) {
    export_plain_writer_t *w = (export_plain_writer_t *)calloc(1, sizeof(*w));
    if (!w) {
        return nullptr;
    }
    w->fp = fopen(path, "wb");
    if (!w->fp) {
        free(w);
        return nullptr;
    }
    w->buf = (char *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (w->buf) {
        setvbuf(w->fp, w->buf, _IOFBF, 64 * 1024);
    }
    return w;
}

static int export_plain_write(void *writer, void *buffer, int len) {
    export_plain_writer_t *w = (export_plain_writer_t *)writer;
    if (!w || !w->fp || !buffer || len <= 0) {
        return -1;
    }
    size_t n = fwrite(buffer, 1, (size_t)len, w->fp);
    return (n == (size_t)len) ? len : -1;
}

static int export_plain_seek(void *writer, uint64_t pos) {
    export_plain_writer_t *w = (export_plain_writer_t *)writer;
    if (!w || !w->fp) {
        return -1;
    }
    return fseek(w->fp, (long)pos, SEEK_SET) == 0 ? 0 : -1;
}

static int export_plain_close(void *writer) {
    export_plain_writer_t *w = (export_plain_writer_t *)writer;
    if (!w) {
        return 0;
    }
    if (w->fp) {
        fflush(w->fp);
        fsync(fileno(w->fp));
        fclose(w->fp);
    }
    if (w->buf) {
        free(w->buf);
    }
    free(w);
    return 0;
}

// Carica un JPEG da SD; se ha header MRBI lo decifra (serve aes_key).
static bool export_load_jpeg_plain(const char *path, const uint8_t *aes_key,
                                   uint8_t **buf, size_t *buf_cap, size_t *out_len) {
    if (!path || !buf || !buf_cap || !out_len) {
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    uint32_t magic = 0;
    size_t magic_n = fread(&magic, 1, sizeof(magic), fp);
    bool encrypted = (magic_n == sizeof(magic) && magic == CORE_CRYPTO_MAGIC);
    if (encrypted) {
        if (!aes_key) {
            fclose(fp);
            ESP_LOGE(TAG, "Export AVI: JPEG cifrato senza chiave: %s", path);
            return false;
        }
        uint8_t iv[16];
        if (fread(iv, 1, sizeof(iv), fp) != sizeof(iv)) {
            fclose(fp);
            return false;
        }
        long plain_sz = sz - (long)CORE_CRYPTO_FILE_HEADER;
        if (plain_sz <= 0) {
            fclose(fp);
            return false;
        }
        if ((size_t)plain_sz > *buf_cap) {
            uint8_t *nb = (uint8_t *)heap_caps_realloc(*buf, (size_t)plain_sz + 64,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                nb = (uint8_t *)realloc(*buf, (size_t)plain_sz + 64);
            }
            if (!nb) {
                fclose(fp);
                return false;
            }
            *buf = nb;
            *buf_cap = (size_t)plain_sz + 64;
        }
        size_t rd = fread(*buf, 1, (size_t)plain_sz, fp);
        fclose(fp);
        if (rd != (size_t)plain_sz) {
            return false;
        }
        core_crypto_ctx_t ctx;
        if (!core_crypto_init_ctx(&ctx, aes_key, iv)) {
            return false;
        }
        bool ok = core_crypto_crypt_at(&ctx, *buf, rd, 0);
        core_crypto_deinit_ctx(&ctx);
        if (!ok) {
            return false;
        }
        *out_len = rd;
        return true;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    if ((size_t)sz > *buf_cap) {
        uint8_t *nb = (uint8_t *)heap_caps_realloc(*buf, (size_t)sz + 64,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            nb = (uint8_t *)realloc(*buf, (size_t)sz + 64);
        }
        if (!nb) {
            fclose(fp);
            return false;
        }
        *buf = nb;
        *buf_cap = (size_t)sz + 64;
    }
    size_t rd = fread(*buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        return false;
    }
    *out_len = rd;
    return true;
}

bool core_recorder_export_jpegs_avi(const char *out_abs_avi,
                                    char jpeg_abs_paths[][192],
                                    int jpeg_count,
                                    uint8_t fps,
                                    const uint8_t *aes_key,
                                    bool encrypt_out,
                                    core_recorder_export_progress_fn progress,
                                    void *progress_user) {
    if (!out_abs_avi || !jpeg_abs_paths || jpeg_count <= 0) {
        return false;
    }
    if (encrypt_out && !aes_key) {
        ESP_LOGE(TAG, "Export AVI: encrypt_out senza chiave AES");
        return false;
    }
    if (fps == 0) {
        fps = CORE_PHOTO_EXPORT_FPS;
    }

    size_t buf_cap = 0;
    uint8_t *buf = nullptr;
    size_t jpeg_len = 0;
    if (!export_load_jpeg_plain(jpeg_abs_paths[0], aes_key, &buf, &buf_cap, &jpeg_len)) {
        ESP_LOGE(TAG, "Export AVI: impossibile aprire/decifrare %s", jpeg_abs_paths[0]);
        free(buf);
        return false;
    }

    uint16_t width = 0;
    uint16_t height = 0;
    if (!jpeg_parse_sof_size(buf, jpeg_len, &width, &height)) {
        ESP_LOGE(TAG, "Export AVI: SOF JPEG non trovato in %s", jpeg_abs_paths[0]);
        free(buf);
        return false;
    }

    remove(out_abs_avi);

    export_mux_ctx_t mux_ctx = {};
    snprintf(mux_ctx.path, sizeof(mux_ctx.path), "%s", out_abs_avi);

    // AVI + MJPEG: riproducibile su Windows/VLC; MP4+MJPEG spesso risulta "corrotto".
    avi_muxer_register();
    avi_muxer_config_t avi_cfg = {
        .base_config = {
            .muxer_type = ESP_MUXER_TYPE_AVI,
            .slice_duration = 3600000,
            .url_pattern = nullptr,
            .url_pattern_ex = export_mux_path_handler,
            .ctx = &mux_ctx,
            .ram_cache_size = encrypt_out ? CORE_MUXER_RAM_CACHE_ENC_BYTES
                                          : CORE_MUXER_RAM_CACHE_PLAIN_BYTES,
            .no_key_frame_verify = true,
        },
        .index_type = AVI_MUXER_INDEX_AT_END,
    };

    esp_muxer_handle_t handle = esp_muxer_open((esp_muxer_config_t *)&avi_cfg, sizeof(avi_cfg));
    if (!handle) {
        ESP_LOGE(TAG, "Export AVI: esp_muxer_open fallita");
        free(buf);
        return false;
    }

    if (encrypt_out) {
        core_recorder_set_mux_key(aes_key);
    }
    esp_muxer_file_writer_t writer = {
        .on_open = encrypt_out ? enc_writer_open : export_plain_open,
        .on_write = encrypt_out ? enc_writer_write : export_plain_write,
        .on_seek = encrypt_out ? enc_writer_seek : export_plain_seek,
        .on_close = encrypt_out ? enc_writer_close : export_plain_close,
    };
    if (esp_muxer_set_file_writer(handle, &writer) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        free(buf);
        ESP_LOGE(TAG, "Export AVI: set_file_writer fallita");
        return false;
    }

    esp_muxer_video_stream_info_t vinfo = {
        .codec = ESP_MUXER_VDEC_MJPEG,
        .width = width,
        .height = height,
        .fps = fps,
        .min_packet_duration = 1000U / (uint32_t)fps,
        .codec_spec_info = nullptr,
        .spec_info_len = 0,
    };
    int stream_index = -1;
    if (esp_muxer_add_video_stream(handle, &vinfo, &stream_index) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        free(buf);
        ESP_LOGE(TAG, "Export AVI: add_video_stream MJPEG fallita");
        return false;
    }

    ESP_LOGI(TAG, "Export AVI MJPEG: %d frame %ux%u @%u fps → %s (AES=%d)",
             jpeg_count, (unsigned)width, (unsigned)height, (unsigned)fps, out_abs_avi,
             encrypt_out ? 1 : 0);

    if (progress) {
        progress(0, jpeg_count, progress_user);
    }

    bool ok = true;
    const uint32_t frame_ms = 1000U / (uint32_t)fps;
    int written = 0;

    for (int i = 0; i < jpeg_count; ++i) {
        if (i > 0) {
            if (!export_load_jpeg_plain(jpeg_abs_paths[i], aes_key, &buf, &buf_cap, &jpeg_len)) {
                ESP_LOGW(TAG, "Export AVI: skip %s (load/decrypt)", jpeg_abs_paths[i]);
                if (progress) {
                    progress(i + 1, jpeg_count, progress_user);
                }
                continue;
            }
        }

        esp_muxer_video_packet_t pkt = {
            .data = buf,
            .len = (int)jpeg_len,
            .pts = (uint32_t)written * frame_ms,
            .dts = (uint32_t)written * frame_ms,
            .key_frame = true,
        };
        if (esp_muxer_add_video_packet(handle, stream_index, &pkt) != ESP_MUXER_ERR_OK) {
            ESP_LOGE(TAG, "Export AVI: add_video_packet fallita al frame %d", i + 1);
            ok = false;
            break;
        }
        written++;
        if (progress) {
            progress(i + 1, jpeg_count, progress_user);
        }
        if ((i + 1) % 20 == 0 || i + 1 == jpeg_count) {
            ESP_LOGI(TAG, "Export AVI: %d/%d", i + 1, jpeg_count);
        }
        if ((i & 3) == 0) {
            vTaskDelay(1);
        }
    }

    esp_muxer_close(handle);
    if (buf) {
        free(buf);
    }

    if (!ok || written == 0) {
        remove(out_abs_avi);
        return false;
    }

    struct stat st;
    if (stat(out_abs_avi, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "Export AVI: file assente o troppo piccolo");
        remove(out_abs_avi);
        return false;
    }

    ESP_LOGI(TAG, "Export AVI completato: %s (%ld bytes, %d frame%s)",
             out_abs_avi, (long)st.st_size, written, encrypt_out ? ", cifrato" : "");
    return true;
}
