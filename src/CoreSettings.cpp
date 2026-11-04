#include "CoreSettings.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static const char *TAG = "core_settings";

static nvs_handle_t s_nvs = 0;

void core_settings_rec_pins_set_defaults(core_settings_t *s) {
    if (!s) {
        return;
    }
    s->rec_gpio_start = (uint8_t)CORE_GPIO_D1_WAKE;
    s->rec_gpio_stop = (uint8_t)CORE_GPIO_D2_END;
}

bool core_settings_rec_pins_valid(uint8_t start_gpio, uint8_t stop_gpio) {
    if (start_gpio == stop_gpio) {
        return false;
    }
    if (start_gpio != (uint8_t)CORE_GPIO_D1_WAKE && start_gpio != (uint8_t)CORE_GPIO_D2_END) {
        return false;
    }
    if (stop_gpio != (uint8_t)CORE_GPIO_D1_WAKE && stop_gpio != (uint8_t)CORE_GPIO_D2_END) {
        return false;
    }
    return true;
}

bool core_settings_rec_max_duration_valid(uint32_t sec) {
    return sec >= CORE_REC_MAX_DURATION_SEC_MIN && sec <= CORE_REC_MAX_DURATION_SEC_MAX;
}

bool core_settings_rec_media_mode_valid(uint8_t mode) {
    return mode == CORE_REC_MEDIA_VIDEO || mode == CORE_REC_MEDIA_PHOTO;
}

bool core_settings_rec_stop_mode_valid(uint8_t mode) {
    return mode == CORE_REC_STOP_MODE_GPIO || mode == CORE_REC_STOP_MODE_TIMER;
}

bool core_settings_rec_session_sec_valid(uint32_t sec) {
    return sec >= CORE_REC_SESSION_SEC_MIN && sec <= CORE_REC_SESSION_SEC_MAX;
}

bool core_settings_rec_stop_ignores_gpio(const core_settings_t *s) {
    return s && s->rec_stop_mode == CORE_REC_STOP_MODE_TIMER;
}

bool core_settings_rec_stop_is_timer(const core_settings_t *s) {
    return core_settings_rec_stop_ignores_gpio(s);
}

bool core_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_open(CORE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void core_settings_video_set_defaults(core_settings_t *s) {
    if (!s) {
        return;
    }
    s->video_width = CORE_VIDEO_WIDTH_DEFAULT;
    s->video_height = CORE_VIDEO_HEIGHT_DEFAULT;
    s->video_fps = (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    s->video_vflip = false;
    s->video_hflip = false;
    s->video_bayer_mode = CORE_VIDEO_BAYER_AUTO;
    s->ch2_width = 1280;
    s->ch2_height = 720;
    s->ch2_fps = 15;
    s->rec_encrypt = false;
    s->rec_media_mode = CORE_REC_MEDIA_VIDEO;
    s->rec_stop_mode = CORE_REC_STOP_MODE_GPIO;
    s->rec_session_sec = CORE_REC_SESSION_SEC_DEFAULT;
}

void core_settings_osd_set_defaults(core_settings_t *s) {
    if (!s) {
        return;
    }
    s->osd_enable = true;
    s->osd_font = (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    s->osd_x_pct = (uint8_t)CORE_REC_OSD_X_PCT_DEFAULT;
    s->osd_y_pct = (uint8_t)CORE_REC_OSD_Y_PCT_DEFAULT;
    s->rtsp_always = false;
    s->demo_enable = false;
}

bool core_settings_osd_font_valid(uint8_t font) {
    return font == 12 || font == 16 || font == 20 || font == 24 || font == 32;
}

void core_settings_osd_normalize(core_settings_t *s) {
    if (!s) {
        return;
    }
    if (!core_settings_osd_font_valid(s->osd_font)) {
        s->osd_font = (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    }
    if (s->osd_x_pct > 100) {
        s->osd_x_pct = 100;
    }
    if (s->osd_y_pct > 100) {
        s->osd_y_pct = 100;
    }
}

void core_settings_osd_box_size(uint8_t font, uint16_t *out_w, uint16_t *out_h) {
    if (!core_settings_osd_font_valid(font)) {
        font = (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    }
    // Font esp_painter basic: larghezza tipica ≈ font/2 (glyph mono).
    uint16_t pad = 8;
    uint16_t char_w = (uint16_t)((font + 1) / 2);
    if (char_w < 6) {
        char_w = 6;
    }
    uint16_t tw = (uint16_t)(char_w * CORE_REC_OSD_TEXT_CHARS);
    uint16_t th = (uint16_t)(font + 4);
    if (out_w) {
        *out_w = (uint16_t)(pad * 2 + tw);
    }
    if (out_h) {
        *out_h = (uint16_t)(pad * 2 + th);
    }
}

void core_settings_osd_pixel_pos(const core_settings_t *s, uint16_t frame_w, uint16_t frame_h,
                                 uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_box_w, uint16_t *out_box_h) {
    uint8_t font = s ? s->osd_font : (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    uint8_t xp = s ? s->osd_x_pct : (uint8_t)CORE_REC_OSD_X_PCT_DEFAULT;
    uint8_t yp = s ? s->osd_y_pct : (uint8_t)CORE_REC_OSD_Y_PCT_DEFAULT;
    if (xp > 100) {
        xp = 100;
    }
    if (yp > 100) {
        yp = 100;
    }
    uint16_t bw = 0;
    uint16_t bh = 0;
    core_settings_osd_box_size(font, &bw, &bh);
    if (frame_w < 64) {
        frame_w = 64;
    }
    if (frame_h < 64) {
        frame_h = 64;
    }
    if (bw > frame_w) {
        bw = frame_w;
    }
    if (bh > frame_h) {
        bh = frame_h;
    }
    uint16_t max_x = (uint16_t)(frame_w - bw);
    uint16_t max_y = (uint16_t)(frame_h - bh);
    uint16_t x = (uint16_t)(((uint32_t)xp * max_x) / 100U);
    uint16_t y = (uint16_t)(((uint32_t)yp * max_y) / 100U);
    if (out_x) {
        *out_x = x;
    }
    if (out_y) {
        *out_y = y;
    }
    if (out_box_w) {
        *out_box_w = bw;
    }
    if (out_box_h) {
        *out_box_h = bh;
    }
}

bool core_settings_video_valid(uint16_t width, uint16_t height, uint8_t fps) {
    if (width < CORE_VIDEO_WIDTH_MIN || width > CORE_VIDEO_WIDTH_MAX) {
        return false;
    }
    if (height < CORE_VIDEO_HEIGHT_MIN || height > CORE_VIDEO_HEIGHT_MAX) {
        return false;
    }
    if (fps < CORE_VIDEO_FPS_MIN || fps > CORE_VIDEO_FPS_MAX) {
        return false;
    }
    if ((width & 1) || (height & 1)) {
        return false;
    }
    if (fps > core_video_fps_ceiling(width, height)) {
        return false;
    }
    return true;
}

bool core_settings_bayer_mode_valid(uint8_t mode) {
    return mode <= CORE_VIDEO_BAYER_MAX;
}

void core_settings_video_normalize(core_settings_t *s) {
    if (!s) {
        return;
    }
    const uint16_t ow = s->video_width;
    const uint16_t oh = s->video_height;
    s->video_width = core_video_align16(s->video_width, CORE_VIDEO_WIDTH_MAX);
    s->video_height = core_video_align16(s->video_height, CORE_VIDEO_HEIGHT_MAX);
    if (s->video_width != ow || s->video_height != oh) {
        ESP_LOGW(TAG, "Video %ux%u allineato a %ux%u (griglia 16 px)",
                 (unsigned)ow, (unsigned)oh,
                 (unsigned)s->video_width, (unsigned)s->video_height);
    }
    {
        const uint8_t cap = core_video_fps_ceiling(s->video_width, s->video_height);
        if (s->video_fps > cap) {
            ESP_LOGW(TAG, "Video %ux%u: fps %u oltre il tetto %u — ridotto",
                     (unsigned)s->video_width, (unsigned)s->video_height,
                     (unsigned)s->video_fps, (unsigned)cap);
            s->video_fps = cap;
        }
    }
    if (!core_settings_video_valid(s->video_width, s->video_height, s->video_fps)) {
        core_settings_video_set_defaults(s);
    }
    if (!core_settings_bayer_mode_valid(s->video_bayer_mode)) {
        s->video_bayer_mode = CORE_VIDEO_BAYER_AUTO;
    }
}

uint32_t core_settings_video_frame_ms(const core_settings_t *s) {
    uint8_t fps = s ? s->video_fps : (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    if (fps == 0) {
        fps = (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    }
    return 1000U / (uint32_t)fps;
}

uint32_t core_settings_bitrate_for(uint16_t width, uint16_t height, uint8_t fps) {
    if (width == 0 || height == 0 || fps == 0) {
        return CORE_VIDEO_BITRATE_DEFAULT;
    }
    uint64_t px = (uint64_t)width * (uint64_t)height * (uint64_t)fps;
    uint64_t base_px = (uint64_t)CORE_VIDEO_WIDTH_DEFAULT * (uint64_t)CORE_VIDEO_HEIGHT_DEFAULT
                       * (uint64_t)CORE_VIDEO_FPS_DEFAULT;
    if (base_px == 0) {
        return CORE_VIDEO_BITRATE_DEFAULT;
    }
    uint64_t bps = ((uint64_t)CORE_VIDEO_BITRATE_DEFAULT * px) / base_px;
    /* 0.28 bit per pixel: 8 MP at 15 fps uses the 32 Mbps cap. */
    uint64_t motion = (px * 28ULL) / 100ULL;
    if (bps < motion) {
        bps = motion;
    }
    if (CORE_VIDEO_IS_FULLHD(width, height) && bps < CORE_VIDEO_BITRATE_1080_MIN) {
        bps = CORE_VIDEO_BITRATE_1080_MIN;
    }
    if (bps < CORE_VIDEO_BITRATE_MIN) {
        bps = CORE_VIDEO_BITRATE_MIN;
    }
    if (bps > CORE_VIDEO_BITRATE_MAX) {
        bps = CORE_VIDEO_BITRATE_MAX;
    }
    return (uint32_t)bps;
}

uint32_t core_settings_video_bitrate(const core_settings_t *s) {
    if (!s) {
        return CORE_VIDEO_BITRATE_DEFAULT;
    }
    return core_settings_bitrate_for(s->video_width, s->video_height, s->video_fps);
}

uint32_t core_settings_video_gop(const core_settings_t *s) {
    uint8_t fps = s ? s->video_fps : (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    if (fps == 0) {
        fps = (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    }
    if (s && (uint32_t)s->video_width * (uint32_t)s->video_height > (1280u * 720u)) {
        uint32_t gop = (uint32_t)fps / 2U;
        if (gop < 4U) {
            gop = 4U;
        }
        return gop;
    }
    if (fps <= CORE_VIDEO_FPS_LOW_MAX) {
        uint32_t gop = (uint32_t)fps * 2U;
        if (gop < 2U) {
            gop = 2U;
        }
        return gop;
    }
    // GOP ≈ 1 s (prima era 2 s): al massimo ~1 s di attesa al primo IDR dopo prime/warmup.
    uint32_t gop = (uint32_t)fps;
    if (gop < 8) {
        gop = 8;
    }
    if (gop > 30) {
        gop = 30;
    }
    return gop;
}

bool core_settings_load(core_settings_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->d2_post_delay_ms = CORE_D2_POST_DELAY_MS;
    out->rec_max_duration_sec = CORE_REC_MAX_DURATION_SEC_DEFAULT;
    core_settings_rec_pins_set_defaults(out);
    core_settings_video_set_defaults(out);
    out->rec_media_mode = CORE_REC_MEDIA_VIDEO;
    out->rec_stop_mode = CORE_REC_STOP_MODE_GPIO;
    out->rec_session_sec = CORE_REC_SESSION_SEC_DEFAULT;
    core_settings_osd_set_defaults(out);

    uint32_t id = 0;
    if (nvs_get_u32(s_nvs, "core_id", &id) == ESP_OK && id >= CORE_ID_MIN && id <= CORE_ID_MAX) {
        out->core_id = id;
    }

    size_t key_len = sizeof(out->aes_key);
    if (nvs_get_blob(s_nvs, "aes_key", out->aes_key, &key_len) == ESP_OK && key_len == sizeof(out->aes_key)) {
        out->aes_key_valid = true;
    }

    size_t ssid_len = sizeof(out->wifi_ssid);
    nvs_get_str(s_nvs, "wifi_ssid", out->wifi_ssid, &ssid_len);

    size_t pass_len = sizeof(out->wifi_pass);
    nvs_get_str(s_nvs, "wifi_pass", out->wifi_pass, &pass_len);

    uint32_t delay_ms = 0;
    if (nvs_get_u32(s_nvs, "d2_delay", &delay_ms) == ESP_OK &&
        delay_ms >= CORE_D2_DELAY_MIN_MS && delay_ms <= CORE_D2_DELAY_MAX_MS) {
        out->d2_post_delay_ms = delay_ms;
    }

    uint32_t max_dur_sec = 0;
    if (nvs_get_u32(s_nvs, "rec_max_sec", &max_dur_sec) == ESP_OK &&
        core_settings_rec_max_duration_valid(max_dur_sec)) {
        out->rec_max_duration_sec = max_dur_sec;
    }

    uint8_t start_gpio = 0;
    uint8_t stop_gpio = 0;
    const bool have_start = nvs_get_u8(s_nvs, "rec_gpio_start", &start_gpio) == ESP_OK;
    const bool have_stop = nvs_get_u8(s_nvs, "rec_gpio_stop", &stop_gpio) == ESP_OK;
    /* Layout Pico Pro: D2 è linux 58. La NVS precedente aveva GPIO4_C1 (145). */
    bool migrated = false;
    if (have_start && start_gpio == 145) {
        start_gpio = (uint8_t)CORE_GPIO_D2_END;
        migrated = true;
    }
    if (have_stop && stop_gpio == 145) {
        stop_gpio = (uint8_t)CORE_GPIO_D2_END;
        migrated = true;
    }
    if (have_start && have_stop &&
        core_settings_rec_pins_valid(start_gpio, stop_gpio)) {
        out->rec_gpio_start = start_gpio;
        out->rec_gpio_stop = stop_gpio;
        if (migrated) {
            nvs_set_u8(s_nvs, "rec_gpio_start", start_gpio);
            nvs_set_u8(s_nvs, "rec_gpio_stop", stop_gpio);
            nvs_commit(s_nvs);
            ESP_LOGI(TAG, "Pin D2 NVS migrato da linux 145 a %d", (int)CORE_GPIO_D2_END);
        }
    } else if (have_start || have_stop) {
        ESP_LOGW(TAG, "Pin registrazione NVS non validi (start=%u stop=%u) — ripristino default D1/D2",
                 (unsigned)start_gpio, (unsigned)stop_gpio);
    }

    uint16_t vw = 0;
    uint16_t vh = 0;
    uint8_t vf = 0;
    if (nvs_get_u16(s_nvs, "video_w", &vw) == ESP_OK &&
        nvs_get_u16(s_nvs, "video_h", &vh) == ESP_OK &&
        nvs_get_u8(s_nvs, "video_fps", &vf) == ESP_OK &&
        core_settings_video_valid(vw, vh, vf)) {
        out->video_width = vw;
        out->video_height = vh;
        out->video_fps = vf;
    }
    uint16_t c2w = 0;
    uint16_t c2h = 0;
    uint8_t c2f = 0;
    if (nvs_get_u16(s_nvs, "ch2_w", &c2w) == ESP_OK &&
        nvs_get_u16(s_nvs, "ch2_h", &c2h) == ESP_OK &&
        nvs_get_u8(s_nvs, "ch2_fps", &c2f) == ESP_OK &&
        core_settings_video_valid(c2w, c2h, c2f)) {
        out->ch2_width = c2w;
        out->ch2_height = c2h;
        out->ch2_fps = c2f;
    }
    uint8_t flip = 0;
    if (nvs_get_u8(s_nvs, "video_vflip", &flip) == ESP_OK) {
        out->video_vflip = (flip != 0);
    }
    if (nvs_get_u8(s_nvs, "video_hflip", &flip) == ESP_OK) {
        out->video_hflip = (flip != 0);
    }
    uint8_t bayer_mode = CORE_VIDEO_BAYER_AUTO;
    if (nvs_get_u8(s_nvs, "video_bayer", &bayer_mode) == ESP_OK &&
        core_settings_bayer_mode_valid(bayer_mode)) {
        out->video_bayer_mode = bayer_mode;
    }
    uint8_t rec_enc = 0;
    uint8_t enc_optin = 0;
    // Default di fabbrica era true: chi non ha mai toccato la checkbox restava in AES (~6 fps).
    // enc_optin=1 solo dopo un salvataggio esplicito da web.
    if (nvs_get_u8(s_nvs, "enc_optin", &enc_optin) == ESP_OK && enc_optin != 0) {
        if (nvs_get_u8(s_nvs, "rec_encrypt", &rec_enc) == ESP_OK) {
            out->rec_encrypt = (rec_enc != 0);
        } else {
            out->rec_encrypt = false;
        }
    } else {
        out->rec_encrypt = false;
        (void)nvs_set_u8(s_nvs, "rec_encrypt", 0);
        (void)nvs_set_u8(s_nvs, "enc_optin", 1);
        (void)nvs_commit(s_nvs);
        ESP_LOGI(TAG, "AES SD: default off (non era una scelta utente)");
    }
    uint8_t media_mode = CORE_REC_MEDIA_VIDEO;
    if (nvs_get_u8(s_nvs, "rec_media", &media_mode) == ESP_OK &&
        core_settings_rec_media_mode_valid(media_mode)) {
        out->rec_media_mode = media_mode;
    }
    uint8_t stop_mode = CORE_REC_STOP_MODE_GPIO;
    if (nvs_get_u8(s_nvs, "rec_stop_mode", &stop_mode) == ESP_OK &&
        core_settings_rec_stop_mode_valid(stop_mode)) {
        out->rec_stop_mode = stop_mode;
    }
    uint32_t sess_sec = 0;
    if (nvs_get_u32(s_nvs, "rec_sess_sec", &sess_sec) == ESP_OK &&
        core_settings_rec_session_sec_valid(sess_sec)) {
        out->rec_session_sec = sess_sec;
    }
    uint8_t osd_en = 1;
    if (nvs_get_u8(s_nvs, "osd_en", &osd_en) == ESP_OK) {
        out->osd_enable = (osd_en != 0);
    }
    uint8_t osd_font = CORE_REC_OSD_FONT_DEFAULT;
    if (nvs_get_u8(s_nvs, "osd_font", &osd_font) == ESP_OK) {
        out->osd_font = osd_font;
    }
    uint8_t osd_xp = CORE_REC_OSD_X_PCT_DEFAULT;
    if (nvs_get_u8(s_nvs, "osd_x_pct", &osd_xp) == ESP_OK) {
        out->osd_x_pct = osd_xp;
    }
    uint8_t osd_yp = CORE_REC_OSD_Y_PCT_DEFAULT;
    if (nvs_get_u8(s_nvs, "osd_y_pct", &osd_yp) == ESP_OK) {
        out->osd_y_pct = osd_yp;
    }
    uint8_t rtsp_on = 0;
    if (nvs_get_u8(s_nvs, "rtsp_on", &rtsp_on) == ESP_OK) {
        out->rtsp_always = (rtsp_on != 0);
    }
    uint8_t demo_en = 0;
    if (nvs_get_u8(s_nvs, "demo_en", &demo_en) == ESP_OK) {
        out->demo_enable = (demo_en != 0);
    }
    core_settings_video_normalize(out);
    core_settings_osd_normalize(out);
    return true;
}

bool core_settings_save(const core_settings_t *in) {
    if (!in) return false;
    esp_err_t err = nvs_set_u32(s_nvs, "core_id", in->core_id);
    if (err != ESP_OK) return false;
    if (in->aes_key_valid) {
        err = nvs_set_blob(s_nvs, "aes_key", in->aes_key, sizeof(in->aes_key));
        if (err != ESP_OK) return false;
    }
    err = nvs_set_str(s_nvs, "wifi_ssid", in->wifi_ssid);
    if (err != ESP_OK) return false;
    err = nvs_set_str(s_nvs, "wifi_pass", in->wifi_pass);
    if (err != ESP_OK) return false;
    err = nvs_set_u32(s_nvs, "d2_delay", in->d2_post_delay_ms);
    if (err != ESP_OK) return false;
    err = nvs_set_u32(s_nvs, "rec_max_sec", in->rec_max_duration_sec);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_gpio_start", in->rec_gpio_start);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_gpio_stop", in->rec_gpio_stop);
    if (err != ESP_OK) return false;
    core_settings_t video = *in;
    core_settings_video_normalize(&video);
    err = nvs_set_u16(s_nvs, "video_w", video.video_width);
    if (err != ESP_OK) return false;
    err = nvs_set_u16(s_nvs, "video_h", video.video_height);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "video_fps", video.video_fps);
    if (err != ESP_OK) return false;
    if (!core_settings_video_valid(video.ch2_width, video.ch2_height, video.ch2_fps)) {
        video.ch2_width = 1280;
        video.ch2_height = 720;
        video.ch2_fps = 15;
    }
    err = nvs_set_u16(s_nvs, "ch2_w", video.ch2_width);
    if (err != ESP_OK) return false;
    err = nvs_set_u16(s_nvs, "ch2_h", video.ch2_height);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "ch2_fps", video.ch2_fps);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "video_vflip", in->video_vflip ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "video_hflip", in->video_hflip ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "video_bayer", in->video_bayer_mode);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_encrypt", in->rec_encrypt ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "enc_optin", 1);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_media", in->rec_media_mode);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_stop_mode", in->rec_stop_mode);
    if (err != ESP_OK) return false;
    err = nvs_set_u32(s_nvs, "rec_sess_sec", in->rec_session_sec);
    if (err != ESP_OK) return false;
    core_settings_t osd = *in;
    core_settings_osd_normalize(&osd);
    err = nvs_set_u8(s_nvs, "osd_en", osd.osd_enable ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "osd_font", osd.osd_font);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "osd_x_pct", osd.osd_x_pct);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "osd_y_pct", osd.osd_y_pct);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rtsp_on", in->rtsp_always ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "demo_en", in->demo_enable ? 1 : 0);
    if (err != ESP_OK) return false;
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fallito: %s", esp_err_to_name(err));
        return false;
    }
    if (!core_settings_export_tbrec_override(in)) {
        ESP_LOGW(TAG, "NVS ok ma override PIR (tbrec) non scritto — check /userdata/mrbin");
    }
    ESP_LOGI(TAG, "Impostazioni salvate (id=%05lu delay=%lu ms max_rec=%lu s stop_mode=%u sess=%lu s stop=GPIO%u video=%ux%u@%u)",
             (unsigned long)in->core_id, (unsigned long)in->d2_post_delay_ms,
             (unsigned long)in->rec_max_duration_sec,
             (unsigned)in->rec_stop_mode, (unsigned long)in->rec_session_sec,
             (unsigned)in->rec_gpio_stop,
             (unsigned)video.video_width, (unsigned)video.video_height, (unsigned)video.video_fps);
    return true;
}

bool core_settings_export_tbrec_override(const core_settings_t *in) {
    if (!in) return false;
#ifndef _WIN32
    /* PIR uses mrbin-tbrec, not CoreRecorder — publish stop mode / duration. */
    (void)mkdir(CORE_NVS_DIR, 0755);
    const char *path = CORE_NVS_DIR "/tbrec.override";
    const char *tmp = CORE_NVS_DIR "/tbrec.override.tmp";
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        ESP_LOGE(TAG, "tbrec.override: impossibile creare %s", tmp);
        return false;
    }
    const int stop_mode = core_settings_rec_stop_is_timer(in) ? 1 : 0;
    const unsigned sess = in->rec_session_sec ? in->rec_session_sec : CORE_REC_SESSION_SEC_DEFAULT;
    /* Timer: exact session length. GPIO: safety ceiling (segment max). */
    const unsigned rec_sec = stop_mode ? sess : (in->rec_max_duration_sec ? in->rec_max_duration_sec : 600U);
    fprintf(fp,
            "# Generated by mrbin-core settings — do not edit by hand\n"
            "REC_STOP_MODE=%d\n"
            "REC_SECONDS=%u\n"
            "STOP_GPIO=%u\n"
            "ENCRYPT=%d\n"
            "VIDEO_W=%u\n"
            "VIDEO_H=%u\n"
            "VIDEO_FPS=%u\n",
            stop_mode, rec_sec, (unsigned)in->rec_gpio_stop,
            (in->rec_encrypt && in->aes_key_valid) ? 1 : 0,
            (unsigned)in->video_width, (unsigned)in->video_height,
            (unsigned)in->video_fps);
    bool ok = fflush(fp) == 0 && ferror(fp) == 0;
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(tmp);
        ESP_LOGE(TAG, "tbrec.override: scrittura fallita");
        return false;
    }
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "tbrec.override: rename fallito");
        remove(tmp);
        return false;
    }
    ESP_LOGI(TAG, "PIR override: %s (stop_mode=%d rec=%us stop_gpio=%u encrypt=%d video=%ux%u@%u)",
             path, stop_mode, rec_sec, (unsigned)in->rec_gpio_stop,
             (in->rec_encrypt && in->aes_key_valid) ? 1 : 0,
             (unsigned)in->video_width, (unsigned)in->video_height,
             (unsigned)in->video_fps);
    return true;
#else
    (void)in;
    return true;
#endif
}

bool core_settings_ensure_id(core_settings_t *s) {
    if (!s) return false;
    if (s->core_id >= CORE_ID_MIN && s->core_id <= CORE_ID_MAX) {
        ESP_LOGI(TAG, "CORE ID esistente: %05lu", (unsigned long)s->core_id);
        return true;
    }
    uint32_t range = (CORE_ID_MAX - CORE_ID_MIN + 1);
    s->core_id = CORE_ID_MIN + (esp_random() % range);
    ESP_LOGI(TAG, "Nuovo CORE ID creato: %05lu", (unsigned long)s->core_id);
    return core_settings_save(s);
}

bool core_settings_ensure_aes_key(core_settings_t *s) {
    if (!s) return false;
    if (s->aes_key_valid) {
        ESP_LOGI(TAG, "Chiave AES-128 esistente, riutilizzo");
        return true;
    }
    esp_fill_random(s->aes_key, sizeof(s->aes_key));
    s->aes_key_valid = true;
    ESP_LOGI(TAG, "Nuova chiave AES-128 creata");
    return core_settings_save(s);
}

static uint16_t rec_file_seq_normalize(uint16_t seq) {
    if (seq > CORE_REC_FILE_SEQ_MAX) {
        return CORE_REC_FILE_SEQ_MIN;
    }
    return seq;
}

bool core_settings_rec_file_seq_peek(uint16_t *out_next) {
    if (!out_next || !s_nvs) {
        return false;
    }
    uint16_t seq = CORE_REC_FILE_SEQ_MIN;
    if (nvs_get_u16(s_nvs, "rec_fseq", &seq) != ESP_OK) {
        seq = CORE_REC_FILE_SEQ_MIN;
    }
    *out_next = rec_file_seq_normalize(seq);
    return true;
}

bool core_settings_rec_file_seq_commit(uint16_t next_after_use) {
    if (!s_nvs) {
        return false;
    }
    next_after_use = rec_file_seq_normalize(next_after_use);
    esp_err_t err = nvs_set_u16(s_nvs, "rec_fseq", next_after_use);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_commit(s_nvs);
    return err == ESP_OK;
}
