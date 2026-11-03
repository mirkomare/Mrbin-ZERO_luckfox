#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t core_id;           // 5 cifre
    uint8_t  aes_key[16];       // AES-128, creata al primo avvio
    bool     aes_key_valid;
    char     wifi_ssid[32];
    char     wifi_pass[64];
    uint32_t d2_post_delay_ms;  // default 5000
    uint32_t rec_max_duration_sec;  // durata segmento PIR (default 10 min); rollover _partNN
    uint8_t  rec_gpio_start;    // GPIO attivazione registrazione (default D1 linux 52)
    uint8_t  rec_gpio_stop;     // GPIO stop registrazione (default D2 linux 58)
    uint16_t video_width;       // H264 registrazione (persistente NVS)
    uint16_t video_height;
    uint8_t  video_fps;
    bool     video_vflip;       // capovolgi immagine (flip verticale, sensore)
    bool     video_hflip;       // specchia immagine (flip orizzontale, sensore)
    uint8_t  video_bayer_mode;  // CORE_VIDEO_BAYER_* — auto o fase manuale (NVS)
    bool     rec_encrypt;       // true: AES su SD (solo se checkbox in Reg video); default false
    uint8_t  rec_media_mode;    // CORE_REC_MEDIA_VIDEO | CORE_REC_MEDIA_PHOTO
    uint8_t  rec_stop_mode;     // CORE_REC_STOP_MODE_GPIO | CORE_REC_STOP_MODE_TIMER
    uint32_t rec_session_sec;   // durata fissa sessione (TIMER); pin STOP ignorato
    bool     osd_enable;        // burn-in data/ora su video/foto
    uint8_t  osd_font;          // px: 12/16/20/24/32
    uint8_t  osd_x_pct;         // 0–100: posizione angolo alto-sx (%% larghezza frame)
    uint8_t  osd_y_pct;         // 0–100: posizione angolo alto-sx (%% altezza frame)
    bool     rtsp_always;       // true: encoder H.264 sempre su per rtsp://:554/live
} core_settings_t;

void core_settings_video_set_defaults(core_settings_t *s);
bool core_settings_video_valid(uint16_t width, uint16_t height, uint8_t fps);
bool core_settings_bayer_mode_valid(uint8_t mode);
void core_settings_video_normalize(core_settings_t *s);
uint32_t core_settings_video_frame_ms(const core_settings_t *s);
uint32_t core_settings_video_bitrate(const core_settings_t *s);
uint32_t core_settings_video_gop(const core_settings_t *s);

bool core_settings_rec_pins_valid(uint8_t start_gpio, uint8_t stop_gpio);
void core_settings_rec_pins_set_defaults(core_settings_t *s);
bool core_settings_rec_max_duration_valid(uint32_t sec);
bool core_settings_rec_media_mode_valid(uint8_t mode);
bool core_settings_rec_stop_mode_valid(uint8_t mode);
bool core_settings_rec_session_sec_valid(uint32_t sec);
bool core_settings_rec_stop_ignores_gpio(const core_settings_t *s);
bool core_settings_rec_stop_is_timer(const core_settings_t *s);
bool core_settings_wants_native_1080(const core_settings_t *s);

void core_settings_osd_set_defaults(core_settings_t *s);
void core_settings_osd_normalize(core_settings_t *s);
bool core_settings_osd_font_valid(uint8_t font);
void core_settings_osd_box_size(uint8_t font, uint16_t *out_w, uint16_t *out_h);
void core_settings_osd_pixel_pos(const core_settings_t *s, uint16_t frame_w, uint16_t frame_h,
                                 uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_box_w, uint16_t *out_box_h);

bool core_settings_init(void);
bool core_settings_load(core_settings_t *out);
bool core_settings_save(const core_settings_t *in);
bool core_settings_export_tbrec_override(const core_settings_t *in);

bool core_settings_ensure_id(core_settings_t *s);
bool core_settings_ensure_aes_key(core_settings_t *s);

// Sequenza nome file registrazione (000–999 in NVS, persiste tra boot).
bool core_settings_rec_file_seq_peek(uint16_t *out_next);
bool core_settings_rec_file_seq_commit(uint16_t next_after_use);

#ifdef __cplusplus
}
#endif
