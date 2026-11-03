#include "CoreIspCal.h"
#include "CoreConfig.h"
#include "CoreTime.h"
#include "CoreVideo.h"

#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"
#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
#include "esp_ipa.h"
#include "esp_video_isp_pipeline.h"
#endif
#include "linux/videodev2.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static const char *TAG = "core_isp";
static const uint32_t CORE_ISP_CAL_MAGIC = 0x49535031;
static const uint8_t CORE_ISP_CAL_VERSION = 4;

static int s_isp_fd = -1;
static core_isp_cal_t s_cal_cache;
static bool s_cal_loaded = false;
static bool s_startup_wb_done = false;

// Layout profilo fino alla versione 2 (senza hue) — solo per migrazione blob.
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
    char     name[20];
} core_isp_profile_v2_t;

typedef struct {
    bool schedule_enabled;
    uint8_t fallback_profile;
    bool pir_wifi_time_sync;
    core_isp_profile_v2_t profiles[CORE_ISP_PROFILE_COUNT];
} core_isp_cal_v1_t;

typedef struct {
    bool     manual_isp_enabled;
    bool     schedule_enabled;
    uint8_t  default_profile;
    core_isp_profile_v2_t profiles[CORE_ISP_PROFILE_COUNT];
} core_isp_cal_v2_t;

// Profilo v3 (hue, senza focus) — migrazione blob NVS.
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
    uint16_t hue;
    char     name[20];
} core_isp_profile_v3_t;

typedef struct {
    bool     manual_isp_enabled;
    bool     schedule_enabled;
    uint8_t  default_profile;
    core_isp_profile_v3_t profiles[CORE_ISP_PROFILE_COUNT];
} core_isp_cal_v3_t;

static void profile_from_v2(const core_isp_profile_v2_t *in, core_isp_profile_t *out) {
    out->enabled = in->enabled;
    out->manual_wb = in->manual_wb;
    out->hour_start = in->hour_start;
    out->min_start = in->min_start;
    out->hour_end = in->hour_end;
    out->min_end = in->min_end;
    out->red_gain_x1000 = in->red_gain_x1000;
    out->blue_gain_x1000 = in->blue_gain_x1000;
    out->saturation = in->saturation;
    out->contrast = in->contrast;
    out->brightness = in->brightness;
    out->hue = CORE_ISP_HUE_DEFAULT;
    out->focus_pos = CORE_ISP_FOCUS_DEFAULT;
    memcpy(out->name, in->name, sizeof(out->name));
    out->name[sizeof(out->name) - 1] = '\0';
}

static void profile_from_v3(const core_isp_profile_v3_t *in, core_isp_profile_t *out) {
    out->enabled = in->enabled;
    out->manual_wb = in->manual_wb;
    out->hour_start = in->hour_start;
    out->min_start = in->min_start;
    out->hour_end = in->hour_end;
    out->min_end = in->min_end;
    out->red_gain_x1000 = in->red_gain_x1000;
    out->blue_gain_x1000 = in->blue_gain_x1000;
    out->saturation = in->saturation;
    out->contrast = in->contrast;
    out->brightness = in->brightness;
    out->hue = in->hue;
    out->focus_pos = CORE_ISP_FOCUS_DEFAULT;
    memcpy(out->name, in->name, sizeof(out->name));
    out->name[sizeof(out->name) - 1] = '\0';
}

static int core_isp_open_device(void) {
    if (s_isp_fd >= 0) {
        return s_isp_fd;
    }
    int fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ESP_LOGW(TAG, "open(%s) fallito", ESP_VIDEO_ISP1_DEVICE_NAME);
        return -1;
    }
    s_isp_fd = fd;
    return fd;
}

void core_isp_cal_close_device(void) {
    if (s_isp_fd >= 0) {
        close(s_isp_fd);
        s_isp_fd = -1;
    }
    s_startup_wb_done = false;
}

/* SC3336 CPROC is the rkaiq ACP module. The running rkaiq_3A_server owns the
 * context; this process sends ENUM_ID_CPROC_SETATTRIB (0x1D01) on its unix
 * socket. 128 is neutral. Hue 0 is the old OV "neutral" and maps to 128. */
static bool core_isp_is_sc3336(void) {
    const core_video_sensor_kind_t k = core_video_sensor_kind();
    return k == CORE_VIDEO_SENSOR_SC3336 || k == CORE_VIDEO_SENSOR_IMX415 ||
           k == CORE_VIDEO_SENSOR_UNKNOWN;
}

static int core_isp_cproc_u8(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static int core_isp_cproc_hue(uint16_t hue) {
    if (hue == 0 || hue > 255) {
        return 128;
    }
    return (int)hue;
}

/* Same mix the 3A server uses to accept a tool packet (seed 0x61, m = 0x5bd1e995). */
static uint32_t core_isp_rkaiq_hash(const uint8_t *key, int len) {
    const uint32_t m = 0x5bd1e995u;
    uint32_t h = (uint32_t)len ^ 0x61u;
    while (len > 3) {
        uint32_t k;
        memcpy(&k, key, 4);
        key += 4;
        len -= 4;
        h *= m;
        k *= m;
        k ^= k >> 24;
        k *= m;
        h ^= k;
    }
    if (len == 3) {
        h ^= (uint32_t)key[2] << 16;
    }
    if (len >= 2) {
        h ^= (uint32_t)key[1] << 8;
    }
    if (len >= 1) {
        h ^= key[0];
        h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

static int core_isp_rkaiq_connect(void) {
    static const char *paths[] = {"/tmp/UNIX.domain0", "/tmp/UNIX.domain", nullptr};
    for (int i = 0; paths[i]; ++i) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", paths[i]);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* acp_attrib_t: sync_mode u32, done u8, pad 3, then brightness/contrast/saturation/hue. */
static bool core_isp_cproc_set(int bright, int contrast, int sat, int hue) {
    const int b = core_isp_cproc_u8(bright);
    const int c = core_isp_cproc_u8(contrast);
    const int s = core_isp_cproc_u8(sat);
    const int h = core_isp_cproc_u8(hue);
    int fd = core_isp_rkaiq_connect();
    if (fd < 0) {
        static int fails = 0;
        if ((fails++ % 20) == 0) {
            ESP_LOGW(TAG, "ACP SC3336: socket 3A non raggiungibile");
        }
        return false;
    }

    uint8_t attr[12];
    memset(attr, 0, sizeof(attr));
    attr[0] = 1; /* RK_AIQ_UAPI_MODE_SYNC */
    attr[8] = (uint8_t)b;
    attr[9] = (uint8_t)c;
    attr[10] = (uint8_t)s;
    attr[11] = (uint8_t)h;

    uint8_t pkt[40];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 'R';
    pkt[1] = 'K';
    const uint32_t data_size = 12;
    const uint32_t packet_size = 22 + data_size;
    const uint32_t cmd = 0x1D01; /* ENUM_ID_CPROC_SETATTRIB */
    memcpy(pkt + 2, &packet_size, 4);
    memcpy(pkt + 6, &cmd, 4);
    memcpy(pkt + 14, &data_size, 4);
    memcpy(pkt + 18, attr, data_size);
    const uint32_t hash = core_isp_rkaiq_hash(pkt + 18, (int)data_size);
    memcpy(pkt + 18 + data_size, &hash, 4);

    bool ok = false;
    if (send(fd, pkt, packet_size, 0) == (ssize_t)packet_size) {
        uint8_t reply[64];
        int got = 0;
        while (got < 22) {
            const ssize_t n = recv(fd, reply + got, sizeof(reply) - (size_t)got, 0);
            if (n <= 0) {
                break;
            }
            got += (int)n;
        }
        if (got >= 22 && reply[0] == 'R' && reply[1] == 'K') {
            int32_t result = 0;
            memcpy(&result, reply + 10, 4);
            ok = (result == 0);
            if (!ok) {
                ESP_LOGW(TAG, "ACP SC3336 rifiutato (%d)", (int)result);
            }
        }
    }
    close(fd);
    if (ok) {
        static int last_b = -1, last_c = -1, last_s = -1, last_h = -1;
        if (b != last_b || c != last_c || s != last_s || h != last_h) {
            last_b = b;
            last_c = c;
            last_s = s;
            last_h = h;
            ESP_LOGI(TAG, "ACP SC3336 %d/%d/%d/%d", b, c, s, h);
        }
    }
    return ok;
}

static bool core_isp_cproc_neutral(void) {
    return core_isp_cproc_set(128, 128, 128, 128);
}

static bool core_isp_cproc_from_profile(const core_isp_profile_t *profile) {
    if (!profile) {
        return false;
    }
    return core_isp_cproc_set(profile->brightness, profile->contrast,
                              profile->saturation, core_isp_cproc_hue(profile->hue));
}

static bool core_isp_set_ext_ctrl(int fd, uint32_t id, void *payload, size_t payload_size) {
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = id;
    control.size = payload_size;
    control.p_u8 = (uint8_t *)payload;
    controls.ctrl_class = V4L2_CID_USER_CLASS;
    controls.count = 1;
    controls.controls = &control;
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0;
}

static void core_isp_clamp_profile(core_isp_profile_t *p) {
    if (!p) {
        return;
    }
    if (p->hour_start > 23) {
        p->hour_start = 0;
    }
    if (p->hour_end > 23) {
        p->hour_end = 23;
    }
    if (p->min_start > 59) {
        p->min_start = 0;
    }
    if (p->min_end > 59) {
        p->min_end = 59;
    }
    if (p->red_gain_x1000 < CORE_ISP_GAIN_MIN_X1000) {
        p->red_gain_x1000 = CORE_ISP_GAIN_MIN_X1000;
    }
    if (p->red_gain_x1000 > CORE_ISP_GAIN_MAX_X1000) {
        p->red_gain_x1000 = CORE_ISP_GAIN_MAX_X1000;
    }
    if (p->blue_gain_x1000 < CORE_ISP_GAIN_MIN_X1000) {
        p->blue_gain_x1000 = CORE_ISP_GAIN_MIN_X1000;
    }
    if (p->blue_gain_x1000 > CORE_ISP_GAIN_MAX_X1000) {
        p->blue_gain_x1000 = CORE_ISP_GAIN_MAX_X1000;
    }
    if (p->saturation < CORE_ISP_COLOR_MIN || p->saturation > CORE_ISP_COLOR_MAX) {
        p->saturation = CORE_ISP_COLOR_DEFAULT;
    }
    if (p->contrast < CORE_ISP_COLOR_MIN || p->contrast > CORE_ISP_COLOR_MAX) {
        p->contrast = CORE_ISP_COLOR_DEFAULT;
    }
    if (p->brightness < CORE_ISP_COLOR_MIN || p->brightness > CORE_ISP_COLOR_MAX) {
        p->brightness = CORE_ISP_COLOR_DEFAULT;
    }
    if (p->hue > CORE_ISP_HUE_MAX) {
        p->hue = CORE_ISP_HUE_DEFAULT;
    }
    if (p->focus_pos > CORE_ISP_FOCUS_MAX) {
        p->focus_pos = CORE_ISP_FOCUS_DEFAULT;
    }
    if (p->name[0] == '\0') {
        snprintf(p->name, sizeof(p->name), "Profilo");
    }
}

void core_isp_cal_set_defaults(core_isp_cal_t *cal) {
    if (!cal) {
        return;
    }
    memset(cal, 0, sizeof(*cal));
    cal->manual_isp_enabled = false;
    cal->schedule_enabled = true;
    cal->default_profile = 1;

    const struct {
        const char *name;
        uint8_t h0, m0, h1, m1;
        uint16_t red, blue;
        bool manual;
    } presets[] = {
        {"Alba", 5, 0, 8, 59, 1400, 900, true},
        {"Giorno", 9, 0, 16, 59, 1200, 1100, false},
        {"Tramonto", 17, 0, 20, 59, 1500, 850, true},
        {"Notte", 21, 0, 4, 59, 1100, 1300, true},
    };

    for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
        core_isp_profile_t *p = &cal->profiles[i];
        p->enabled = true;
        p->manual_wb = presets[i].manual;
        p->hour_start = presets[i].h0;
        p->min_start = presets[i].m0;
        p->hour_end = presets[i].h1;
        p->min_end = presets[i].m1;
        p->red_gain_x1000 = presets[i].red;
        p->blue_gain_x1000 = presets[i].blue;
        p->saturation = CORE_ISP_COLOR_DEFAULT;
        p->contrast = CORE_ISP_COLOR_DEFAULT;
        p->brightness = CORE_ISP_COLOR_DEFAULT;
        p->hue = CORE_ISP_HUE_DEFAULT;
        p->focus_pos = CORE_ISP_FOCUS_DEFAULT;
        snprintf(p->name, sizeof(p->name), "%s", presets[i].name);
    }
}

void core_isp_cal_normalize_profile(core_isp_profile_t *p) {
    core_isp_clamp_profile(p);
}

static bool core_isp_cal_valid(const core_isp_cal_t *cal) {
    return cal && cal->default_profile < CORE_ISP_PROFILE_COUNT;
}

static void core_isp_cal_migrate_v1(const core_isp_cal_v1_t *v1, core_isp_cal_t *out) {
    core_isp_cal_set_defaults(out);
    out->manual_isp_enabled = true;
    out->schedule_enabled = v1->schedule_enabled;
    out->default_profile = v1->fallback_profile;
    if (out->default_profile >= CORE_ISP_PROFILE_COUNT) {
        out->default_profile = 0;
    }
    for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
        profile_from_v2(&v1->profiles[i], &out->profiles[i]);
        core_isp_clamp_profile(&out->profiles[i]);
    }
}

static void core_isp_cal_migrate_v2(const core_isp_cal_v2_t *v2, core_isp_cal_t *out) {
    core_isp_cal_set_defaults(out);
    out->manual_isp_enabled = v2->manual_isp_enabled;
    out->schedule_enabled = v2->schedule_enabled;
    out->default_profile = v2->default_profile;
    if (out->default_profile >= CORE_ISP_PROFILE_COUNT) {
        out->default_profile = 0;
    }
    for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
        profile_from_v2(&v2->profiles[i], &out->profiles[i]);
        core_isp_clamp_profile(&out->profiles[i]);
    }
}

bool core_isp_cal_load(core_isp_cal_t *out) {
    if (!out) {
        return false;
    }
    core_isp_cal_set_defaults(out);

    nvs_handle_t nvs = 0;
    if (nvs_open(CORE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return true;
    }

    struct {
        uint32_t magic;
        uint8_t version;
        core_isp_cal_t cal;
    } blob = {};

    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(nvs, "isp_cal", &blob, &len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        return true;
    }

    if (len < 5 || blob.magic != CORE_ISP_CAL_MAGIC) {
        return true;
    }

    if (blob.version == CORE_ISP_CAL_VERSION && len == sizeof(blob) && core_isp_cal_valid(&blob.cal)) {
        *out = blob.cal;
        for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
            core_isp_clamp_profile(&out->profiles[i]);
        }
        return true;
    }

    if (blob.version == 3) {
        struct {
            uint32_t magic;
            uint8_t version;
            core_isp_cal_v3_t cal;
        } old = {};
        if (len == sizeof(old)) {
            if (nvs_open(CORE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                len = sizeof(old);
                if (nvs_get_blob(nvs, "isp_cal", &old, &len) == ESP_OK) {
                    core_isp_cal_set_defaults(out);
                    out->manual_isp_enabled = old.cal.manual_isp_enabled;
                    out->schedule_enabled = old.cal.schedule_enabled;
                    out->default_profile = old.cal.default_profile;
                    if (out->default_profile >= CORE_ISP_PROFILE_COUNT) {
                        out->default_profile = 0;
                    }
                    for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
                        profile_from_v3(&old.cal.profiles[i], &out->profiles[i]);
                        core_isp_clamp_profile(&out->profiles[i]);
                    }
                }
                nvs_close(nvs);
            }
        }
    } else if (blob.version == 2) {
        struct {
            uint32_t magic;
            uint8_t version;
            core_isp_cal_v2_t cal;
        } old = {};
        if (len == sizeof(old)) {
            if (nvs_open(CORE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                len = sizeof(old);
                if (nvs_get_blob(nvs, "isp_cal", &old, &len) == ESP_OK) {
                    core_isp_cal_migrate_v2(&old.cal, out);
                }
                nvs_close(nvs);
            }
        }
    } else if (blob.version == 1) {
        struct {
            uint32_t magic;
            uint8_t version;
            core_isp_cal_v1_t cal;
        } old = {};
        if (len == sizeof(old)) {
            if (nvs_open(CORE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                len = sizeof(old);
                if (nvs_get_blob(nvs, "isp_cal", &old, &len) == ESP_OK) {
                    core_isp_cal_migrate_v1(&old.cal, out);
                }
                nvs_close(nvs);
            }
        }
    }
    return true;
}

bool core_isp_cal_save(const core_isp_cal_t *in) {
    if (!in || !core_isp_cal_valid(in)) {
        return false;
    }

    nvs_handle_t nvs = 0;
    if (nvs_open(CORE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }

    struct {
        uint32_t magic;
        uint8_t version;
        core_isp_cal_t cal;
    } blob = {
        .magic = CORE_ISP_CAL_MAGIC,
        .version = CORE_ISP_CAL_VERSION,
        .cal = *in,
    };

    esp_err_t err = nvs_set_blob(nvs, "isp_cal", &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Salvataggio isp_cal fallito: %s", esp_err_to_name(err));
        return false;
    }

    s_cal_cache = *in;
    s_cal_loaded = true;
    ESP_LOGI(TAG, "Calibrazione ISP salvata (manual=%d default=%u)",
             in->manual_isp_enabled ? 1 : 0, (unsigned)in->default_profile);
    return true;
}

void core_isp_cal_init(void) {
    if (!s_cal_loaded) {
        core_isp_cal_load(&s_cal_cache);
        s_cal_loaded = true;
    }
}

const core_isp_cal_t *core_isp_cal_get(void) {
    core_isp_cal_init();
    return &s_cal_cache;
}

static int minutes_of_day(uint8_t hour, uint8_t min) {
    return (int)hour * 60 + (int)min;
}

static bool profile_matches_now(const core_isp_profile_t *p, int now_min) {
    int start = minutes_of_day(p->hour_start, p->min_start);
    int end = minutes_of_day(p->hour_end, p->min_end);
    if (start <= end) {
        return now_min >= start && now_min <= end;
    }
    return now_min >= start || now_min <= end;
}

static const core_isp_profile_t *core_isp_default_profile(const core_isp_cal_t *cal) {
    unsigned idx = cal->default_profile;
    if (idx >= CORE_ISP_PROFILE_COUNT) {
        idx = 0;
    }
    return &cal->profiles[idx];
}

const core_isp_profile_t *core_isp_cal_profile_by_index(const core_isp_cal_t *cal, unsigned idx) {
    if (!cal || idx >= CORE_ISP_PROFILE_COUNT) {
        return nullptr;
    }
    return &cal->profiles[idx];
}

const core_isp_profile_t *core_isp_cal_profile_for_now(const core_isp_cal_t *cal) {
    if (!cal) {
        return nullptr;
    }

    if (cal->schedule_enabled && core_time_is_valid()) {
        struct tm t;
        if (core_time_get_local_tm(&t)) {
            int now_min = minutes_of_day((uint8_t)t.tm_hour, (uint8_t)t.tm_min);
            for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
                const core_isp_profile_t *p = &cal->profiles[i];
                if (p->enabled && profile_matches_now(p, now_min)) {
                    return p;
                }
            }
        }
    }

    return core_isp_default_profile(cal);
}

bool core_isp_cal_apply_auto(void) {
    if (core_isp_is_sc3336()) {
        return core_isp_cproc_neutral();
    }
    int fd = core_isp_open_device();
    if (fd < 0) {
        return false;
    }

    const bool imx708 = (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708);
    if (imx708) {
        // Non sovrascrivere le finestre AWB dell'IPA (imx708_default.json):
        // un ioctl custom ha già mandato luma>1023 e spezzato la pipeline.
        esp_video_isp_wb_t wb = {};
        wb.enable = false;
        if (!core_isp_set_ext_ctrl(fd, V4L2_CID_USER_ESP_ISP_WB, &wb, sizeof(wb))) {
            ESP_LOGW(TAG, "Disattivazione WB manuale fallita");
        }
        ESP_LOGI(TAG, "IMX708: AWB/CCM dallo IPA stock (niente override ioctl)");
        return true;
    }

    esp_video_isp_awb_t awb = {};
    awb.enable = true;
    awb.green_min = 70;
    awb.green_max = 220;
    awb.rg_min = 0.30f;
    awb.rg_max = 1.15f;
    awb.bg_min = 0.30f;
    awb.bg_max = 1.15f;
    if (!core_isp_set_ext_ctrl(fd, V4L2_CID_USER_ESP_ISP_AWB, &awb, sizeof(awb))) {
        ESP_LOGW(TAG, "Riattivazione AWB fallita");
        return false;
    }

    esp_video_isp_wb_t wb = {};
    wb.enable = false;
    if (!core_isp_set_ext_ctrl(fd, V4L2_CID_USER_ESP_ISP_WB, &wb, sizeof(wb))) {
        ESP_LOGW(TAG, "Disattivazione WB manuale fallita");
    }

    ESP_LOGI(TAG, "Modalita ISP automatica (AWB IPA attivo)");
    return true;
}

bool core_isp_cal_read_wb(uint16_t *red_x1000, uint16_t *blue_x1000) {
    int fd = core_isp_open_device();
    if (fd < 0) {
        return false;
    }

    esp_video_isp_wb_t wb = {};
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = V4L2_CID_USER_ESP_ISP_WB;
    control.size = sizeof(wb);
    control.p_u8 = (uint8_t *)&wb;
    controls.ctrl_class = V4L2_CID_USER_CLASS;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "Lettura WB corrente fallita");
        return false;
    }

    int red = (int)(wb.red_gain * 1000.0f + 0.5f);
    int blue = (int)(wb.blue_gain * 1000.0f + 0.5f);
    if (red < CORE_ISP_GAIN_MIN_X1000) red = CORE_ISP_GAIN_MIN_X1000;
    if (red > CORE_ISP_GAIN_MAX_X1000) red = CORE_ISP_GAIN_MAX_X1000;
    if (blue < CORE_ISP_GAIN_MIN_X1000) blue = CORE_ISP_GAIN_MIN_X1000;
    if (blue > CORE_ISP_GAIN_MAX_X1000) blue = CORE_ISP_GAIN_MAX_X1000;
    if (red_x1000) *red_x1000 = (uint16_t)red;
    if (blue_x1000) *blue_x1000 = (uint16_t)blue;
    ESP_LOGI(TAG, "WB corrente ISP: R=%d B=%d (enable=%d)", red, blue, wb.enable ? 1 : 0);
    return true;
}

bool core_isp_cal_imx708_tune_ipa(void) {
    if (core_video_sensor_kind() != CORE_VIDEO_SENSOR_IMX708) {
        return true;
    }
#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
    (void)esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE);

    const esp_ipa_config_t *base = esp_ipa_pipeline_get_config("IMX708");
    if (!base || !base->agc) {
        ESP_LOGW(TAG, "IMX708: config IPA non trovata");
        return false;
    }
    static esp_ipa_config_t s_ipa_cfg;
    static esp_ipa_agc_config_t s_agc_cfg;
    s_ipa_cfg = *base;
    s_agc_cfg = *base->agc;
    s_ipa_cfg.agc = &s_agc_cfg;
    s_agc_cfg.luma_target = CORE_IMX708_AE_TARGET;
    s_agc_cfg.luma_low = (CORE_IMX708_AE_TARGET > 50) ? (uint8_t)(CORE_IMX708_AE_TARGET - 50) : 1;
    s_agc_cfg.luma_high = (CORE_IMX708_AE_TARGET < 205) ? (uint8_t)(CORE_IMX708_AE_TARGET + 50) : 255;
    if (esp_video_isp_pipeline_set_ipa_config(&s_ipa_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "IMX708: set_ipa_config AE=%u fallita (pipeline non ancora su?)",
                 (unsigned)CORE_IMX708_AE_TARGET);
        return false;
    }
    ESP_LOGI(TAG, "IMX708: IPA stock + AE target %u (web_stream)",
             (unsigned)CORE_IMX708_AE_TARGET);
    return true;
#else
    ESP_LOGW(TAG, "IMX708: ISP pipeline controller disabilitato");
    return false;
#endif
}

bool core_isp_cal_startup_wb_check(void) {
    if (s_startup_wb_done) {
        return true;
    }
    if (core_isp_is_sc3336()) {
        s_startup_wb_done = true;
        return core_isp_cal_apply_active();
    }
    if (!core_isp_cal_apply_auto()) {
        return false;
    }
    (void)core_isp_cal_imx708_tune_ipa();

    uint16_t r1 = 0, b1 = 0, r2 = 0, b2 = 0;
    if (!core_isp_cal_read_wb(&r1, &b1)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!core_isp_cal_read_wb(&r2, &b2)) {
        return false;
    }

    // V4L2 WB a 1000/1000 con enable=0 è normale: i gain li scrive l'IPA,
    // non questo ioctl. Seminare 1.25/1.35 tingevano di viola.
    const bool v4l2_idle = (r1 == 1000 && b1 == 1000 && r2 == 1000 && b2 == 1000);
    const bool extreme = !v4l2_idle && (r2 < 600 || r2 > 2500 || b2 < 600 || b2 > 2500);
    ESP_LOGI(TAG, "Check bianco avvio: R=%u B=%u (prec R=%u B=%u)%s%s",
             (unsigned)r2, (unsigned)b2, (unsigned)r1, (unsigned)b1,
             v4l2_idle ? " — IPA AWB (V4L2 idle)" : "",
             extreme ? " — gain estremi, lascio AWB" : "");

    s_startup_wb_done = true;
    return true;
}

bool core_isp_cal_apply_focus(uint16_t focus_pos) {
    if (!core_video_module_is_af()) {
        return true;
    }
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708 &&
        focus_pos < CORE_IMX708_FOCUS_MIN) {
        // DW9807: 0–447 è battuta (immagine sempre morbida). Default IPA ~2 m.
        focus_pos = CORE_IMX708_FOCUS_DEFAULT;
    }
    if (!core_video_is_ready() && !core_video_init()) {
        return false;
    }
    return core_video_focus_set(focus_pos);
}

bool core_isp_cal_save_focus_all(uint16_t focus_pos) {
    if (focus_pos > CORE_ISP_FOCUS_MAX) {
        focus_pos = CORE_ISP_FOCUS_MAX;
    }
    core_isp_cal_init();
    core_isp_cal_t cal = s_cal_cache;
    for (unsigned i = 0; i < CORE_ISP_PROFILE_COUNT; ++i) {
        cal.profiles[i].focus_pos = focus_pos;
    }
    if (!core_isp_cal_save(&cal)) {
        return false;
    }
    (void)core_isp_cal_apply_focus(focus_pos);
    ESP_LOGI(TAG, "Fuoco VCM salvato su tutti i profili: %u", (unsigned)focus_pos);
    return true;
}

bool core_isp_cal_apply_profile(const core_isp_profile_t *profile) {
    if (!profile) {
        return false;
    }
    if (core_isp_is_sc3336()) {
        return core_isp_cproc_from_profile(profile);
    }

    // Focus anche con AWB automatico: è indipendente dal bilanciamento colore.
    (void)core_isp_cal_apply_focus(profile->focus_pos);

    int fd = core_isp_open_device();
    if (fd < 0) {
        return false;
    }

    bool ok = true;

    if (profile->manual_wb) {
        esp_video_isp_awb_t awb = {};
        awb.enable = false;
        if (!core_isp_set_ext_ctrl(fd, V4L2_CID_USER_ESP_ISP_AWB, &awb, sizeof(awb))) {
            ESP_LOGW(TAG, "Disabilitazione AWB fallita");
            ok = false;
        }

        esp_video_isp_wb_t wb = {};
        wb.enable = true;
        wb.red_gain = (float)profile->red_gain_x1000 / 1000.0f;
        wb.blue_gain = (float)profile->blue_gain_x1000 / 1000.0f;
        if (!core_isp_set_ext_ctrl(fd, V4L2_CID_USER_ESP_ISP_WB, &wb, sizeof(wb))) {
            ESP_LOGW(TAG, "WB manuale fallito");
            ok = false;
        }

        struct v4l2_ext_control control = {};
        struct v4l2_ext_controls controls = {};
        controls.ctrl_class = V4L2_CTRL_CLASS_USER;
        controls.count = 1;
        controls.controls = &control;

        control.id = V4L2_CID_SATURATION;
        control.value = profile->saturation;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ok = false;
        }
        control.id = V4L2_CID_CONTRAST;
        control.value = profile->contrast;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ok = false;
        }
        // UI 0–255 (128 = neutro) → driver ISP −128…+127 (0 = neutro).
        // Senza offset il default 128 è fuori range e il driver rifiuta
        // l'intera config colore (anche saturazione e contrasto).
        control.id = V4L2_CID_BRIGHTNESS;
        control.value = (int32_t)profile->brightness - 128;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ok = false;
        }
        control.id = V4L2_CID_HUE;
        control.value = profile->hue;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ok = false;
        }
    }

    ESP_LOGI(TAG, "Profilo \"%s\" manuale (R=%u B=%u sat=%d hue=%u focus=%u)",
             profile->name, (unsigned)profile->red_gain_x1000, (unsigned)profile->blue_gain_x1000,
             (int)profile->saturation, (unsigned)profile->hue, (unsigned)profile->focus_pos);
    return ok;
}

bool core_isp_cal_apply_active(void) {
    const core_isp_cal_t *cal = core_isp_cal_get();
    if (core_isp_is_sc3336()) {
        if (!cal->manual_isp_enabled) {
            return core_isp_cproc_neutral();
        }
        return core_isp_cproc_from_profile(&cal->profiles[0]);
    }
    const core_isp_profile_t *p = core_isp_cal_profile_for_now(cal);
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708) {
        // WB/sat dei profili sono tarati OV5647; di notte spegnevano AWB → magenta.
        uint16_t focus = CORE_IMX708_FOCUS_DEFAULT;
        if (p && p->focus_pos >= CORE_IMX708_FOCUS_MIN) {
            focus = p->focus_pos;
        }
        (void)core_isp_cal_apply_focus(focus);
        (void)core_isp_cal_apply_auto();
        (void)core_isp_cal_imx708_tune_ipa();
        return true;
    }
    if (!cal->manual_isp_enabled) {
        if (p) {
            (void)core_isp_cal_apply_focus(p->focus_pos);
        }
        return core_isp_cal_apply_auto();
    }

    if (!p || !p->manual_wb) {
        if (p) {
            (void)core_isp_cal_apply_focus(p->focus_pos);
        }
        ESP_LOGI(TAG, "Profilo \"%s\" automatico AWB (focus=%u)", p ? p->name : "?",
                 p ? (unsigned)p->focus_pos : 0);
        return core_isp_cal_apply_auto();
    }
    return core_isp_cal_apply_profile(p);
}

bool core_isp_cal_apply_preview(const core_isp_profile_t *profile, bool manual_isp_enabled,
                                bool focus_only) {
    if (!profile) {
        return false;
    }
    if (core_isp_is_sc3336()) {
        if (!manual_isp_enabled) {
            return core_isp_cproc_neutral();
        }
        return core_isp_cproc_from_profile(profile);
    }
    // Slider fuoco: solo VCM. Riapplicare AWB/ISP a ogni tick satura Hosted+CPU → GUI lenta.
    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708) {
        uint16_t focus = profile->focus_pos;
        if (focus < CORE_IMX708_FOCUS_MIN) {
            focus = CORE_IMX708_FOCUS_DEFAULT;
        }
        if (focus_only) {
            return core_isp_cal_apply_focus(focus);
        }
        (void)core_isp_cal_apply_focus(focus);
        return core_isp_cal_apply_auto();
    }
    if (focus_only) {
        return core_isp_cal_apply_focus(profile->focus_pos);
    }
    if (!manual_isp_enabled || !profile->manual_wb) {
        (void)core_isp_cal_apply_focus(profile->focus_pos);
        return core_isp_cal_apply_auto();
    }
    return core_isp_cal_apply_profile(profile);
}