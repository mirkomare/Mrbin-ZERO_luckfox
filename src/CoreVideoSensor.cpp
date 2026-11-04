// Selezione modalità sensore (OV5647 960 / IMX708 1080 / SC3336 rkisp).
// Bayer: OV5647 AF→BGGR, fisso→GRBG; IMX708/SC3336→RGGB (driver/rkisp).
#include "CoreConfig.h"
#include "CoreSettings.h"
#include "CoreVideo.h"

#include "esp_cam_sensor_types.h"
#include "esp_log.h"
#include "esp_video_ioctl.h"
#include "sdkconfig.h"

#include "linux/videodev2.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ov5647_settings.h"
#include "ov5647_types.h"

#if CONFIG_CAMERA_IMX708
#include "imx708.h"
#endif

static const char *TAG = "core_video";

// Modulo AF (VCM DW9714/DW9807) vs ottica fissa — impostato dopo video init.
static bool s_module_af = false;
static bool s_module_probed = false;
static uint8_t s_bayer_mode = CORE_VIDEO_BAYER_AUTO;
static core_video_sensor_kind_t s_sensor_kind = CORE_VIDEO_SENSOR_UNKNOWN;

static esp_cam_sensor_bayer_pattern_t core_video_pattern_from_mode(uint8_t mode) {
    switch (mode) {
        case CORE_VIDEO_BAYER_RGGB:
            return ESP_CAM_SENSOR_BAYER_RGGB;
        case CORE_VIDEO_BAYER_GRBG:
            return ESP_CAM_SENSOR_BAYER_GRBG;
        case CORE_VIDEO_BAYER_GBRG:
            return ESP_CAM_SENSOR_BAYER_GBRG;
        case CORE_VIDEO_BAYER_BGGR:
            return ESP_CAM_SENSOR_BAYER_BGGR;
        default:
            return ESP_CAM_SENSOR_BAYER_BGGR;
    }
}

static bool core_video_sensor_rkisp_rggb(void) {
    return s_sensor_kind == CORE_VIDEO_SENSOR_IMX708 ||
           s_sensor_kind == CORE_VIDEO_SENSOR_SC3336;
}

static esp_cam_sensor_bayer_pattern_t core_video_bayer_for_module(void) {
    if (core_video_sensor_rkisp_rggb()) {
        return ESP_CAM_SENSOR_BAYER_RGGB;
    }
    // OV5647 AF Waveshare: BGGR. Fisso: GRBG (patch ov5647 — GBRG/BGGR scambiano R↔B).
    return s_module_af ? ESP_CAM_SENSOR_BAYER_BGGR : ESP_CAM_SENSOR_BAYER_GRBG;
}

// OV5647 driver: 0x3820 bit1 = VFLIP, 0x3821 bit1 = HMIRROR.
// Le tabelle Espressif 960/1080 hanno già lo mirror ON → va riallineato al flag utente
// altrimenti ISP (Bayer con hflip) e sensore (mirror fisso in tabella) divergono.
static void core_video_regs_apply_flip(ov5647_reginfo_t *regs, size_t n, bool vflip, bool hflip) {
    if (!regs) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (regs[i].reg == 0x3820) {
            regs[i].val = (uint8_t)((regs[i].val & (uint8_t)~0x02) | (vflip ? 0x02 : 0x00));
        } else if (regs[i].reg == 0x3821) {
            regs[i].val = (uint8_t)((regs[i].val & (uint8_t)~0x02) | (hflip ? 0x02 : 0x00));
        }
    }
}

// Flip/mirror sul sensore (bit1 di 0x3820/0x3821) spostano la fase Bayer 2×2.
// H: colonne scambiate; V: righe scambiate. Indici enum: RGGB=0 GRBG=1 GBRG=2 BGGR=3.
static esp_cam_sensor_bayer_pattern_t core_video_bayer_with_flip(
        esp_cam_sensor_bayer_pattern_t base, bool vflip, bool hflip) {
    static const esp_cam_sensor_bayer_pattern_t k_h[4] = {
        ESP_CAM_SENSOR_BAYER_GRBG, // RGGB
        ESP_CAM_SENSOR_BAYER_RGGB, // GRBG
        ESP_CAM_SENSOR_BAYER_BGGR, // GBRG
        ESP_CAM_SENSOR_BAYER_GBRG, // BGGR
    };
    static const esp_cam_sensor_bayer_pattern_t k_v[4] = {
        ESP_CAM_SENSOR_BAYER_GBRG, // RGGB
        ESP_CAM_SENSOR_BAYER_BGGR, // GRBG
        ESP_CAM_SENSOR_BAYER_RGGB, // GBRG
        ESP_CAM_SENSOR_BAYER_GRBG, // BGGR
    };
    esp_cam_sensor_bayer_pattern_t b = base;
    if ((unsigned)b > (unsigned)ESP_CAM_SENSOR_BAYER_BGGR) {
        return base;
    }
    if (hflip) {
        b = k_h[b];
    }
    if (vflip) {
        b = k_v[b];
    }
    return b;
}

// Tabella 960/1080 Espressif: 0x3821 bit1 mirror già ON → riferimento hflip=1.
static esp_cam_sensor_bayer_pattern_t core_video_bayer_auto(bool vflip, bool hflip) {
    if (core_video_sensor_rkisp_rggb()) {
        (void)vflip;
        (void)hflip;
        return ESP_CAM_SENSOR_BAYER_RGGB;
    }
    if (s_module_af) {
        return core_video_bayer_with_flip(ESP_CAM_SENSOR_BAYER_BGGR, vflip, hflip);
    }
    const bool ref_hflip = true;
    const bool ref_vflip = false;
    const bool adj_h = (hflip != ref_hflip);
    const bool adj_v = (vflip != ref_vflip);
    return core_video_bayer_with_flip(ESP_CAM_SENSOR_BAYER_GRBG, adj_v, adj_h);
}

static esp_cam_sensor_bayer_pattern_t core_video_bayer_effective(bool vflip, bool hflip) {
    if (core_video_sensor_rkisp_rggb()) {
        if (s_bayer_mode != CORE_VIDEO_BAYER_AUTO) {
            return core_video_pattern_from_mode(s_bayer_mode);
        }
        (void)vflip;
        (void)hflip;
        return ESP_CAM_SENSOR_BAYER_RGGB;
    }
    if (s_bayer_mode != CORE_VIDEO_BAYER_AUTO) {
        // NVS = fase a orientazione 0,0. Flip/mirror spostano la mosaica 2×2
        // (senza questo il verde diventa viola/magenta: R↔B).
        return core_video_bayer_with_flip(core_video_pattern_from_mode(s_bayer_mode),
                                          vflip, hflip);
    }
    return core_video_bayer_auto(vflip, hflip);
}

static const char *core_video_bayer_name(esp_cam_sensor_bayer_pattern_t b) {
    switch (b) {
        case ESP_CAM_SENSOR_BAYER_RGGB: return "RGGB";
        case ESP_CAM_SENSOR_BAYER_GRBG: return "GRBG";
        case ESP_CAM_SENSOR_BAYER_GBRG: return "GBRG";
        case ESP_CAM_SENSOR_BAYER_BGGR: return "BGGR";
        default: return "?";
    }
}

#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS

static ov5647_reginfo_t s_regs_1080[ARRAY_SIZE(ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps)];
static bool s_regs_1080_ready = false;

static esp_cam_sensor_isp_info_t s_ov5647_isp_1080 = {
    .isp_v1_info = {
        .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
        .pclk = 81666700,
        .hts = 2416,
        .vts = 1104,
        .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
    },
};

static esp_cam_sensor_format_t s_ov5647_fmt_1080 = {
    .name = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1920,
    .height = 1080,
    .regs = s_regs_1080,
    .regs_size = ARRAY_SIZE(s_regs_1080),
    .fps = 30,
    .isp_info = &s_ov5647_isp_1080,
    .mipi_info = {
        .mipi_clk = OV5647_MIPI_CSI_LINE_RATE_1920x1080_30FPS,
        .lane_num = 2,
        .line_sync_en = CONFIG_CAMERA_OV5647_CSI_LINESYNC_ENABLE ? true : false,
    },
};
#endif

#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS

static ov5647_reginfo_t s_regs_960[ARRAY_SIZE(ov5647_mipi_2lane_24Minput_1280x960_raw10_45fps)];
static bool s_regs_960_ready = false;

static esp_cam_sensor_isp_info_t s_ov5647_isp_960 = {
    .isp_v1_info = {
        .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
        .pclk = 88333333,
        .hts = 1093,
        .vts = 1796,
        .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
    },
};

static esp_cam_sensor_format_t s_ov5647_fmt_960 = {
    .name = "MIPI_2lane_24Minput_RAW10_1280x960_binning_45fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1280,
    .height = 960,
    .regs = s_regs_960,
    .regs_size = ARRAY_SIZE(s_regs_960),
    .fps = 45,
    .isp_info = &s_ov5647_isp_960,
    .mipi_info = {
        .mipi_clk = OV5647_MIPI_CSI_LINE_RATE_1280x960_45FPS,
        .lane_num = 2,
        .line_sync_en = CONFIG_CAMERA_OV5647_CSI_LINESYNC_ENABLE ? true : false,
    },
};
#endif

void core_video_set_module_af(bool is_af) {
    s_module_af = is_af;
    s_module_probed = true;
    ESP_LOGI(TAG, "Modulo camera: %s → Bayer %s",
             core_video_sensor_label(),
             core_video_bayer_name(core_video_bayer_for_module()));
}

void core_video_reset_module_probe(void) {
    s_module_af = false;
    s_module_probed = false;
    /* Keep s_sensor_kind. Live stop / video_deinit used to clear it to UNKNOWN,
     * and rec then hit the OV5647 1280×960 clamp before detect ran again. */
}

static bool core_video_name_is(const char *s, const char *a, const char *b) {
    return s && ((a && strstr(s, a)) || (b && strstr(s, b)));
}

static bool core_video_sysfs_name_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char buf[64] = {};
    const bool ok = (fgets(buf, sizeof(buf), f) != nullptr) && needle && strstr(buf, needle);
    fclose(f);
    return ok;
}

static bool core_video_sysfs_is_sensor(const char *needle) {
    static const char *k_paths[] = {
        "/sys/bus/i2c/devices/4-0037/name",
        "/sys/bus/i2c/devices/4-0030/name",
        "/sys/bus/i2c/devices/4-0036/name",
        "/sys/bus/i2c/devices/4-001a/name",
        "/sys/bus/i2c/devices/3-0030/name",
    };
    if (!needle) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); ++i) {
        if (core_video_sysfs_name_has(k_paths[i], needle)) {
            return true;
        }
    }
    return false;
}

static bool core_video_module_present(const char *mod) {
    if (!mod) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/sys/module/%s", mod);
    return access(path, F_OK) == 0;
}

void core_video_detect_sensor(void) {
    s_sensor_kind = CORE_VIDEO_SENSOR_UNKNOWN;
    if (core_video_sysfs_is_sensor("imx415") || core_video_sysfs_is_sensor("IMX415") ||
        core_video_module_present("imx415")) {
        s_sensor_kind = CORE_VIDEO_SENSOR_IMX415;
    } else if (core_video_sysfs_is_sensor("sc3336") || core_video_sysfs_is_sensor("SC3336") ||
        core_video_module_present("sc3336")) {
        s_sensor_kind = CORE_VIDEO_SENSOR_SC3336;
    } else if (core_video_sysfs_is_sensor("imx708") || core_video_module_present("imx708")) {
        s_sensor_kind = CORE_VIDEO_SENSOR_IMX708;
    } else if (core_video_sysfs_is_sensor("ov5647") || core_video_module_present("ov5647")) {
        s_sensor_kind = CORE_VIDEO_SENSOR_OV5647;
    }

    const int fd = core_video_device_fd();
    esp_cam_sensor_format_t fmt = {};
    bool got_fmt = false;
    if (fd >= 0) {
        got_fmt = (ioctl(fd, VIDIOC_G_SENSOR_FMT, &fmt) == 0);
        if (s_sensor_kind == CORE_VIDEO_SENSOR_UNKNOWN && got_fmt && fmt.name) {
            if (core_video_name_is(fmt.name, "IMX415", "imx415")) {
                s_sensor_kind = CORE_VIDEO_SENSOR_IMX415;
            } else if (core_video_name_is(fmt.name, "SC3336", "sc3336")) {
                s_sensor_kind = CORE_VIDEO_SENSOR_SC3336;
            } else if (core_video_name_is(fmt.name, "IMX708", "imx708")) {
                s_sensor_kind = CORE_VIDEO_SENSOR_IMX708;
            } else if (core_video_name_is(fmt.name, "OV5647", "ov5647")) {
                s_sensor_kind = CORE_VIDEO_SENSOR_OV5647;
            }
        }
        if (s_sensor_kind == CORE_VIDEO_SENSOR_UNKNOWN) {
            struct v4l2_capability cap = {};
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                const char *card = (const char *)cap.card;
                const char *drv = (const char *)cap.driver;
                if (core_video_name_is(card, "IMX415", "imx415") ||
                    core_video_name_is(drv, "IMX415", "imx415")) {
                    s_sensor_kind = CORE_VIDEO_SENSOR_IMX415;
                } else if (core_video_name_is(card, "SC3336", "sc3336") ||
                    core_video_name_is(drv, "SC3336", "sc3336")) {
                    s_sensor_kind = CORE_VIDEO_SENSOR_SC3336;
                } else if (core_video_name_is(card, "IMX708", "imx708") ||
                           core_video_name_is(drv, "IMX708", "imx708")) {
                    s_sensor_kind = CORE_VIDEO_SENSOR_IMX708;
                } else if (core_video_name_is(card, "OV5647", "ov5647") ||
                           core_video_name_is(drv, "OV5647", "ov5647")) {
                    s_sensor_kind = CORE_VIDEO_SENSOR_OV5647;
                }
            }
        }
        if (s_sensor_kind == CORE_VIDEO_SENSOR_UNKNOWN && got_fmt) {
            if (fmt.width >= 3000 || fmt.height >= 2000) {
                s_sensor_kind = CORE_VIDEO_SENSOR_IMX415;
            } else if (fmt.width >= 2200 || fmt.height >= 1200) {
                s_sensor_kind = CORE_VIDEO_SENSOR_SC3336;
            } else if (fmt.width >= 1920 && fmt.height >= 1080 && fmt.fps >= 25 && fmt.fps <= 30) {
                s_sensor_kind = CORE_VIDEO_SENSOR_IMX708;
            } else {
                s_sensor_kind = CORE_VIDEO_SENSOR_OV5647;
            }
        }
    }
    if (s_sensor_kind == CORE_VIDEO_SENSOR_UNKNOWN) {
        s_sensor_kind = CORE_VIDEO_SENSOR_IMX415;
        ESP_LOGW(TAG, "Sensore unknown — default IMX415 (Pico Zero CSI 4 lane)");
    }
    ESP_LOGI(TAG, "Sensore rilevato: %s (%s %ux%u @ %u fps)",
             core_video_sensor_name(),
             (got_fmt && fmt.name) ? fmt.name : "?",
             (unsigned)(got_fmt ? fmt.width : 0),
             (unsigned)(got_fmt ? fmt.height : 0),
             (unsigned)(got_fmt ? fmt.fps : 0));

    // IMX708: lascia Bayer/CCM al driver+IPA (imx708_web_stream). Niente override.
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX708 && s_bayer_mode != CORE_VIDEO_BAYER_AUTO) {
        ESP_LOGW(TAG, "Bayer NVS %s ignorato su IMX708 — Auto (driver RGGB)",
                 core_video_bayer_mode_label(s_bayer_mode));
        s_bayer_mode = CORE_VIDEO_BAYER_AUTO;
        core_settings_t st;
        if (core_settings_load(&st) && st.video_bayer_mode != CORE_VIDEO_BAYER_AUTO) {
            st.video_bayer_mode = CORE_VIDEO_BAYER_AUTO;
            (void)core_settings_save(&st);
        }
    }

    // NVS da OV5647 (1280×960@12) o fps bassi: IMX708 nativo 1920×1080@28.
    core_settings_t vs;
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX708 && core_settings_load(&vs)) {
        const bool keep_720 = (vs.video_width == 1280 && vs.video_height == 720);
        const bool keep_768 = (vs.video_width == 1024 && vs.video_height == 768);
        bool changed = false;
        if (!keep_720 && !keep_768 &&
            (vs.video_width < 1920 || vs.video_height < 1080)) {
            vs.video_width = 1920;
            vs.video_height = 1080;
            changed = true;
        }
        if (vs.video_fps < 20) {
            vs.video_fps = 28;
            changed = true;
        }
        if (changed) {
            ESP_LOGI(TAG, "IMX708: NVS video → %ux%u @ %u fps",
                     (unsigned)vs.video_width, (unsigned)vs.video_height,
                     (unsigned)vs.video_fps);
            (void)core_settings_save(&vs);
        }
    }
}

core_video_sensor_kind_t core_video_sensor_kind(void) {
    return s_sensor_kind;
}

const char *core_video_sensor_name(void) {
    switch (s_sensor_kind) {
        case CORE_VIDEO_SENSOR_IMX708:
            return "IMX708";
        case CORE_VIDEO_SENSOR_OV5647:
            return "OV5647";
        case CORE_VIDEO_SENSOR_SC3336:
            return "SC3336";
        case CORE_VIDEO_SENSOR_IMX415:
            return "IMX415";
        default:
            return "unknown";
    }
}

const char *core_video_sensor_label(void) {
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX415) {
        return "IMX415 8MP (CSI 4 lane, 0x37)";
    }
    if (s_sensor_kind == CORE_VIDEO_SENSOR_SC3336) {
        return "SC3336 (CSI rkisp)";
    }
    if (!s_module_probed) {
        return "auto (rilevamento all'avvio stream)";
    }
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX708) {
        return s_module_af ? "IMX708-AF (DW9807)" : "IMX708";
    }
    if (s_sensor_kind == CORE_VIDEO_SENSOR_OV5647) {
        return s_module_af ? "OV5647-AF (DW9714)" : "OV5647 fisso";
    }
    return s_module_af ? "camera AF" : "camera fissa";
}

void core_video_set_bayer_mode(uint8_t mode) {
    if (!core_settings_bayer_mode_valid(mode)) {
        mode = CORE_VIDEO_BAYER_AUTO;
    }
    s_bayer_mode = mode;
    ESP_LOGI(TAG, "Bayer ISP: modalità %s", core_video_bayer_mode_label(mode));
}

uint8_t core_video_get_bayer_mode(void) {
    return s_bayer_mode;
}

const char *core_video_bayer_mode_label(uint8_t mode) {
    switch (mode) {
        case CORE_VIDEO_BAYER_AUTO:
            return "Auto";
        case CORE_VIDEO_BAYER_RGGB:
            return "RGGB";
        case CORE_VIDEO_BAYER_GRBG:
            return "GRBG";
        case CORE_VIDEO_BAYER_GBRG:
            return "GBRG";
        case CORE_VIDEO_BAYER_BGGR:
            return "BGGR";
        default:
            return "?";
    }
}

const char *core_video_bayer_effective_name(bool vflip, bool hflip) {
    return core_video_bayer_name(core_video_bayer_effective(vflip, hflip));
}

bool core_video_apply_bayer_sync(void) {
    return core_video_sync_bayer_order(core_video_get_vflip(), core_video_get_hflip());
}

bool core_video_module_is_af(void) {
    return s_module_af;
}

bool core_video_module_probed(void) {
    return s_module_probed;
}

bool core_settings_wants_native_1080(const core_settings_t *s) {
    if (!s) {
        return false;
    }
    return s->video_width >= CORE_VIDEO_WIDTH_MAX && s->video_height >= CORE_VIDEO_HEIGHT_MAX;
}

static bool core_video_sensor_get_size(uint16_t *out_w, uint16_t *out_h) {
    if (!out_w || !out_h || !core_video_is_ready()) {
        return false;
    }
    const int fd = core_video_device_fd();
    if (fd < 0) {
        return false;
    }
    esp_cam_sensor_format_t fmt = {};
    bool ok = (ioctl(fd, VIDIOC_G_SENSOR_FMT, &fmt) == 0);
    if (!ok) {
        return false;
    }
    *out_w = (uint16_t)fmt.width;
    *out_h = (uint16_t)fmt.height;
    return true;
}

static bool core_video_verify_capture_size(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0 || !core_video_is_ready()) {
        return false;
    }

    const int fd = core_video_device_fd();
    if (fd < 0) {
        ESP_LOGE(TAG, "FD video assente per verifica V4L2");
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT fallito");
        return false;
    }

    ESP_LOGI(TAG, "V4L2 capture: %ux%u (atteso %ux%u)", (unsigned)fmt.fmt.pix.width,
             (unsigned)fmt.fmt.pix.height, (unsigned)width, (unsigned)height);

    return fmt.fmt.pix.width == width && fmt.fmt.pix.height == height;
}

static void core_video_refresh_bayer_in_formats(bool vflip, bool hflip) {
    const esp_cam_sensor_bayer_pattern_t bayer = core_video_bayer_effective(vflip, hflip);
#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS
    s_ov5647_isp_1080.isp_v1_info.bayer_type = bayer;
#endif
#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS
    s_ov5647_isp_960.isp_v1_info.bayer_type = bayer;
#endif
}

static bool core_video_prepare_format_regs(esp_cam_sensor_format_t *fmt, bool vflip, bool hflip) {
    if (!fmt) {
        return false;
    }
#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS
    if (fmt == &s_ov5647_fmt_1080) {
        if (!s_regs_1080_ready) {
            memcpy(s_regs_1080, ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps, sizeof(s_regs_1080));
            s_regs_1080_ready = true;
        }
        core_video_regs_apply_flip(s_regs_1080, ARRAY_SIZE(s_regs_1080), vflip, hflip);
        return true;
    }
#endif
#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS
    if (fmt == &s_ov5647_fmt_960) {
        if (!s_regs_960_ready) {
            memcpy(s_regs_960, ov5647_mipi_2lane_24Minput_1280x960_raw10_45fps, sizeof(s_regs_960));
            s_regs_960_ready = true;
        }
        core_video_regs_apply_flip(s_regs_960, ARRAY_SIZE(s_regs_960), vflip, hflip);
        return true;
    }
#endif
    return false;
}

static bool core_video_apply_sensor_format(esp_cam_sensor_format_t *fmt, bool vflip, bool hflip) {
    if (!fmt || !core_video_is_ready()) {
        return false;
    }
    if (!core_video_prepare_format_regs(fmt, vflip, hflip)) {
        ESP_LOGE(TAG, "Prepare registri formato fallita");
        return false;
    }
    core_video_refresh_bayer_in_formats(vflip, hflip);
    const int fd = core_video_device_fd();
    if (fd < 0) {
        ESP_LOGE(TAG, "FD video assente per cambio formato sensore");
        return false;
    }
    // Due passaggi: il primo collega il nostro format; il secondo assicura
    // che CSI/ISP leggano il bayer_type aggiornato (e i flip nei regs).
    bool ok = (ioctl(fd, VIDIOC_S_SENSOR_FMT, fmt) == 0);
    if (ok) {
        core_video_refresh_bayer_in_formats(vflip, hflip);
        ok = (ioctl(fd, VIDIOC_S_SENSOR_FMT, fmt) == 0);
    }
    if (!ok) {
        ESP_LOGE(TAG, "VIDIOC_S_SENSOR_FMT %s fallito", fmt->name ? fmt->name : "?");
        return false;
    }
    // Allinea anche i control V4L2 (stessi bit già nei regs del format).
    core_video_reapply_flip();
    ESP_LOGI(TAG, "Sensore %s: %ux%u nativo (%s) Bayer=%s (%s%s vflip=%d hflip=%d)",
             core_video_sensor_name(),
             (unsigned)fmt->width, (unsigned)fmt->height,
             fmt->name ? fmt->name : "formato",
             core_video_bayer_name(core_video_bayer_effective(vflip, hflip)),
             s_module_af ? "AF" : "fisso",
             s_bayer_mode != CORE_VIDEO_BAYER_AUTO ? " manuale" : "",
             (int)vflip, (int)hflip);
    return true;
}

#if CONFIG_CAMERA_IMX708
static bool core_video_apply_imx708_index(size_t index, bool vflip, bool hflip) {
    if (!core_video_is_ready()) {
        return false;
    }
    const esp_cam_sensor_format_t *src = imx708_format_by_index(index);
    if (!src) {
        ESP_LOGE(TAG, "IMX708: indice formato %u non valido", (unsigned)index);
        return false;
    }
    // isp_v1_info.version è const: memcpy, niente copy-assign C++.
    alignas(esp_cam_sensor_format_t) static uint8_t s_imx_fmt_raw[sizeof(esp_cam_sensor_format_t)];
    alignas(esp_cam_sensor_isp_info_t) static uint8_t s_imx_isp_raw[sizeof(esp_cam_sensor_isp_info_t)];
    auto *s_imx_fmt = reinterpret_cast<esp_cam_sensor_format_t *>(s_imx_fmt_raw);
    auto *s_imx_isp = reinterpret_cast<esp_cam_sensor_isp_info_t *>(s_imx_isp_raw);
    memcpy(s_imx_fmt_raw, src, sizeof(s_imx_fmt_raw));
    if (src->isp_info) {
        memcpy(s_imx_isp_raw, src->isp_info, sizeof(s_imx_isp_raw));
        s_imx_fmt->isp_info = s_imx_isp;
    }
    // esp_video resolve_sensor_format ripristina la entry della tabella se
    // `regs` è lo stesso puntatore — e butta via il nostro bayer_type.
    // Stessa layout di imx708_reginfo_t (non includere settings.h: tabelle enormi).
    typedef struct {
        uint16_t reg;
        uint8_t val;
    } core_imx708_reg_t;
    static core_imx708_reg_t s_imx_regs_dup[512];
    const size_t nregs = (src->regs_size > 0) ? (size_t)src->regs_size : 0;
    if (src->regs && nregs > 0 && nregs <= (sizeof(s_imx_regs_dup) / sizeof(s_imx_regs_dup[0]))) {
        memcpy(s_imx_regs_dup, src->regs, nregs * sizeof(s_imx_regs_dup[0]));
        s_imx_fmt->regs = s_imx_regs_dup;
    }
    const int fd = core_video_device_fd();
    if (fd < 0) {
        ESP_LOGE(TAG, "FD video assente per formato IMX708");
        return false;
    }
    bool ok = (ioctl(fd, VIDIOC_S_SENSOR_FMT, s_imx_fmt) == 0);
    if (ok) {
        ok = (ioctl(fd, VIDIOC_S_SENSOR_FMT, s_imx_fmt) == 0);
    }
    if (!ok) {
        ESP_LOGE(TAG, "VIDIOC_S_SENSOR_FMT IMX708 %s fallito",
                 s_imx_fmt->name ? s_imx_fmt->name : "?");
        return false;
    }
    core_video_reapply_flip();
    ESP_LOGI(TAG, "Sensore IMX708: %ux%u (%s) Bayer=%s (%s%s vflip=%d hflip=%d)",
             (unsigned)s_imx_fmt->width, (unsigned)s_imx_fmt->height,
             s_imx_fmt->name ? s_imx_fmt->name : "formato",
             core_video_bayer_name(core_video_bayer_effective(vflip, hflip)),
             s_module_af ? "AF" : "fisso",
             s_bayer_mode != CORE_VIDEO_BAYER_AUTO ? " manuale" : "",
             (int)vflip, (int)hflip);
    return true;
}
#endif

bool core_video_sync_bayer_order(bool vflip, bool hflip) {
    if (!core_video_is_ready()) {
        return false;
    }
    if (s_sensor_kind == CORE_VIDEO_SENSOR_SC3336) {
        (void)vflip;
        (void)hflip;
        /* Rockchip rkisp + sc3336.ko own Bayer; ESP VIDIOC_S_SENSOR_FMT is a no-op. */
        core_video_reapply_flip();
        return true;
    }
#if CONFIG_CAMERA_IMX708
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX708) {
        return core_video_apply_imx708_index(0, vflip, hflip);
    }
#endif
    uint16_t cur_w = 0;
    uint16_t cur_h = 0;
    (void)core_video_sensor_get_size(&cur_w, &cur_h);

#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS
    if (cur_w >= 1920 && cur_h >= 1080) {
        return core_video_apply_sensor_format(&s_ov5647_fmt_1080, vflip, hflip);
    }
#endif
#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS
    return core_video_apply_sensor_format(&s_ov5647_fmt_960, vflip, hflip);
#else
    ESP_LOGW(TAG, "Nessun formato OV5647 compilato per sync Bayer");
    return false;
#endif
}

bool core_video_get_sensor_size(uint16_t *out_w, uint16_t *out_h) {
    return core_video_sensor_get_size(out_w, out_h);
}

bool core_video_get_sensor_fps(uint8_t *out_fps) {
    if (!out_fps || !core_video_is_ready()) {
        return false;
    }
    const int fd = core_video_device_fd();
    if (fd < 0) {
        return false;
    }
    esp_cam_sensor_format_t fmt = {};
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &fmt) != 0 || fmt.fps == 0) {
        return false;
    }
    *out_fps = (uint8_t)(fmt.fps > 60 ? 60 : fmt.fps);
    return true;
}

size_t core_video_imx708_mode_index(uint16_t want_w, uint16_t want_h) {
    if (want_w >= 1920 || want_h >= 1000 || (want_w >= 1280 && want_h >= 900)) {
        return 0;
    }
    if (want_w >= 1280 && want_h >= 720 && want_h < 768) {
        return 1;
    }
    if (want_w >= 1024 && want_h >= 768) {
        return 2;
    }
    if (want_w >= 1280) {
        return 1;
    }
    return 0;
}

void core_video_imx708_encode_size(uint16_t want_w, uint16_t want_h,
                                   uint16_t *out_w, uint16_t *out_h) {
    if (!out_w || !out_h) {
        return;
    }
    const size_t idx = core_video_imx708_mode_index(want_w, want_h);
    if (idx == 1) {
        *out_w = 1280;
        *out_h = 720;
    } else if (idx == 2) {
        *out_w = 1024;
        *out_h = 768;
    } else {
        *out_w = CORE_IMX708_CLEAN_W;
        *out_h = CORE_IMX708_CLEAN_H;
    }
}

bool core_video_imx708_trim_rgn(uint16_t sw, uint16_t sh,
                                uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h) {
    if (!x || !y || !w || !h) {
        return false;
    }
    if (sw < 1920 || sh < 1080) {
        return false;
    }
    *x = 0;
    *y = 0;
    *w = CORE_IMX708_CLEAN_W;
    *h = CORE_IMX708_CLEAN_H;
    return true;
}

bool core_video_prepare_for_settings(const core_settings_t *s) {
    if (!s || !core_video_is_ready()) {
        return false;
    }

    const bool want_1080 = core_settings_wants_native_1080(s);
    const bool vflip = core_video_get_vflip();
    const bool hflip = core_video_get_hflip();
    uint16_t target_w = 1280;
    uint16_t target_h = 960;
    bool sensor_ok = true;

    if (s_sensor_kind == CORE_VIDEO_SENSOR_SC3336) {
        ESP_LOGI(TAG, "SC3336: rkisp native %ux%u, encode %ux%u@%u (no ESP S_SENSOR_FMT)",
                 (unsigned)CORE_VIDEO_SENSOR_NATIVE_W, (unsigned)CORE_VIDEO_SENSOR_NATIVE_H,
                 (unsigned)s->video_width, (unsigned)s->video_height, (unsigned)s->video_fps);
        core_video_reapply_flip();
        return true;
    }

#if CONFIG_CAMERA_IMX708
    if (s_sensor_kind == CORE_VIDEO_SENSOR_IMX708) {
        const size_t idx = core_video_imx708_mode_index(s->video_width, s->video_height);
        if (idx == 1) {
            target_w = 1280;
            target_h = 720;
        } else if (idx == 2) {
            target_w = 1024;
            target_h = 768;
        } else {
            target_w = 1920;
            target_h = 1080;
        }
        ESP_LOGI(TAG, "IMX708: modo %u %ux%u (impostazione %ux%u)",
                 (unsigned)idx, (unsigned)target_w, (unsigned)target_h,
                 (unsigned)s->video_width, (unsigned)s->video_height);
        sensor_ok = core_video_apply_imx708_index(idx, vflip, hflip);
        if (!sensor_ok) {
            ESP_LOGW(TAG, "Cambio formato IMX708 fallito");
        }
        if (!core_video_verify_capture_size(target_w, target_h)) {
            ESP_LOGW(TAG, "V4L2 %ux%u non allineato al sensore IMX708",
                     (unsigned)target_w, (unsigned)target_h);
            return sensor_ok;
        }
        return true;
    }
#endif

    // 1080p nativo OV5647 Espressif è un crop centrale ~1928×1088 sul
    // 2592×1944 (FOV ~1.3× più stretto del 960 binning). FullHD deve
    // inquadrare la stessa scena del 960/live: restiamo in 1280×960 e
    // il PPA ritaglia solo 16:9 (alto/basso) poi scala a 1920×1080.
    if (want_1080) {
        ESP_LOGI(TAG, "FullHD %ux%u: sensore 1280×960 (stesso FOV del binning), "
                 "PPA 16:9 → %ux%u — niente crop nativo 1080p",
                 (unsigned)s->video_width, (unsigned)s->video_height,
                 (unsigned)s->video_width, (unsigned)s->video_height);
    }

#if CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS
    sensor_ok = core_video_apply_sensor_format(&s_ov5647_fmt_960, vflip, hflip);
#endif

    if (!sensor_ok) {
        ESP_LOGW(TAG, "Cambio formato sensore fallito");
    }

    if (!core_video_verify_capture_size(target_w, target_h)) {
        ESP_LOGW(TAG, "V4L2 %ux%u non allineato al sensore — pipeline potrebbe upscalare",
                 (unsigned)target_w, (unsigned)target_h);
        return sensor_ok;
    }
    return true;
}
