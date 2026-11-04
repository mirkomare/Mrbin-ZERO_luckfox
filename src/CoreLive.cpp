#include "CoreLive.h"
#include "CoreRtsp.h"
#include "CoreDemo.h"
#include "hal_capture.h"
#include "CoreConfig.h"
#include "CoreIspCal.h"
#include "CoreVideo.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static const char *TAG = "core_live";
static const char *MJPEG_BOUNDARY = "mrbinframe";

#define LIVE_STREAM_WORKER_STACK  32768
#define LIVE_STREAM_WORKER_PRIO   5
// Se la camera non produce frame, non bloccare per sempre il worker HTTP.
#define LIVE_FIRST_FRAME_TIMEOUT_MS  15000
#define LIVE_NO_FRAME_TIMEOUT_MS     8000

static const core_settings_t *s_settings = nullptr;
static bool s_rtsp_held = false;
static bool s_use_preview = false;
static bool s_use_bypass = false;

static void live_rtsp_hold(void) {
    if (s_rtsp_held) {
        return;
    }
    core_rtsp_hold();
    s_rtsp_held = true;
}

static void live_rtsp_drop(void) {
    if (!s_rtsp_held) {
        return;
    }
    s_rtsp_held = false;
    core_rtsp_release();
}
static esp_capture_handle_t s_capture = nullptr;
static esp_capture_video_src_if_t *s_vsrc = nullptr;
static esp_capture_sink_handle_t s_sink = nullptr;
static bool s_running = false;
static volatile bool s_abort_requested = false;
static volatile int s_live_sock = -1;

void core_live_bind_settings(const core_settings_t *s) {
    s_settings = s;
}

void core_live_preview_size(uint16_t rec_w, uint16_t rec_h, uint16_t *out_w, uint16_t *out_h) {
    if (!out_w || !out_h) {
        return;
    }
    if (rec_w == 0 || rec_h == 0) {
        *out_w = (uint16_t)CORE_LIVE_WIDTH;
        *out_h = (uint16_t)CORE_LIVE_HEIGHT;
        return;
    }
    // Fit in CORE_LIVE_WIDTH×HEIGHT mantenendo l'aspect della registrazione
    // (stesso crop ISP: es. 1280×720 e 1920×1080 → anteprima 16:9, non 800×640 4:3).
    uint32_t w = (uint32_t)CORE_LIVE_WIDTH;
    uint32_t h = (w * (uint32_t)rec_h) / (uint32_t)rec_w;
    if (h > (uint32_t)CORE_LIVE_HEIGHT) {
        h = (uint32_t)CORE_LIVE_HEIGHT;
        w = (h * (uint32_t)rec_w) / (uint32_t)rec_h;
    }
    w &= ~1u;
    h &= ~1u;
    if (w < 160u) {
        w = 160u;
    }
    if (h < 120u) {
        h = 120u;
    }
    *out_w = (uint16_t)w;
    *out_h = (uint16_t)h;
}

static QueueHandle_t s_stream_queue = nullptr;
static SemaphoreHandle_t s_worker_ready = nullptr;
static TaskHandle_t s_worker_task = nullptr;
static volatile bool s_job_active = false;
/* True only while core_live_stop is inside 3A/VI teardown. */
static volatile bool s_teardown = false;
/* True once MJPEG frames are flowing. 3A may be stopped early only then.
 * Killing 3A during EnableChn leaves the worker blocked in poll and the
 * next Registra sees the sensor busy (anteprima nera). */
static volatile bool s_streaming = false;
static volatile bool s_busy_drill = false;
/* Client gone, VI+3A still up. Record may adopt this pipeline. */
static volatile bool s_vi_parked = false;
/* Record owns the VI. Live must not DisableChn it. */
static volatile bool s_vi_owned_by_rec = false;
/* Next stream end parks instead of stream-off. */
static volatile bool s_handoff = false;
/* force_release: stream end must DisableChn, not park. */
static volatile bool s_force_release = false;
/* Parked VI resumed; skip the cold-start frame discard. */
static volatile bool s_resumed = false;
static SemaphoreHandle_t s_live_mu = nullptr;

static void live_record_geometry(const core_settings_t *s, uint16_t *w, uint16_t *h, uint8_t *fps) {
    core_settings_t v = {};
    if (s) {
        v = *s;
        core_settings_video_normalize(&v);
    } else {
        core_settings_video_set_defaults(&v);
    }
    if (v.video_width > CORE_VIDEO_WIDTH_MAX) {
        v.video_width = (uint16_t)CORE_VIDEO_WIDTH_MAX;
    }
    if (v.video_height > CORE_VIDEO_HEIGHT_MAX) {
        v.video_height = (uint16_t)CORE_VIDEO_HEIGHT_MAX;
    }
    if (v.video_fps < CORE_VIDEO_FPS_MIN || v.video_fps > CORE_VIDEO_FPS_MAX) {
        v.video_fps = (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    }
    if (w) {
        *w = v.video_width;
    }
    if (h) {
        *h = v.video_height;
    }
    if (fps) {
        *fps = v.video_fps;
    }
}

static void live_lock(void) {
    if (!s_live_mu) {
        s_live_mu = xSemaphoreCreateMutex();
    }
    if (s_live_mu) {
        xSemaphoreTake(s_live_mu, portMAX_DELAY);
    }
}

/* HTTP Registra must not sit on Avvio if the worker already holds the lock
 * inside VI teardown. A timed miss means that worker owns the release. */
static bool live_lock_ms(uint32_t ms) {
    if (!s_live_mu) {
        s_live_mu = xSemaphoreCreateMutex();
    }
    if (!s_live_mu) {
        return false;
    }
    return xSemaphoreTake(s_live_mu, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void live_unlock(void) {
    if (s_live_mu) {
        xSemaphoreGive(s_live_mu);
    }
}

typedef struct {
    httpd_req_t *req;
} live_stream_job_t;

bool core_live_start(void) {
#ifdef LUCKFOX_RKMPI
    if (s_running && s_use_bypass && hal_live_running()) {
        s_abort_requested = false;
        return true;
    }
    if (s_running && s_use_preview && !s_use_bypass && hal_preview_flip_matches()) {
        s_abort_requested = false;
        return true;
    }
    s_abort_requested = false;
    s_streaming = false;
    uint16_t rec_w = s_settings ? s_settings->video_width : (uint16_t)CORE_VIDEO_WIDTH_DEFAULT;
    uint16_t rec_h = s_settings ? s_settings->video_height : (uint16_t)CORE_VIDEO_HEIGHT_DEFAULT;
    uint16_t pw = (uint16_t)CORE_LIVE_WIDTH;
    uint16_t ph = (uint16_t)CORE_LIVE_HEIGHT;
    core_live_preview_size(rec_w, rec_h, &pw, &ph);
    if (!core_video_is_ready() && !core_video_init()) {
        ESP_LOGE(TAG, "Video init fallita");
        return false;
    }
    if (s_use_preview && !s_use_bypass) {
        hal_preview_stop();
        s_use_preview = false;
        s_running = false;
    }
    if (hal_live_start((int)pw, (int)ph, CORE_LIVE_FPS) == 0) {
        s_use_bypass = true;
        s_use_preview = true;
        s_running = true;
        ESP_LOGI(TAG, "Live MJPEG bypass %ux%u @ %d fps",
                 (unsigned)pw, (unsigned)ph, CORE_LIVE_FPS);
        return true;
    }
    ESP_LOGW(TAG, "Bypass non disponibile, live sul selfpath");
    s_use_bypass = false;
    if (hal_preview_start((int)pw, (int)ph, CORE_LIVE_FPS, 0) != 0) {
        ESP_LOGE(TAG, "Anteprima selfpath non partita");
        s_use_preview = false;
        s_running = false;
        return false;
    }
    s_use_preview = true;
    s_running = true;
    ESP_LOGI(TAG, "Live MJPEG selfpath %ux%u @ %d fps",
             (unsigned)pw, (unsigned)ph, CORE_LIVE_FPS);
    return true;
#else
    if (s_vi_owned_by_rec || s_handoff) {
        if (s_vi_owned_by_rec && !esp_capture_mpi_busy()) {
            s_vi_owned_by_rec = false;
        } else {
            return false;
        }
    }
    if (s_running && s_capture) {
        s_abort_requested = false;
        if (s_vi_parked) {
            s_vi_parked = false;
            s_resumed = true;
            core_snap("live resume");
            ESP_LOGI(TAG, "Live ripreso senza stream-on");
        }
        return true;
    }
    s_abort_requested = false;
    s_streaming = false;
    s_resumed = false;

    live_rtsp_hold();
    if (!core_video_prepare_for_stream(s_settings)) {
        ESP_LOGE(TAG, "Video init fallita");
        live_rtsp_drop();
        return false;
    }

    /* SC3336 Live opens VI at the record size and pipe rate. Registra then
     * swaps MJPEG→H.264 without DisableChn. JPEG output stays at CORE_LIVE_FPS.
     * Other sensors keep the small preview. */
    uint16_t live_w = (uint16_t)CORE_LIVE_WIDTH;
    uint16_t live_h = (uint16_t)CORE_LIVE_HEIGHT;
    uint8_t keep_fps = (uint8_t)CORE_LIVE_FPS;
    bool sc_full = false;
    if (s_settings && (core_video_sensor_kind() == CORE_VIDEO_SENSOR_SC3336 ||
                       core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX415)) {
        live_record_geometry(s_settings, &live_w, &live_h, &keep_fps);
        sc_full = true;
    } else if (s_settings) {
        uint16_t pw = s_settings->video_width;
        uint16_t ph = s_settings->video_height;
        if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708) {
            core_video_imx708_encode_size(pw, ph, &pw, &ph);
        }
        core_live_preview_size(pw, ph, &live_w, &live_h);
    }

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = 4,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    s_vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!s_vsrc) {
        ESP_LOGE(TAG, "Sorgente V4L2 non disponibile");
        esp_video_enc_unregister_default();
        live_rtsp_drop();
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = s_vsrc,
        .share_overlay = false,
    };

    esp_capture_err_t err = esp_capture_open(&cap_cfg, &s_capture);
    if (sc_full) {
        esp_capture_set_preview_fps((int)CORE_LIVE_FPS);
    }
    if (err != ESP_CAPTURE_ERR_OK || !s_capture) {
        ESP_LOGE(TAG, "esp_capture_open fallita (%d)", err);
        free(s_vsrc);
        s_vsrc = nullptr;
        esp_video_enc_unregister_default();
        live_rtsp_drop();
        return false;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_MJPEG,
            .width = live_w,
            .height = live_h,
            .fps = keep_fps,
        },
    };

    err = esp_capture_sink_setup(s_capture, 0, &sink_cfg, &s_sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita (%d)", err);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        free(s_vsrc);
        s_vsrc = nullptr;
        esp_video_enc_unregister_default();
        live_rtsp_drop();
        return false;
    }

    esp_capture_sink_enable(s_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);

    /* Hold idle-wait from EnableChn onward. Record must not open 1080
     * while this start still owns rkisp. */
    s_running = true;
    err = esp_capture_start(s_capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start fallita (%d)", err);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        free(s_vsrc);
        s_vsrc = nullptr;
        s_sink = nullptr;
        esp_video_enc_unregister_default();
        s_running = false;
        /* Partial VI (SYS up, EnableChn failed) must not stay owned. */
        esp_capture_release_leftover();
        core_video_deinit();
        live_rtsp_drop();
        return false;
    }

    // SC3336: HFLIP/VFLIP sta sul subdev, non sulla coda rkisp_mainpath.
    // Reapply dopo VI+VENC (il driver può riscrivere 0x3221 al set-fmt).
    core_video_reapply_flip();
    if (s_abort_requested) {
        ESP_LOGI(TAG, "Live abort durante avvio VI — skip 3A, rilascio CSI");
        s_running = true;
        if (s_force_release) {
            core_live_stop();
        } else {
            s_streaming = false;
            s_vi_parked = true;
            s_abort_requested = false;
            core_snap("live park");
        }
        return false;
    }
    /* 3A was started before EnableChn. Leave it up while the first
     * frames are pulled (stopping earlier wedges rkisp). */
    core_video_isp_live_after_vi();
    /* live-3a may rewrite CPROC 128. Put the saved calibration back on the preview. */
    (void)core_isp_cal_apply_active();

    s_running = true;
    ESP_LOGI(TAG, "Live MJPEG %dx%d out %d fps (VI keep %u)",
             (int)live_w, (int)live_h, CORE_LIVE_FPS, (unsigned)keep_fps);
    return true;
#endif
}

#ifndef _WIN32
static bool rkaiq_3a_alive(void) {
    DIR *d = opendir("/proc");
    if (!d) {
        return false;
    }
    bool alive = false;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char comm[32] = {0};
        if (fgets(comm, sizeof(comm), f) && strncmp(comm, "rkaiq_3A_serve", 14) == 0) {
            alive = true;
        }
        fclose(f);
        if (alive) {
            break;
        }
    }
    closedir(d);
    return alive;
}
#endif

/* stop-3a kill -9 is not instant. Call it only after VI is down.
 * Doing it first leaves rockit's SYS_Exit thread in select() on the 3A
 * stream-off event: that join never returns, Registra stays on Avvio,
 * Stop times out, and Live cannot reopen. */
static void wait_3a_gone(uint32_t cap_ms) {
#ifndef _WIN32
    const int64_t t0 = esp_timer_get_time();
    bool saw = false;
    while (rkaiq_3a_alive()) {
        saw = true;
        if ((uint32_t)((esp_timer_get_time() - t0) / 1000) >= cap_ms) {
            ESP_LOGW(TAG, "3A ancora vivo dopo %u ms", (unsigned)cap_ms);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (saw) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
#else
    (void)cap_ms;
#endif
}

void core_live_stop(void) {
#ifdef LUCKFOX_RKMPI
    if (s_use_preview || s_use_bypass) {
        live_lock();
        if (s_use_bypass) {
            hal_live_stop();
        } else {
            hal_preview_stop();
        }
        s_use_bypass = false;
        s_use_preview = false;
        s_running = false;
        s_streaming = false;
        s_teardown = false;
        s_vi_parked = false;
        s_handoff = false;
        s_force_release = false;
        live_unlock();
        ESP_LOGI(TAG, "Live fermato");
        return;
    }
#endif
    if (s_vi_owned_by_rec && esp_capture_mpi_busy()) {
        return;
    }
    live_lock();
    if (!s_running && !s_capture && !s_vsrc && !s_teardown) {
        s_streaming = false;
        live_unlock();
        return;
    }
    /* Stay "busy" until VI is actually down. Clearing s_running first let
     * Record open 1080 while 3A still owned rkisp. */
    s_teardown = true;
    s_streaming = false;

    /* Drop VI while 3A is still alive so rockit gets the stream-off event.
     * kill -9 before DisableChn/SYS_Exit makes that select() wait forever. */
    core_snap("live_stop begin");
    if (s_capture) {
        esp_capture_stop(s_capture);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        s_sink = nullptr;
    } else {
        esp_capture_release_leftover();
    }

    if (s_vsrc) {
        free(s_vsrc);
        s_vsrc = nullptr;
    }

    esp_video_enc_unregister_default();
    /* After vi_release the next /live/stream must re-open VI. Leaving the
     * subsystem "initialized" makes capture_start hang (black img, empty
     * reply, Live clicks look dead because the worker never returns). */
    core_video_deinit();
    core_snap("live_stop vi done");

    core_video_isp_stop_3a();
    wait_3a_gone(250);
    core_snap("live_stop 3a done");
    s_running = false;
    s_teardown = false;
    s_vi_parked = false;
    s_handoff = false;
    s_force_release = false;
    live_unlock();
    live_rtsp_drop();
    ESP_LOGI(TAG, "Live fermato");
}

/* Stream client is gone. Park VI when Record may adopt it. A forced
 * release still DisableChn before 3A is killed (core_live_stop). */
static void core_live_release_client(void) {
#ifdef LUCKFOX_RKMPI
    if (s_use_preview) {
        core_live_stop();
        return;
    }
#endif
    if (s_force_release || s_vi_owned_by_rec) {
        if (!s_vi_owned_by_rec) {
            core_live_stop();
        }
        s_vi_parked = false;
        return;
    }
    if (s_capture && esp_capture_mpi_busy()) {
        s_streaming = false;
        s_abort_requested = false;
        s_vi_parked = true;
        s_running = true;
        core_snap("live park");
        ESP_LOGI(TAG, "Live sospeso — VI resta acceso");
        return;
    }
    core_live_stop();
}

/* Worker is stuck in GetStream/EnableChn poll (abort is not visible there).
 * Drop VI while 3A can still ack stream-off, then stop 3A.
 * Short waits only — this is the failure path, not Avvio. */
static void core_live_hard_release(void) {
#ifdef LUCKFOX_RKMPI
    if (s_use_preview) {
        core_live_stop();
        return;
    }
#endif
    if (s_vi_owned_by_rec && esp_capture_mpi_busy()) {
        core_snap("hard skip rec vi");
        return;
    }
    s_abort_requested = true;
    s_teardown = true;
    s_streaming = false;
    core_snap("hard_release begin");
    /* Same lock as core_live_stop. If the worker already holds it inside
     * VI teardown, do not block Registra and do not kill 3A out from
     * under that DisableChn (the select() then never returns). */
    if (!live_lock_ms(1500)) {
        core_snap("hard_release lock busy");
        ESP_LOGW(TAG, "Live lock busy — il worker sta già chiudendo VI");
        return;
    }
    if (!s_running && !s_capture && !s_vsrc && !esp_capture_mpi_busy()) {
        s_streaming = false;
        s_teardown = false;
        live_unlock();
        core_snap("hard_release already idle");
        return;
    }
    core_isp_cal_close_device();
    if (esp_capture_mpi_busy()) {
        /* EnableChn owns rkisp. Cancel and wait; do not kill 3A yet. */
        esp_capture_release_leftover();
        live_unlock();
        for (int i = 0; i < 60 && esp_capture_mpi_busy(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (!live_lock_ms(1500)) {
            core_snap("hard_release lock busy");
            return;
        }
    }
    if (s_capture) {
        esp_capture_stop(s_capture);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        s_sink = nullptr;
    } else {
        esp_capture_release_leftover();
    }
    if (s_vsrc) {
        free(s_vsrc);
        s_vsrc = nullptr;
    }
    esp_video_enc_unregister_default();
    core_video_deinit();
    core_video_isp_stop_3a();
    wait_3a_gone(200);
    s_running = false;
    s_streaming = false;
    s_teardown = false;
    live_unlock();
    for (int i = 0; i < 24 && s_job_active; ++i) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    core_snap("hard_release done");
    live_rtsp_drop();
    ESP_LOGW(TAG, "VI+3A rilasciati (hard) job=%d mpi=%d running=%d",
             (int)s_job_active, esp_capture_mpi_busy(), (int)s_running);
}

void core_live_request_stop(void) {
    s_abort_requested = true;
    /* Do not shutdown(s_live_sock). Closing it from the Record thread while
     * the worker is in send_chunk reset the POST (errore di rete) and could
     * drop httpd. The worker notices the flag between frames. */
}

bool core_live_consume_busy_drill(void) {
    const bool v = s_busy_drill;
    s_busy_drill = false;
    return v;
}

void core_live_force_release(void) {
#ifdef LUCKFOX_RKMPI
    if (s_use_preview) {
        s_abort_requested = true;
        s_force_release = true;
        if (!core_live_wait_idle(CORE_LIVE_RELEASE_WAIT_MS)) {
            core_live_stop();
        }
        s_force_release = false;
        return;
    }
#endif
    if (s_vi_owned_by_rec) {
        if (esp_capture_mpi_busy()) {
            core_snap("force skip rec vi");
            return;
        }
        s_vi_owned_by_rec = false;
    }
#ifndef _WIN32
    if (access("/tmp/mrbin-force-busy", F_OK) == 0) {
        unlink("/tmp/mrbin-force-busy");
        s_busy_drill = true;
        ESP_LOGW(TAG, "Busy forzato — rilascio VI+3A e ripristino anteprima");
        core_live_hard_release();
        return;
    }
#endif
    if (!s_running && !s_teardown && !s_job_active && !s_capture &&
        !esp_capture_mpi_busy()) {
        return;
    }
    s_force_release = true;
    s_handoff = false;
    if (s_vi_parked && !s_job_active) {
        core_snap("park stop");
        core_live_stop();
        s_vi_parked = false;
        s_force_release = false;
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    core_snap("force_release begin");
    core_live_request_stop();
    /* Do not stop-3a here. The live worker owns that, inside core_live_stop.
     * A parallel mrbin-isp stop-3a raced DisableChn and the process exited
     * before Registra could answer. */
    if (!core_live_wait_idle(CORE_LIVE_RELEASE_WAIT_MS)) {
        ESP_LOGW(TAG, "Live non idle dopo %u ms (running=%d tear=%d job=%d) — hard release",
                 (unsigned)CORE_LIVE_RELEASE_WAIT_MS,
                 (int)s_running, (int)s_teardown, (int)s_job_active);
        core_live_hard_release();
        return;
    }
    const unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    s_force_release = false;
    core_snap("force_release idle");
    ESP_LOGI(TAG, "Live VI idle in %u ms", ms);
}

bool core_live_is_running(void) {
    return s_running || s_teardown || s_vi_parked;
}

bool core_live_can_handoff(const core_settings_t *s) {
    if (s_use_preview) {
        return false;
    }
    if (!s || s_vi_owned_by_rec) {
        return false;
    }
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_UNKNOWN) {
        core_video_detect_sensor();
    }
    if (core_video_sensor_kind() != CORE_VIDEO_SENSOR_SC3336 &&
        core_video_sensor_kind() != CORE_VIDEO_SENSOR_IMX415) {
        return false;
    }
    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t fps = 0;
    live_record_geometry(s, &w, &h, &fps);
    return esp_capture_vi_matches((int)w, (int)h, (int)fps) != 0;
}

void core_live_expect_handoff(void) {
    s_force_release = false;
    s_handoff = true;
    s_abort_requested = true;
    core_snap("expect handoff");
}

bool core_live_take_for_record(uint16_t w, uint16_t h, uint8_t fps) {
    if (s_use_preview) {
        return false;
    }
    if (s_vi_owned_by_rec) {
        return false;
    }
    s_force_release = false;
    s_handoff = true;
    s_abort_requested = true;
    /* Park happens before the multipart close, which can sit on a dead
     * socket. Do not wait for that close — GetStream is already done. */
    for (int i = 0; i < 50 && s_job_active && !s_vi_parked; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_job_active && !s_vi_parked) {
        core_snap("handoff busy");
        return false;
    }
    if (!esp_capture_vi_matches((int)w, (int)h, (int)fps) || !s_capture) {
        s_handoff = false;
        return false;
    }
    if (!live_lock_ms(200)) {
        return false;
    }
    s_vi_parked = false;
    s_running = false;
    s_streaming = false;
    s_teardown = false;
    s_capture = nullptr;
    s_sink = nullptr;
    if (s_vsrc) {
        free(s_vsrc);
        s_vsrc = nullptr;
    }
    s_vi_owned_by_rec = true;
    s_handoff = false;
    live_unlock();
    live_rtsp_drop();
    core_snap("live handed");
    ESP_LOGI(TAG, "Live VI consegnato a H.264 %ux%u@%u senza stream-off",
             (unsigned)w, (unsigned)h, (unsigned)fps);
    return true;
}

void core_live_abandon_handoff(void) {
    s_vi_owned_by_rec = false;
    s_handoff = false;
    if (esp_capture_mpi_busy()) {
        esp_capture_release_leftover();
    }
    core_video_isp_stop_3a();
    core_snap("handoff abandon");
}

bool core_live_wait_idle(uint32_t timeout_ms) {
    int64_t t0 = esp_timer_get_time();
    while (s_running || s_teardown) {
        if ((uint32_t)((esp_timer_get_time() - t0) / 1000) >= timeout_ms) {
            ESP_LOGW(TAG, "Timeout attesa live idle (running=%d tear=%d job=%d)",
                     (int)s_running, (int)s_teardown, (int)s_job_active);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static int live_take(esp_capture_stream_frame_t *frame) {
#ifdef LUCKFOX_RKMPI
    if (s_use_bypass || s_use_preview) {
        uint8_t *p = nullptr;
        int n = 0;
        const int got = s_use_bypass ? hal_live_acquire(&p, &n, 200)
                                     : hal_preview_acquire(&p, &n, 200);
        if (got != 0 || !p || n <= 0) {
            free(p);
            return -1;
        }
        frame->stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO;
        frame->data = p;
        frame->size = n;
        frame->pts = 0;
        return 0;
    }
#endif
    if (esp_capture_sink_acquire_frame(s_sink, frame, true) != ESP_CAPTURE_ERR_OK ||
        !frame->data || frame->size <= 0) {
        return -1;
    }
    return 0;
}

static void live_give(esp_capture_stream_frame_t *frame) {
#ifdef LUCKFOX_RKMPI
    if (s_use_preview) {
        free(frame->data);
        frame->data = nullptr;
        return;
    }
#endif
    esp_capture_sink_release_frame(s_sink, frame);
}

static esp_err_t core_live_stream_impl(httpd_req_t *req) {
    s_live_sock = httpd_req_to_sockfd(req);
#ifndef _WIN32
    if (s_live_sock >= 0) {
        /* Short send timeout so Record's abort is seen within one frame,
         * without shutdown() on this socket (that reset the POST). */
        struct timeval snd_to = {.tv_sec = 0, .tv_usec = 400000};
        (void)setsockopt(s_live_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    }
#endif
    if (!core_live_start()) {
        s_live_sock = -1;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera non disponibile");
        return ESP_FAIL;
    }

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };

    // Attendi il primo frame PRIMA del multipart. Scarta frame AE neri
    // (no_wait: se la coda muore non restiamo bloccati per sempre).
    // IMX708: +AWB — le finestre IPA di default non trovano il bianco indoor.
    const int64_t t_start = esp_timer_get_time();
    int64_t settle_us = (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708)
                            ? 1400000
                            : 1600000;
    if (s_resumed) {
        settle_us = 0;
        s_resumed = false;
    }
    const int64_t ae_until = t_start + settle_us;
    bool got_first = false;
    while (!got_first) {
        if (s_abort_requested) {
            s_abort_requested = false;
            core_live_release_client();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Live interrotto");
            return ESP_FAIL;
        }
        const bool still_ae = esp_timer_get_time() < ae_until;
        if (live_take(&frame) == 0) {
            if (still_ae) {
                live_give(&frame);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            got_first = true;
            s_streaming = true;
            break;
        }
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
        if (elapsed_ms >= LIVE_FIRST_FRAME_TIMEOUT_MS) {
            ESP_LOGE(TAG, "Timeout primo frame MJPEG (%lu ms) — camera non produce immagini "
                          "(modulo AF? abilita CONFIG_CAMERA_OV5647_ENABLE_MOTOR_BY_GPIO0)",
                     (unsigned long)elapsed_ms);
            if (s_handoff && !s_force_release) {
                core_live_release_client();
            } else {
                core_live_stop();
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Nessun frame dalla camera (timeout)");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    char content_type[72];
    snprintf(content_type, sizeof(content_type),
             "multipart/x-mixed-replace; boundary=%s", MJPEG_BOUNDARY);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Connection", "close");

    char part_hdr[96];
    uint32_t sent_frames = 0;
    int64_t t_last_ok = esp_timer_get_time();

    // Primo frame già acquisito
    {
        (void)core_isp_cal_startup_wb_check();
        ESP_LOGI(TAG, "Primo frame MJPEG: %d byte", frame.size);
        int hlen = snprintf(part_hdr, sizeof(part_hdr),
                            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                            MJPEG_BOUNDARY, frame.size);
        if (httpd_resp_send_chunk(req, part_hdr, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)frame.data, frame.size) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnesso al primo frame");
            live_give(&frame);
            httpd_resp_send_chunk(req, nullptr, 0);
            core_live_release_client();
            return ESP_OK;
        }
        live_give(&frame);
        sent_frames = 1;
        t_last_ok = esp_timer_get_time();
    }

    while (true) {
        if (s_abort_requested) {
            ESP_LOGI(TAG, "Live interrotto (registrazione o takeover nuovo client)");
            s_abort_requested = false;
            break;
        }

        if (live_take(&frame) != 0) {
            const uint32_t gap_ms = (uint32_t)((esp_timer_get_time() - t_last_ok) / 1000);
            if (gap_ms >= LIVE_NO_FRAME_TIMEOUT_MS) {
                ESP_LOGE(TAG, "Timeout frame MJPEG dopo %lu ms senza immagini",
                         (unsigned long)gap_ms);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (frame.size <= 0 || !frame.data) {
            live_give(&frame);
            continue;
        }
        if (s_abort_requested) {
            live_give(&frame);
            break;
        }

        int hlen = snprintf(part_hdr, sizeof(part_hdr),
                            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                            MJPEG_BOUNDARY, frame.size);

        if (httpd_resp_send_chunk(req, part_hdr, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)frame.data, frame.size) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnesso dopo %lu frame MJPEG", (unsigned long)sent_frames);
            live_give(&frame);
            break;
        }

        live_give(&frame);
        sent_frames++;
        t_last_ok = esp_timer_get_time();
        /* VENC MJPEG often ignores dst fps — cap here so Live stays ~5 fps / ~0.5 MB/s. */
        {
            const int64_t min_us = 1000000LL / (int64_t)(CORE_LIVE_FPS > 0 ? CORE_LIVE_FPS : 5);
            const int64_t now_us = esp_timer_get_time();
            const int64_t due_us = t_last_ok + min_us;
            if (due_us > now_us + 1000) {
                uint32_t left = (uint32_t)((due_us - now_us) / 1000);
                while (left > 0 && !s_abort_requested) {
                    const uint32_t step = left > 40 ? 40 : left;
                    vTaskDelay(pdMS_TO_TICKS(step));
                    left -= step;
                }
                if (s_abort_requested) {
                    break;
                }
            }
        }
        if ((sent_frames % 30) == 0) {
            ESP_LOGI(TAG, "Live stream: %lu frame MJPEG inviati", (unsigned long)sent_frames);
        }
    }

    /* Park VI (3A stays up) unless this is a forced release. The multipart
     * close can block in send; Record adopts the pipeline without waiting
     * for that close only after this returns — the worker then drops
     * s_job_active. A closed socket fails the last send immediately. */
    core_live_release_client();
    s_live_sock = -1;
    (void)httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static void live_stream_worker(void *) {
    ESP_LOGI(TAG, "Worker live stream avviato");
    while (true) {
        xSemaphoreGive(s_worker_ready);

        live_stream_job_t job;
        if (xQueueReceive(s_stream_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Live stream async — connessione client");
        s_job_active = true;
        core_live_stream_impl(job.req);
        s_live_sock = -1;
        s_job_active = false;

        if (httpd_req_async_handler_complete(job.req) != ESP_OK) {
            ESP_LOGW(TAG, "httpd_req_async_handler_complete fallita");
        }
    }
}

bool core_live_async_init(void) {
    if (s_worker_task) {
        return true;
    }

    s_worker_ready = xSemaphoreCreateCounting(1, 0);
    if (!s_worker_ready) {
        return false;
    }

    s_stream_queue = xQueueCreate(1, sizeof(live_stream_job_t));
    if (!s_stream_queue) {
        vSemaphoreDelete(s_worker_ready);
        s_worker_ready = nullptr;
        return false;
    }

    if (xTaskCreate(live_stream_worker, "live_stream", LIVE_STREAM_WORKER_STACK, nullptr,
                    LIVE_STREAM_WORKER_PRIO, &s_worker_task) != pdPASS) {
        vQueueDelete(s_stream_queue);
        vSemaphoreDelete(s_worker_ready);
        s_stream_queue = nullptr;
        s_worker_ready = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Async live stream pronto");
    return true;
}

esp_err_t core_live_stream(httpd_req_t *req) {
    if (!core_live_async_init()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Live worker non disponibile");
        return ESP_FAIL;
    }

    httpd_req_t *async_req = nullptr;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async live fallito");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_worker_ready, pdMS_TO_TICKS(100)) != pdTRUE) {
        // Navigazione tra pagine: il client precedente è quasi sempre già andato ma il
        // worker non se n'è ancora accorto. Takeover: abort del vecchio stream e attesa
        // del rilascio, così l'anteprima parte al primo caricamento senza refresh manuale.
        ESP_LOGI(TAG, "Live stream attivo — takeover: interrompo il client precedente");
        s_abort_requested = true;
        if (xSemaphoreTake(s_worker_ready, pdMS_TO_TICKS(4000)) != pdTRUE) {
            ESP_LOGW(TAG, "Live stream già attivo — takeover fallito");
            /* Respond on the async request. Sending on the original req
             * after begin() resets the socket and the img stays black. */
            httpd_resp_set_status(async_req, "503 Service Unavailable");
            httpd_resp_set_type(async_req, "text/plain");
            httpd_resp_send(async_req, "Live stream busy", HTTPD_RESP_USE_STRLEN);
            httpd_req_async_handler_complete(async_req);
            return ESP_OK;
        }
    }

    live_stream_job_t job = { .req = async_req };
    if (xQueueSend(s_stream_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_worker_ready);
        httpd_req_async_handler_complete(async_req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Coda live piena");
        return ESP_FAIL;
    }

    return ESP_OK;
}
