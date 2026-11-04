#include "CoreDemo.h"
#include "CoreConfig.h"
#include "CoreGPIO.h"
#include "CoreLive.h"
#include "CoreRecorder.h"
#include "CoreRtsp.h"
#include "CoreSD.h"
#include "CoreStatusLed.h"
#include "CoreVideo.h"

#include "hal_capture.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "core_demo";
static volatile bool s_demo = false;
static bool s_demo_decided = false;
static volatile bool s_rtsp_view = false;
static core_settings_t s_rtsp_cfg;
static volatile int s_rtsp_goal = -1;
static volatile int s_rtsp_ack = 0;
static TaskHandle_t s_demo_task = nullptr;
static volatile int64_t s_clip_cooldown_until_us = 0;
static volatile bool s_reset_motion_streak = false;

void core_demo_notify_external_stop(void) {
    s_clip_cooldown_until_us =
        esp_timer_get_time() + ((int64_t)CORE_DEMO_UI_STOP_COOLDOWN_MS * 1000LL);
    s_reset_motion_streak = true;
    ESP_LOGI(TAG, "Stop esterno (GUI) — DEMO in pausa %u ms prima di nuovo movimento",
             (unsigned)CORE_DEMO_UI_STOP_COOLDOWN_MS);
}

bool core_gpio_is_demo_jumper(void) {
    return gpio_get_level((gpio_num_t)CORE_GPIO_DEMO) == 0;
}

static void demo_try_latch(const char *when, const core_settings_t *settings) {
    if (s_demo || s_demo_decided) {
        return;
    }
    s_demo_decided = true;
    const int lvl = gpio_get_level((gpio_num_t)CORE_GPIO_DEMO);
    if (settings && settings->demo_enable) {
        s_demo = true;
        ESP_LOGW(TAG, "DEMO attiva %s (flag Impostazioni, GPIO%d=%d) — no spegnimento TPL",
                 when, (int)CORE_GPIO_DEMO, lvl);
    } else {
        ESP_LOGI(TAG, "DEMO disattivata (impostazioni) %s — GPIO%d=%d ignorato",
                 when, (int)CORE_GPIO_DEMO, lvl);
    }
}

void core_demo_latch_from_boot(const core_settings_t *settings) {
    demo_try_latch("al boot", settings);
}

bool core_demo_is_active(void) {
    return s_demo;
}

const char *core_demo_html_banner(int rtsp_on) {
    static char html[4096];
    const int on = rtsp_on ? 1 : 0;
    const int streaming = (on && core_rtsp_pipe_up()) ? 1 : 0;
    const char *label = !on ? "Attiva RTSP" : (streaming ? "Spegni RTSP" : "RTSP fermo");
    const char *idle = streaming ? "" : " demo-rtsp-idle";
    const char *badge = s_demo
                            ? "<div class=\"demo-badge\" title=\"Modalita demo banco\">DEMO</div>"
                            : "";
    const char *top = s_demo ? "46px" : "10px";
    const int n = snprintf(
        html, sizeof(html),
        "<style>.demo-badge,.demo-rtsp{position:fixed;right:10px;z-index:2000;width:auto;"
        "background:#e65100;color:#fff;font-weight:700;padding:6px 14px;border-radius:6px;"
        "font-size:14px;letter-spacing:.06em;line-height:1.2;white-space:nowrap;"
        "box-shadow:0 2px 10px rgba(0,0,0,.25);font-family:sans-serif}"
        ".demo-rtsp-idle{background:#546e7a}"
        ".demo-badge{top:10px}.demo-rtsp{top:%s;border:0;margin:0;cursor:pointer;"
        "display:inline-block;left:auto}"
        "</style>"
        "%s"
        "<button type=\"button\" id=\"demoRtspBtn\" class=\"demo-rtsp%s\" data-on=\"%d\">%s</button>"
        "<script>(function(){var b=document.getElementById('demoRtspBtn');if(!b)return;"
        "function paint(j){var live=!!(j.on&&j.streaming);b.setAttribute('data-on',j.on?'1':'0');"
        "b.className='demo-rtsp'+(live?'':' demo-rtsp-idle');"
        "b.textContent=j.on?(live?'Spegni RTSP':'RTSP fermo'):'Attiva RTSP';"
        "if(j.url)b.title=j.on&&!live?'Encoder fermo':j.url;}"
        "b.onclick=function(){var next=b.getAttribute('data-on')==='1'?'0':'1';var old=b.textContent;"
        "b.disabled=true;b.textContent=next==='1'?'Apro RTSP...':'Chiudo RTSP...';"
        "fetch('/demo/rtsp',{method:'POST',credentials:'same-origin',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'on='+next})"
        ".then(function(r){return r.json();}).then(function(j){b.disabled=false;if(!j||!j.ok){b.textContent=old;return;}"
        "paint(j);"
        "if(location.pathname==='/live'||location.pathname==='/channels')location.reload();})"
        ".catch(function(){b.disabled=false;b.textContent=old;});};})();</script>",
        top, badge, idle, on, label);
    if (n < 0 || n >= (int)sizeof(html)) {
        html[0] = 0;
    }
    return html;
}

bool core_demo_rtsp_view(void) {
    return s_demo && s_rtsp_view;
}

bool core_demo_set_rtsp_view(bool on, const core_settings_t *settings) {
    if (!s_demo || !settings) {
        return false;
    }
    if (on == s_rtsp_view && s_rtsp_goal < 0) {
        return true;
    }
    /* La pagina non tocca la camera: il task DEMO fa lo scambio e risponde. */
    s_rtsp_cfg = *settings;
    const int ack = s_rtsp_ack;
    __sync_synchronize();
    s_rtsp_goal = on ? 1 : 0;
    for (int i = 0; i < 600 && s_rtsp_ack == ack; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_rtsp_ack != ack && s_rtsp_view == on;
}

static int motion_score_y(const uint8_t *a, int aw, int ah, int astride, const uint8_t *b,
                          int bw, int bh, int bstride) {
    if (!a || !b || aw < 32 || ah < 32 || bw < 32 || bh < 32) {
        return 0;
    }
    const int w = aw < bw ? aw : bw;
    const int h = ah < bh ? ah : bh;
    const int cols = 8;
    const int rows = 6;
    const int cell_w = w / cols;
    const int cell_h = h / rows;
    const int step = 4;
    const int pix_thr = 16;
    if (cell_w < step || cell_h < step) {
        return 0;
    }
    const int x_step = cell_w / 2;
    const int y_step = cell_h / 2;
    int max_pct = 0;
    int min_pct = 100;
    int windows = 0;
    for (int y0 = 0; y0 + cell_h <= h; y0 += y_step) {
        for (int x0 = 0; x0 + cell_w <= w; x0 += x_step) {
            int changed = 0;
            int samples = 0;
            const int y1 = y0 + cell_h;
            const int x1 = x0 + cell_w;
            for (int y = y0; y < y1; y += step) {
                const uint8_t *ra = a + (y * astride);
                const uint8_t *rb = b + (y * bstride);
                for (int x = x0; x < x1; x += step) {
                    int d = (int)ra[x] - (int)rb[x];
                    if (d < 0) {
                        d = -d;
                    }
                    if (d >= pix_thr) {
                        changed++;
                    }
                    samples++;
                }
            }
            const int pct = samples > 0 ? (changed * 100) / samples : 0;
            if (pct > max_pct) {
                max_pct = pct;
            }
            if (pct < min_pct) {
                min_pct = pct;
            }
            windows++;
        }
    }
    if (windows == 0) {
        return 0;
    }
    /* Stessa variazione su tutto il fotogramma: esposizione, non un soggetto. */
    if (min_pct >= 20 && max_pct - min_pct <= 15) {
        return 0;
    }
    return max_pct;
}

static void demo_rtsp_goal_apply(bool *luma_up, uint8_t **prev, int *prev_w, int *prev_h,
                                 int *prev_stride) {
    const int goal = s_rtsp_goal;
    if (goal < 0) {
        return;
    }
    bool ok = false;
    if (goal == 1) {
        if (core_recorder_manual_is_active()) {
            core_recorder_manual_stop();
            (void)core_recorder_manual_wait_done(8000);
        }
        if (*luma_up || hal_preview_running()) {
            hal_preview_stop_force();
            *luma_up = false;
        }
        free(*prev);
        *prev = nullptr;
        *prev_w = *prev_h = *prev_stride = 0;
        for (int i = 0; i < 30 && hal_preview_running(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (hal_preview_running()) {
            ESP_LOGW(TAG, "DEMO: luma ancora attiva, RTSP non acceso");
        } else {
            core_settings_t cfg = s_rtsp_cfg;
            cfg.rtsp_always = true;
            core_rtsp_apply(&cfg);
            core_rtsp_unhold_all();
            /* Un'apertura lenta non va annullata: il clic dopo ricominciava da zero. */
            int saw_busy = 0;
            for (int i = 0; i < 400 && !core_rtsp_pipe_up(); ++i) {
                if (core_rtsp_pipe_busy()) {
                    saw_busy = 1;
                } else if (saw_busy) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (core_rtsp_pipe_up()) {
                s_rtsp_view = true;
                ok = true;
                ESP_LOGI(TAG, "DEMO: RTSP acceso, movimento in pausa");
            } else {
                cfg.rtsp_always = false;
                core_rtsp_apply(&cfg);
                core_rtsp_hold();
                s_rtsp_view = false;
                ESP_LOGW(TAG, "DEMO: RTSP non partito, movimento ancora attivo");
            }
        }
    } else if (s_rtsp_view) {
        if (core_live_is_running()) {
            core_live_force_release();
        }
        core_settings_t cfg = s_rtsp_cfg;
        cfg.rtsp_always = false;
        core_rtsp_apply(&cfg);
        core_rtsp_hold();
        if (core_rtsp_pipe_busy()) {
            ESP_LOGW(TAG, "DEMO: RTSP ancora occupato, movimento resta in pausa");
        } else {
            s_rtsp_view = false;
            *luma_up = false;
            ok = true;
            ESP_LOGI(TAG, "DEMO: RTSP spento, movimento di nuovo attivo");
        }
    } else {
        ok = true;
    }
    if (s_rtsp_goal == goal) {
        s_rtsp_goal = -1;
    }
    (void)ok;
    s_rtsp_ack++;
}

static void demo_motion_task(void *arg) {
    core_settings_t settings = *(const core_settings_t *)arg;
    free(arg);

    uint16_t pw = settings.ch2_width ? settings.ch2_width : 1280;
    uint16_t ph = settings.ch2_height ? settings.ch2_height : 720;
    if (pw == 0 || ph == 0 || (uint32_t)pw * (uint32_t)ph > 1920u * 1080u) {
        pw = 1280;
        ph = 720;
    }

    if (hal_preview_start((int)pw, (int)ph, CORE_DEMO_PREVIEW_FPS, 2) != 0) {
        ESP_LOGE(TAG, "Preview demo non avviata");
        s_demo_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "Monitor movimento su luma Y %ux%u@%d (VI ch1, no MJPEG/AWB)",
             (unsigned)pw, (unsigned)ph, CORE_DEMO_PREVIEW_FPS);
    core_video_isp_demo_motion_stabilize();
    hal_preview_reapply_flip();

    uint8_t *prev = nullptr;
    int prev_w = 0;
    int prev_h = 0;
    int prev_stride = 0;
    int64_t quiet_since_us = 0;
    int64_t clip_start_us = 0;
    int64_t demo_rec_arm_us = 0;
    uint8_t motion_streak = 0;
    uint8_t motion_miss_streak = 0;
    bool was_recording = false;
    bool rec_had_video_this_clip = false;
    /* Il pull-down di fabbrica teneva D2 sempre a 0: un LOW non era un evento.
     * Dopo il pull-up, il riposo è HIGH. Si arma al primo HIGH e da lì un LOW
     * stabile chiude la clip e non ne fa partire un'altra finché resta basso. */
    bool d2_armed = false;
    int64_t d2_low_since_us = 0;
    int64_t d2_block_log_us = 0;
    int64_t last_score_log_us = 0;
    int64_t last_idle_log_us = 0;
    uint8_t luma_fail_streak = 0;
    bool luma_up = true;
    (void)core_sd_init_boot();
    ESP_LOGI(TAG, "Soglie movimento: cella piu mossa >=%d%% (%u frame), ferma <%d%%, stop %u ms",
             CORE_DEMO_MOTION_THRESHOLD, (unsigned)CORE_DEMO_MOTION_START_FRAMES,
             CORE_DEMO_MOTION_STILL_THRESHOLD, (unsigned)CORE_DEMO_NO_MOTION_STOP_MS);

    while (s_demo) {
        /* Anche con la pagina live o RTSP aperti: quel ramo fa continue
         * prima del controllo in fondo, e D2 non chiudeva la clip. */
        {
            const int64_t tick_us = esp_timer_get_time();
            const int d2_now = gpio_get_level((gpio_num_t)CORE_GPIO_D2_END);
            if (d2_now != 0) {
                if (!d2_armed) {
                    d2_armed = true;
                    ESP_LOGI(TAG, "D2 GPIO%d a riposo HIGH — STOP su LOW (pin fisico 26)",
                             (int)CORE_GPIO_D2_END);
                }
                d2_low_since_us = 0;
            } else if (d2_armed) {
                if (d2_low_since_us == 0) {
                    d2_low_since_us = tick_us;
                } else if ((tick_us - d2_low_since_us) >=
                               ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000) &&
                           core_recorder_manual_is_active()) {
                    ESP_LOGI(TAG, "D2 GPIO%d LOW — stop registrazione in corso",
                             (int)CORE_GPIO_D2_END);
                    core_recorder_manual_stop();
                }
            }
        }
        demo_rtsp_goal_apply(&luma_up, &prev, &prev_w, &prev_h, &prev_stride);
        if (s_rtsp_view) {
            if (core_recorder_manual_is_active()) {
                core_recorder_manual_stop();
                (void)core_recorder_manual_wait_done(20000);
            }
            if (luma_up) {
                hal_preview_stop_force();
                free(prev);
                prev = nullptr;
                prev_w = prev_h = prev_stride = 0;
                luma_up = false;
                ESP_LOGI(TAG, "DEMO: luma ferma, camera libera per RTSP");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* Live page (bypass MJPEG or the old selfpath) must not open a clip.
         * Entering /live changes the picture; that used to look like motion
         * and started a new file on every visit. */
        if (core_live_is_running() || hal_live_running()) {
            motion_streak = 0;
            motion_miss_streak = 0;
            if (prev) {
                free(prev);
                prev = nullptr;
                prev_w = prev_h = prev_stride = 0;
            }
            if (!hal_live_running() && luma_up) {
                luma_up = false;
                ESP_LOGI(TAG, "DEMO: luma in pausa, il live usa il selfpath");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!luma_up) {
            if (hal_preview_start((int)pw, (int)ph, CORE_DEMO_PREVIEW_FPS, 2) != 0) {
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            luma_up = true;
            motion_streak = 0;
            motion_miss_streak = 0;
            quiet_since_us = 0;
            core_video_isp_demo_motion_stabilize();
            hal_preview_reapply_flip();
            ESP_LOGI(TAG, "DEMO: movimento di nuovo in ascolto, capovolgi=%d specchia=%d",
                     core_video_get_vflip() ? 1 : 0, core_video_get_hflip() ? 1 : 0);
        }
        uint8_t *frame = nullptr;
        int fw = 0;
        int fh = 0;
        int fstride = 0;
        if (hal_preview_acquire_luma(&frame, &fw, &fh, &fstride, 500) != 0 || !frame) {
            if (luma_fail_streak < 255) {
                luma_fail_streak++;
            }
            if (luma_fail_streak >= 15) {
                ESP_LOGW(TAG, "Preview luma persa — riavvio VI ch1");
                hal_preview_stop_force();
                if (hal_preview_start((int)pw, (int)ph, CORE_DEMO_PREVIEW_FPS, 2) == 0) {
                    luma_fail_streak = 0;
                    free(prev);
                    prev = nullptr;
                    prev_w = prev_h = prev_stride = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        luma_fail_streak = 0;

        const int score = motion_score_y(prev, prev_w, prev_h, prev_stride, frame, fw, fh,
                                         fstride);
        const int64_t now_us = esp_timer_get_time();
        if (prev && (now_us - last_idle_log_us) >= 8000000LL) {
            last_idle_log_us = now_us;
            ESP_LOGI(TAG, "DEMO idle: score=%d (start>=%d, %u frame)",
                     score, CORE_DEMO_MOTION_THRESHOLD,
                     (unsigned)CORE_DEMO_MOTION_START_FRAMES);
        }
        const bool motion_start = score >= CORE_DEMO_MOTION_THRESHOLD;
        const bool rec_active = core_recorder_manual_is_active();
        core_recorder_manual_status_t rec_st = {};
        if (rec_active) {
            core_recorder_manual_get_status(&rec_st);
        }
        const bool rec_has_video = rec_st.frame_count > 0;
        /* Timer 10 s fermo: soglia STILL (11). START (16) solo per avviare la clip. */
        const bool scene_quiet = score < CORE_DEMO_MOTION_STILL_THRESHOLD;

        if (scene_quiet) {
            if (quiet_since_us == 0) {
                quiet_since_us = now_us;
            }
        } else {
            quiet_since_us = 0;
        }

        if (motion_start) {
            motion_miss_streak = 0;
            if (motion_streak < 255) {
                motion_streak++;
            }
        } else if (!rec_active) {
            if (motion_streak > 0) {
                if (motion_miss_streak < 255) {
                    motion_miss_streak++;
                }
                if (motion_miss_streak >= CORE_DEMO_MOTION_MISS_FRAMES) {
                    motion_streak = 0;
                    motion_miss_streak = 0;
                }
            }
        }

        if (s_reset_motion_streak) {
            s_reset_motion_streak = false;
            motion_streak = 0;
            quiet_since_us = 0;
        }

        if (rec_active && rec_has_video) {
            rec_had_video_this_clip = true;
            if (clip_start_us == 0) {
                clip_start_us = now_us;
                quiet_since_us = 0;
                ESP_LOGI(TAG, "DEMO: primo frame in MP4 — partono timer clip/8 s fermo");
            }
        }

        if (!rec_active && was_recording) {
            core_status_led_end_recording(true);
            clip_start_us = 0;
            demo_rec_arm_us = 0;
            quiet_since_us = 0;
            motion_streak = 0;
            motion_miss_streak = 0;
            if (rec_had_video_this_clip) {
                const int64_t cd = esp_timer_get_time() +
                                   ((int64_t)CORE_DEMO_CLIP_COOLDOWN_MS * 1000LL);
                if (cd > s_clip_cooldown_until_us) {
                    s_clip_cooldown_until_us = cd;
                }
                ESP_LOGI(TAG, "Clip terminata — cooldown movimento %u ms",
                         (unsigned)CORE_DEMO_CLIP_COOLDOWN_MS);
            } else {
                /* A clip with no MP4 must not mint a new seq on the next loop.
                 * D2 already low was doing that every ~150 ms. */
                s_clip_cooldown_until_us =
                    esp_timer_get_time() + ((int64_t)CORE_DEMO_CLIP_COOLDOWN_MS * 1000LL);
                ESP_LOGW(TAG, "Registrazione demo senza frame MP4 — pausa %u ms",
                         (unsigned)CORE_DEMO_CLIP_COOLDOWN_MS);
            }
            rec_had_video_this_clip = false;
        }
        was_recording = rec_active;

        if (rec_active &&
            (now_us - last_score_log_us) >= 5000000LL) {
            last_score_log_us = now_us;
            const int64_t quiet_ms =
                quiet_since_us > 0 ? (now_us - quiet_since_us) / 1000LL : 0;
            const int64_t clip_ms =
                clip_start_us > 0 ? (now_us - clip_start_us) / 1000LL : 0;
            ESP_LOGI(TAG, "DEMO rec: score=%d quiet=%lld ms clip=%lld s",
                     score, (long long)quiet_ms, (long long)(clip_ms / 1000));
        }

        const int d2_level = gpio_get_level((gpio_num_t)CORE_GPIO_D2_END);
        if (d2_level != 0) {
            if (!d2_armed) {
                d2_armed = true;
                ESP_LOGI(TAG, "D2 GPIO%d a riposo HIGH — STOP su LOW (pin fisico 26)",
                         (int)CORE_GPIO_D2_END);
            }
            d2_low_since_us = 0;
        } else if (d2_low_since_us == 0) {
            d2_low_since_us = now_us;
        }
        const bool d2_held =
            d2_armed && d2_level == 0 &&
            (now_us - d2_low_since_us) >= ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000);

        const bool cooldown_ok = now_us >= s_clip_cooldown_until_us;
        if (motion_streak == CORE_DEMO_MOTION_START_FRAMES && !rec_active && cooldown_ok &&
            d2_held) {
            if (now_us - d2_block_log_us >= 3000000LL) {
                d2_block_log_us = now_us;
                ESP_LOGI(TAG, "D2 GPIO%d LOW — clip non avviata", (int)CORE_GPIO_D2_END);
            }
        } else if (motion_streak == CORE_DEMO_MOTION_START_FRAMES && !rec_active && cooldown_ok) {
            ESP_LOGI(TAG, "Movimento score=%d — encoder H264 dedicato (cifratura=%d)",
                     score, settings.rec_encrypt ? 1 : 0);
            if (core_recorder_manual_start(&settings)) {
                clip_start_us = 0;
                demo_rec_arm_us = now_us;
                quiet_since_us = 0;
                rec_had_video_this_clip = false;
                core_status_led_set_mode(CORE_LED_RECORDING);
            } else {
                ESP_LOGW(TAG, "Avvio registrazione demo fallito");
            }
        } else if (!rec_active && motion_streak >= CORE_DEMO_MOTION_START_FRAMES && !cooldown_ok) {
            static int64_t s_last_cd_log;
            if (now_us - s_last_cd_log >= 3000000LL) {
                s_last_cd_log = now_us;
                ESP_LOGI(TAG, "Movimento score=%d — cooldown clip (%lld ms rimasti)",
                         score,
                         (long long)((s_clip_cooldown_until_us - now_us) / 1000LL));
            }
        } else if (rec_active) {
            const bool d2_stop = d2_held;
            const int64_t quiet_ms =
                quiet_since_us > 0 ? (now_us - quiet_since_us) / 1000LL : 0;
            const int64_t t0_us = clip_start_us > 0 ? clip_start_us : demo_rec_arm_us;
            const int64_t clip_ms = t0_us > 0 ? (now_us - t0_us) / 1000LL : 0;
            auto demo_stop_rec = [&](const char *why) {
                ESP_LOGI(TAG, "%s — stop registrazione demo (attendo chiusura MP4…)", why);
                core_status_led_end_recording(true);
                core_recorder_manual_stop();
                if (!core_recorder_manual_wait_done(180000)) {
                    ESP_LOGW(TAG, "Timeout chiusura MP4 demo (>180 s)");
                } else {
                    ESP_LOGI(TAG, "Clip demo chiusa (moov scritto)");
                }
                clip_start_us = 0;
                demo_rec_arm_us = 0;
                quiet_since_us = 0;
                motion_streak = 0;
                s_clip_cooldown_until_us =
                    esp_timer_get_time() + ((int64_t)CORE_DEMO_CLIP_COOLDOWN_MS * 1000LL);
            };
            if (d2_stop) {
                demo_stop_rec("D2 LOW");
            } else if (clip_ms >= (int64_t)CORE_DEMO_MAX_CLIP_MS) {
                demo_stop_rec("Durata max clip DEMO");
            } else if (quiet_ms >= (int64_t)CORE_DEMO_NO_MOTION_STOP_MS &&
                       (rec_has_video || quiet_since_us > 0)) {
                demo_stop_rec("8 s scena ferma");
            } else if (!rec_has_video && clip_ms >= 20000) {
                demo_stop_rec("Encoder senza frame da 20 s");
            }
        }

        free(prev);
        prev = frame;
        prev_w = fw;
        prev_h = fh;
        prev_stride = fstride;
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    if (core_recorder_manual_is_active()) {
        core_recorder_manual_stop();
        (void)core_recorder_manual_wait_done(180000);
    }
    free(prev);
    s_demo_task = nullptr;
    vTaskDelete(nullptr);
}

void core_demo_config_start(const core_settings_t *settings) {
    if (!settings) {
        return;
    }
    demo_try_latch("in CONFIG", settings);
    if (!s_demo) {
        return;
    }
    core_settings_t cfg = *settings;
    /* Niente streamer all'avvio: aprire l'H264 RTSP a freddo blocca il sensore
     * per decine di secondi (live-pre + retry) prima che il movimento veda i frame. */
    cfg.rtsp_always = false;
    core_rtsp_hold();
    core_rtsp_apply(&cfg);
    ESP_LOGI(TAG, "DEMO: niente streamer all'avvio, movimento appena la luma e' pronta");

    if (s_demo_task) {
        return;
    }
    /* AP resta in lampeggio lento. I 3 lampeggi dell'IP non vanno interrotti. */
    const core_status_led_mode_t led = core_status_led_current();
    if (led != CORE_LED_AP && led != CORE_LED_STA_CONNECTED) {
        core_status_led_set_mode(CORE_LED_OFF);
        ESP_LOGI(TAG, "LED DEMO: spento in attesa movimento (acceso fisso solo in registrazione)");
    }
    core_settings_t *copy = (core_settings_t *)malloc(sizeof(core_settings_t));
    if (!copy) {
        return;
    }
    *copy = cfg;
    if (xTaskCreate(demo_motion_task, "demo_md", 16384, copy, 4, &s_demo_task) != pdPASS) {
        free(copy);
        ESP_LOGE(TAG, "Task demo_motion non creato");
    }
}
