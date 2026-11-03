#include "CoreVideo.h"
#include "CoreConfig.h"
#include "CoreIspCal.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_cam_motor_types.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "hal_system.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#ifndef V4L2_CID_FOCUS_ABSOLUTE
#define V4L2_CID_FOCUS_ABSOLUTE 0x009a090a
#endif

static const char *TAG = "core_video";

void core_snap(const char *msg) {
#if !defined(_WIN32)
    FILE *f = fopen("/tmp/mrbin-snap.log", "a");
    if (!f) {
        return;
    }
    fprintf(f, "%lld %s\n", (long long)(esp_timer_get_time() / 1000), msg ? msg : "");
    fclose(f);
#else
    (void)msg;
#endif
}

// True solo se DW9714 è davvero agganciato (FOCUS + motor format).
static bool core_video_probe_module_af(int fd) {
    if (fd < 0) {
        return false;
    }
#if !CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
    return false;
#else
    struct v4l2_query_ext_ctrl qctrl;
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) != 0) {
        return false;
    }
    esp_cam_motor_format_t motor_fmt;
    memset(&motor_fmt, 0, sizeof(motor_fmt));
    if (ioctl(fd, VIDIOC_G_MOTOR_FMT, &motor_fmt) != 0) {
        return false;
    }
    // Range focus tipico DW9714; evita falsi positivi su stub/dummy.
    if (qctrl.maximum <= qctrl.minimum || qctrl.maximum < 64) {
        ESP_LOGW(TAG, "FOCUS_ABSOLUTE range sospetto min=%lld max=%lld — treato come fisso",
                 (long long)qctrl.minimum, (long long)qctrl.maximum);
        return false;
    }
    return true;
#endif
}

static bool s_init = false;
static int s_video_fd = -1;
static SemaphoreHandle_t s_init_mtx = nullptr;
static bool s_vflip = false;
static bool s_hflip = false;
static bool s_flip_hw_ok = false;

#ifndef VIDIOC_S_CTRL
#define VIDIOC_S_CTRL 0xC008561C
#endif
#ifndef VIDIOC_G_CTRL
#define VIDIOC_G_CTRL 0xC008561B
#endif

struct core_v4l2_ctrl {
    uint32_t id;
    int32_t value;
};

static bool core_video_name_has_sc3336(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    for (const char *p = name; p[0] && p[1] && p[2] && p[3] && p[4] && p[5]; ++p) {
        char a = p[0] | 0x20, b = p[1] | 0x20, c = p[2], d = p[3], e = p[4], f = p[5];
        if (a == 's' && b == 'c' && c == '3' && d == '3' && e == '3' && f == '6') {
            return true;
        }
    }
    return false;
}

static bool core_video_s_ctrl(int fd, uint32_t id, int32_t value) {
    struct core_v4l2_ctrl c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &c) == 0) {
        return true;
    }
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = id;
    control.value = value;
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = &control;
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0;
}

/* SC3336 flip lives on the sensor subdev (m00_b_sc3336), not rkcif /dev/video0. */
static int core_video_open_sensor_flip_fd(char *name_out, size_t name_len) {
#if defined(_WIN32)
    (void)name_out;
    (void)name_len;
    return -1;
#else
    int preferred = -1;
    int fallback = -1;
    char pref_name[80] = {0};
    char fb_name[80] = {0};
    for (int i = 0; i < 16; ++i) {
        char path[40];
        snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        struct core_v4l2_ctrl c;
        memset(&c, 0, sizeof(c));
        c.id = V4L2_CID_HFLIP;
        if (ioctl(fd, VIDIOC_G_CTRL, &c) != 0) {
            close(fd);
            continue;
        }
        char sys[80];
        char nm[64] = {0};
        snprintf(sys, sizeof(sys), "/sys/class/video4linux/v4l-subdev%d/name", i);
        FILE *f = fopen(sys, "r");
        if (f) {
            if (fgets(nm, sizeof(nm), f)) {
                size_t n = strlen(nm);
                while (n && (nm[n - 1] == '\n' || nm[n - 1] == '\r')) {
                    nm[--n] = 0;
                }
            }
            fclose(f);
        }
        if (core_video_name_has_sc3336(nm) ||
            (nm[0] && strstr(nm, "imx415")) || (nm[0] && strstr(nm, "IMX415"))) {
            if (preferred >= 0) {
                close(preferred);
            }
            preferred = fd;
            snprintf(pref_name, sizeof(pref_name), "%s (%s)", path, nm[0] ? nm : "sc3336");
        } else if (fallback < 0) {
            fallback = fd;
            snprintf(fb_name, sizeof(fb_name), "%s (%s)", path, nm[0] ? nm : "subdev");
        } else {
            close(fd);
        }
    }
    if (preferred >= 0) {
        if (fallback >= 0) {
            close(fallback);
        }
        if (name_out && name_len) {
            snprintf(name_out, name_len, "%s", pref_name);
        }
        return preferred;
    }
    if (fallback >= 0) {
        if (name_out && name_len) {
            snprintf(name_out, name_len, "%s", fb_name);
        }
        return fallback;
    }
    return -1;
#endif
}

static void core_video_close_device(void) {
    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }
}

static bool core_video_open_device(void) {
    if (s_video_fd >= 0) {
        return true;
    }
    int fd = hal_open_timeout(CORE_VIDEO_DEV_NAME, O_RDWR | O_NONBLOCK,
                              CORE_VIDEO_OPEN_TIMEOUT_MS);
    if (fd < 0) {
        ESP_LOGE(TAG, "open(%s) fallito (timeout %d ms o errno=%d)",
                 CORE_VIDEO_DEV_NAME, CORE_VIDEO_OPEN_TIMEOUT_MS, errno);
        return false;
    }
    s_video_fd = fd;
    return true;
}

int core_video_device_fd(void) {
    return s_video_fd;
}

// SC3336: V4L2_CID_HFLIP/VFLIP on the sensor subdev. video0 (rkcif) has no flip.
static void core_video_apply_flip(void) {
    s_flip_hw_ok = false;
    char devname[80] = {0};
    int fd = core_video_open_sensor_flip_fd(devname, sizeof(devname));
    bool own = (fd >= 0);
    if (fd < 0) {
        fd = s_video_fd;
        own = false;
        snprintf(devname, sizeof(devname), "%s", CORE_VIDEO_DEV_NAME);
    }
    if (fd < 0) {
        ESP_LOGW(TAG, "Flip: nessun subdev SC3336 / %s", CORE_VIDEO_DEV_NAME);
        return;
    }
    const bool ok_v = core_video_s_ctrl(fd, V4L2_CID_VFLIP, s_vflip ? 1 : 0);
    const bool ok_h = core_video_s_ctrl(fd, V4L2_CID_HFLIP, s_hflip ? 1 : 0);
    s_flip_hw_ok = ok_v && ok_h;
    if (s_flip_hw_ok) {
        ESP_LOGI(TAG, "Orientamento %s: capovolgi=%d specchia=%d",
                 devname, (int)s_vflip, (int)s_hflip);
    } else {
        ESP_LOGW(TAG, "Flip su %s fallito v=%d h=%d errno=%d",
                 devname, (int)ok_v, (int)ok_h, errno);
    }
    if (own) {
        close(fd);
    }
}

void core_video_set_flip(bool vflip, bool hflip) {
    s_vflip = vflip;
    s_hflip = hflip;
    if (s_init) {
        if (!core_video_sync_bayer_order(s_vflip, s_hflip)) {
            core_video_apply_flip();
        }
        return;
    }
    core_video_apply_flip();
}

bool core_video_get_vflip(void) {
    return s_vflip;
}

bool core_video_get_hflip(void) {
    return s_hflip;
}

bool core_video_flip_hw_applied(void) {
    return s_flip_hw_ok;
}

#if !defined(_WIN32)
/* /usr is the read-only root and its BusyBox has no pidof, so the stock
 * script spawned a new rkaiq on every call until OOM killed the moov. */
static const char *core_video_isp_bin(void) {
    if (access("/oem/usr/bin/mrbin-isp", X_OK) == 0) {
        return "/oem/usr/bin/mrbin-isp";
    }
    if (access("/usr/sbin/mrbin-isp", X_OK) == 0) {
        return "/usr/sbin/mrbin-isp";
    }
    return nullptr;
}

static void core_video_isp_cmd(const char *args, const char *log_path, int timeout_sec) {
    const char *isp = core_video_isp_bin();
    if (!args || !isp) {
        return;
    }
    char cmd[320];
    /* Prefer mrbin-timeout: /usr/bin/timeout on device is often a CRLF copy that fails. */
    const char *to = (access("/usr/sbin/mrbin-timeout", X_OK) == 0) ? "/usr/sbin/mrbin-timeout"
                     : (access("/bin/timeout", X_OK) == 0) ? "/bin/timeout"
                     : (access("/usr/bin/timeout", X_OK) == 0) ? "/usr/bin/timeout"
                     : nullptr;
    if (to) {
        snprintf(cmd, sizeof(cmd), "%s %d %s %s >>%s 2>&1",
                 to, timeout_sec > 0 ? timeout_sec : 4, isp, args,
                 log_path ? log_path : "/tmp/mrbin-isp.log");
    } else {
        snprintf(cmd, sizeof(cmd), "%s %s >>%s 2>&1",
                 isp, args, log_path ? log_path : "/tmp/mrbin-isp.log");
    }
    (void)system(cmd);
}
#endif

void core_video_reapply_isp(void) {
#if !defined(_WIN32)
    core_video_isp_cmd("apply", "/tmp/mrbin-isp-apply.log", 4);
#endif
}

void core_video_isp_rec_begin(void) {
#if !defined(_WIN32)
    core_video_isp_cmd("rec-begin", "/tmp/mrbin-isp-rec.log", 3);
#endif
}

void core_video_isp_rec_after_vi(void) {
#if !defined(_WIN32)
    /* 3A after VI configure / before EnableChn. The script waits until
     * rkaiq is listening, which can outlast the old 8 s cap on a cold boot. */
    core_video_isp_cmd("rec-3a", "/tmp/mrbin-isp-rec.log", 12);
#endif
}

void core_video_isp_rec_end(void) {
#if !defined(_WIN32)
    core_video_isp_cmd("rec-end", "/tmp/mrbin-isp-rec.log", 3);
#endif
}

void core_video_isp_pir_arm(void) {
#if !defined(_WIN32)
    /* Direct call. mrbin-timeout wakes only once a second and would
     * add a full second in front of EnableChn. The script caps itself. */
    const char *isp = core_video_isp_bin();
    if (isp) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "%s pir-arm >>/tmp/mrbin-isp-pir.log 2>&1", isp);
        (void)system(cmd);
    }
#endif
}

void core_video_isp_pir_lock(void) {
#if !defined(_WIN32)
    const char *isp = core_video_isp_bin();
    if (isp) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "%s pir-lock >>/tmp/mrbin-isp-pir.log 2>&1", isp);
        (void)system(cmd);
    }
#endif
}

void core_video_isp_demo_motion_stabilize(void) {
#if !defined(_WIN32)
    /* Obiettivo coperto: 3A alza gain e AWB oscilla (verde/rosso in live).
     * Per il motion DEMO usiamo solo luma Y; qui blocchiamo AE scura tipo PIR. */
    core_video_isp_pir_lock();
#endif
}

void core_video_isp_live_pre(void) {
#if !defined(_WIN32)
    /* Before EnableChn. One niced 3A, fds closed, no ae-follow.
     * 12 s covers the cold-boot wait until rkaiq is listening. */
    core_video_isp_cmd("live-pre", "/tmp/mrbin-isp-live.log", 12);
#endif
}

void core_video_isp_live_after_vi(void) {
#if !defined(_WIN32)
    /* 3A is already running from live-pre. Settle, then stop. Do not
     * restart it — a late 3A zeros chroma (grayscale JPEG). */
    core_video_isp_cmd("live-3a", "/tmp/mrbin-isp-live.log", 8);
#endif
}

void core_video_isp_stop_3a(void) {
#if !defined(_WIN32)
    /* Direct, not mrbin-timeout. That helper only notices the child once
     * a second, so a 0.4 s stop-3a still blocked ~1 s and the first
     * Record click gave up before VI was idle. The script itself is bounded. */
    const char *isp = core_video_isp_bin();
    if (isp) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "%s stop-3a >>/tmp/mrbin-isp-live.log 2>&1", isp);
        (void)system(cmd);
    }
#endif
}

void core_video_reapply_flip(void) {
    /* Flip only. Full mrbin-isp apply here restarted 3A on every VI start and
     * raced the rec VENC (watch already applies once on stream-up). */
    core_video_apply_flip();
}

#define CORE_VIDEO_INIT_RETRY_COUNT  5
#define CORE_VIDEO_INIT_RETRY_MS     500
#define CORE_VIDEO_INIT_FLAGS        (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP | ESP_VIDEO_INIT_FLAGS_MOTOR)

static bool core_video_probe_device(void) {
    return s_video_fd >= 0;
}

static bool core_video_init_once(void);

static void core_video_log_heap(const char *label) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t blk_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s: int=%u KB dma=%u KB blk_dma=%u KB",
             label, (unsigned)(free_int / 1024), (unsigned)(free_dma / 1024),
             (unsigned)(blk_dma / 1024));
}

bool core_video_is_ready(void) {
    return s_init && core_video_probe_device();
}

// Diagnostica: scansiona il bus SCCB (GPIO7/8) e logga chi risponde.
// Nessun ACK = problema elettrico (cavo/alimentazione/connettore).
// 0x36 = OV5647, 0x1A = IMX708, 0x0C = VCM, 0x30 = SC3336 (Luckfox CSI).
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void core_video_i2c_scan(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)CORE_CSI_I2C_SDA,
        .scl_io_num = (gpio_num_t)CORE_CSI_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "Scan I2C: bus non disponibile (occupato da SCCB?)");
        return;
    }
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            found++;
            ESP_LOGW(TAG, "Scan I2C GPIO%d/%d: dispositivo trovato a 0x%02X%s",
                     CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL, addr,
                     addr == 0x36 ? " (OV5647)" :
                     addr == 0x1A ? " (IMX708)" :
                     addr == 0x0C ? " (VCM DW9714/DW9807)" :
                     addr == 0x30 ? " (SC3336)" : "");
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "Scan I2C GPIO%d/%d: NESSUN dispositivo risponde — "
                 "camera senza alimentazione o flat non a contatto",
                 CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL);
    }
    i2c_del_master_bus(bus);
}

bool core_video_init(void) {
    if (!s_init_mtx) {
        s_init_mtx = xSemaphoreCreateMutex();
    }
    if (s_init_mtx && xSemaphoreTake(s_init_mtx, pdMS_TO_TICKS(CORE_VIDEO_OPEN_TIMEOUT_MS + 200)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex init camera");
        return core_video_is_ready();
    }

    bool ok = false;
    const int max_try = core_rec_fast_requested() ? 1 : CORE_VIDEO_INIT_RETRY_COUNT;
    if (s_init && s_video_fd < 0 && !core_video_open_device()) {
        ESP_LOGW(TAG, "FD video perso — reinizializzo sottosistema");
        core_video_deinit();
    }
    if (s_init && core_video_probe_device()) {
        ok = true;
        goto out;
    }
    if (s_init) {
        ESP_LOGW(TAG, "Camera non risponde — reinizializzo sottosistema video");
        core_video_close_device();
        core_video_deinit();
    }

    core_video_log_heap("Pre-init camera");
    for (int attempt = 1; attempt <= max_try; ++attempt) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retry init camera (%d/%d)...", attempt, max_try);
            core_isp_cal_close_device();
            esp_video_deinit();
            vTaskDelay(pdMS_TO_TICKS(CORE_VIDEO_INIT_RETRY_MS));
        }
        if (core_video_init_once()) {
            ok = true;
            break;
        }
        core_video_close_device();
        core_isp_cal_close_device();
        esp_video_deinit();
    }
    if (!ok) {
        core_video_log_heap("Init camera fallita");
        /* Full I2C scan can stall a stuck SCCB bus for seconds — skip on HTTP. */
        ESP_LOGW(TAG, "Init camera fallita — skip scan I2C (max %d ms)",
                 CORE_VIDEO_OPEN_TIMEOUT_MS);
    }

out:
    if (s_init_mtx) {
        xSemaphoreGive(s_init_mtx);
    }
    return ok;
}

static bool core_video_init_once(void) {
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = I2C_NUM_0,
                .scl_pin = (gpio_num_t)CORE_CSI_I2C_SCL,
                .sda_pin = (gpio_num_t)CORE_CSI_I2C_SDA,
            },
            .freq = 100000,
        },
        .reset_pin = (gpio_num_t)-1,
        .pwdn_pin = (gpio_num_t)-1,
        .dont_init_ldo = false,
    };

#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
    // VCM sullo stesso SCCB del sensore: DW9714 (OV5647-AF) o DW9807 (IMX708).
    // Senza cam_motor, ISP AF asserta (VIDIOC_G_MOTOR_FMT / focus_info=NULL).
    esp_video_init_cam_motor_config_t motor_cfg = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = I2C_NUM_0,
                .scl_pin = (gpio_num_t)CORE_CSI_I2C_SCL,
                .sda_pin = (gpio_num_t)CORE_CSI_I2C_SDA,
            },
            .freq = 100000,
        },
        .reset_pin = (gpio_num_t)-1,
        .pwdn_pin = (gpio_num_t)-1,
        .signal_pin = (gpio_num_t)-1,
    };
#endif

    esp_video_init_config_t video_cfg = {
        .csi = &csi_cfg,
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
        .cam_motor = &motor_cfg,
#endif
    };

    esp_err_t err = esp_video_init_with_flags(&video_cfg, CORE_VIDEO_INIT_FLAGS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init fallita: %s", esp_err_to_name(err));
        core_isp_cal_close_device();
        esp_video_deinit();
        return false;
    }

    if (!core_video_open_device()) {
        ESP_LOGE(TAG, "Camera non rilevata su %s (MIPI-CSI / I2C GPIO%d,%d)",
                 CORE_VIDEO_DEV_NAME, CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL);
        core_isp_cal_close_device();
        esp_video_deinit();
        return false;
    }

    s_init = true;

    core_video_detect_sensor();
    // Boot: VCM presente → AF; assente → ottica fissa (Bayer dipende dal chip).
    {
        const bool is_af = core_video_probe_module_af(s_video_fd);
        core_video_set_module_af(is_af);
        ESP_LOGI(TAG, "Probe modulo al boot: %s", core_video_sensor_label());
    }

    if (!core_video_sync_bayer_order(s_vflip, s_hflip)) {
        ESP_LOGW(TAG, "Init: sync Bayer fallita — possibili colori errati");
    }
    core_video_reapply_flip();
    (void)core_isp_cal_apply_active();
    ESP_LOGI(TAG, "Sottosistema video inizializzato (%s, %s)", CORE_VIDEO_DEV_NAME,
             core_video_sensor_label());
    return true;
#else
    ESP_LOGE(TAG, "MIPI-CSI non abilitato in sdkconfig");
    return false;
#endif
}

void core_video_deinit(void) {
    // Idempotente: il preload foto chiama web_release_video_for_io per ogni JPEG.
    if (!s_init && s_video_fd < 0) {
        core_isp_cal_close_device();
        return;
    }
    core_video_close_device();
    core_isp_cal_close_device();  // /dev/video20 — senza questo esp_video_deinit fallisce
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_video_deinit: %s", esp_err_to_name(err));
    }
    s_init = false;
    // Prossimo init deve riprobare AF (cambio modulo a caldo / reboot soft).
    core_video_reset_module_probe();
}

bool core_video_focus_set(uint16_t pos) {
    if (!core_video_is_ready() || !core_video_module_is_af() || s_video_fd < 0) {
        return false;
    }
    if (pos > 1023) {
        pos = 1023;
    }
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = V4L2_CID_FOCUS_ABSOLUTE;
    control.value = (int32_t)pos;
    controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(s_video_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "FOCUS_ABSOLUTE set %u fallito", (unsigned)pos);
        return false;
    }
        ESP_LOGD(TAG, "Focus VCM = %u", (unsigned)pos);
    return true;
}

bool core_video_focus_get(uint16_t *out_pos) {
    if (!out_pos || !core_video_is_ready() || !core_video_module_is_af() || s_video_fd < 0) {
        return false;
    }
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = V4L2_CID_FOCUS_ABSOLUTE;
    controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(s_video_fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        return false;
    }
    int v = control.value;
    if (v < 0) {
        v = 0;
    }
    if (v > 1023) {
        v = 1023;
    }
    *out_pos = (uint16_t)v;
    return true;
}

bool core_video_focus_get_range(uint16_t *out_min, uint16_t *out_max) {
    if (!core_video_is_ready() || !core_video_module_is_af() || s_video_fd < 0) {
        return false;
    }
    struct v4l2_query_ext_ctrl qctrl = {};
    qctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    if (ioctl(s_video_fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) != 0) {
        return false;
    }
    if (out_min) {
        *out_min = (uint16_t)(qctrl.minimum < 0 ? 0 : qctrl.minimum);
    }
    if (out_max) {
        int64_t mx = qctrl.maximum;
        if (mx < 0) {
            mx = 0;
        }
        if (mx > 1023) {
            mx = 1023;
        }
        *out_max = (uint16_t)mx;
    }
    return true;
}

bool core_video_prepare_for_stream(const core_settings_t *s) {
    (void)s;
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!core_video_init()) {
        return false;
    }
    // Live: sempre 960 binning (+ Bayer). NON usare prepare_for_settings/1080p qui:
    // MJPEG FullHD satura WiFi/CPU → un frame e poi blocco GUI.
    if (!core_video_sync_bayer_order(s_vflip, s_hflip)) {
        ESP_LOGW(TAG, "prepare_for_stream: sync Bayer fallita");
    }
    (void)core_isp_cal_apply_active();
    return true;
}
