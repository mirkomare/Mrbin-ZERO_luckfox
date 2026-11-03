#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORE_ISP_PROFILE_COUNT      4
#define CORE_ISP_GAIN_MIN_X1000     500
#define CORE_ISP_GAIN_MAX_X1000     3000
#define CORE_ISP_COLOR_MIN          0
#define CORE_ISP_COLOR_MAX          255
#define CORE_ISP_COLOR_DEFAULT      128
#define CORE_ISP_HUE_MAX            360
#define CORE_ISP_HUE_DEFAULT        0
#define CORE_ISP_FOCUS_MIN          0
#define CORE_ISP_FOCUS_MAX          1023
#define CORE_ISP_FOCUS_DEFAULT      400   // posizione tipica trailcam (modulo AF)

typedef struct {
    bool     enabled;
    bool     manual_wb;
    uint8_t  hour_start;
    uint8_t  min_start;
    uint8_t  hour_end;
    uint8_t  min_end;
    uint16_t red_gain_x1000;
    uint16_t blue_gain_x1000;
    int16_t  saturation;
    int16_t  contrast;
    int16_t  brightness;
    uint16_t hue;          // 0–360°, 0 = neutro
    uint16_t focus_pos;    // DW9714 0–1023; ignorato su ottica fissa
    char     name[20];
} core_isp_profile_t;

typedef struct {
    bool     manual_isp_enabled;
    bool     schedule_enabled;
    uint8_t  default_profile;
    core_isp_profile_t profiles[CORE_ISP_PROFILE_COUNT];
} core_isp_cal_t;

void core_isp_cal_init(void);
const core_isp_cal_t *core_isp_cal_get(void);

void core_isp_cal_set_defaults(core_isp_cal_t *cal);
void core_isp_cal_normalize_profile(core_isp_profile_t *p);
bool core_isp_cal_load(core_isp_cal_t *out);
bool core_isp_cal_save(const core_isp_cal_t *in);

const core_isp_profile_t *core_isp_cal_profile_for_now(const core_isp_cal_t *cal);
const core_isp_profile_t *core_isp_cal_profile_by_index(const core_isp_cal_t *cal, unsigned idx);

bool core_isp_cal_apply_auto(void);
// Legge i gain WB attualmente programmati nell'ISP (con AWB attivo = valori
// calcolati dall'algoritmo automatico). Ritorna false se l'ISP non risponde.
bool core_isp_cal_read_wb(uint16_t *red_x1000, uint16_t *blue_x1000);
// Dopo i primi frame: logga WB V4L2 (l'IPA tiene i gain veri).
bool core_isp_cal_startup_wb_check(void);
// IMX708 come imx708_web_stream: AGC HW off, AE IPA target 70.
bool core_isp_cal_imx708_tune_ipa(void);
bool core_isp_cal_apply_profile(const core_isp_profile_t *profile);
bool core_isp_cal_apply_active(void);
// focus_only: applica solo il VCM (niente AWB/ISP) — per slider fuoco in anteprima.
bool core_isp_cal_apply_preview(const core_isp_profile_t *profile, bool manual_isp_enabled,
                                bool focus_only);
// Applica solo il focus (VCM). No-op se modulo senza AF.
bool core_isp_cal_apply_focus(uint16_t focus_pos);
// Salva la posizione fuoco su tutti i profili (ottica trailcam: un solo VCM).
bool core_isp_cal_save_focus_all(uint16_t focus_pos);

// Chiude /dev/video20 (ISP) — obbligatorio prima di esp_video_deinit.
void core_isp_cal_close_device(void);

#ifdef __cplusplus
}
#endif
