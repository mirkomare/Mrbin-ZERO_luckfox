#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_VIDEO_ISP1_DEVICE_NAME "/dev/video20"
#define ESP_VIDEO_INIT_FLAGS_MIPI_CSI 1
#define ESP_VIDEO_INIT_FLAGS_ISP 2
#define ESP_VIDEO_INIT_FLAGS_MOTOR 4
#define SENSOR_ISP_INFO_VERSION_DEFAULT 1
#define ESP_CAM_SENSOR_PIXFORMAT_RAW10 1
#define ESP_CAM_SENSOR_MIPI_CSI 1
#define VIDIOC_S_SENSOR_FMT 0x5A01
#define VIDIOC_G_SENSOR_FMT 0x5A02
#define VIDIOC_G_MOTOR_FMT  0x5A03
#define V4L2_CID_USER_ESP_ISP_WB  0x00A00001
#define V4L2_CID_USER_ESP_ISP_AWB 0x00A00002
#define ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE 0

typedef enum {
    ESP_CAM_SENSOR_BAYER_RGGB = 0,
    ESP_CAM_SENSOR_BAYER_GRBG,
    ESP_CAM_SENSOR_BAYER_GBRG,
    ESP_CAM_SENSOR_BAYER_BGGR,
} esp_cam_sensor_bayer_pattern_t;

typedef struct {
    uint32_t version;
    uint32_t pclk;
    uint16_t hts;
    uint16_t vts;
    esp_cam_sensor_bayer_pattern_t bayer_type;
} esp_cam_sensor_isp_v1_info_t;

typedef struct {
    esp_cam_sensor_isp_v1_info_t isp_v1_info;
} esp_cam_sensor_isp_info_t;

typedef struct {
    uint32_t mipi_clk;
    uint8_t lane_num;
    bool line_sync_en;
} esp_cam_sensor_mipi_info_t;

typedef struct {
    const char *name;
    int format;
    int port;
    uint32_t xclk;
    uint16_t width;
    uint16_t height;
    void *regs;
    int regs_size;
    uint8_t fps;
    esp_cam_sensor_isp_info_t *isp_info;
    esp_cam_sensor_mipi_info_t mipi_info;
} esp_cam_sensor_format_t;

typedef struct {
    int dummy;
} esp_cam_motor_format_t;

typedef struct {
    bool init_sccb;
    struct {
        int port;
        int scl_pin;
        int sda_pin;
    } i2c_config;
    uint32_t freq;
} esp_video_init_sccb_config_t;

typedef struct {
    esp_video_init_sccb_config_t sccb_config;
    int reset_pin;
    int pwdn_pin;
    bool dont_init_ldo;
} esp_video_init_csi_config_t;

typedef struct {
    esp_video_init_sccb_config_t sccb_config;
    int reset_pin;
    int pwdn_pin;
    int signal_pin;
} esp_video_init_cam_motor_config_t;

typedef struct {
    const esp_video_init_csi_config_t *csi;
    const esp_video_init_cam_motor_config_t *cam_motor;
} esp_video_init_config_t;

typedef struct {
    bool enable;
    float red_gain;
    float blue_gain;
} esp_video_isp_wb_t;

typedef struct {
    bool enable;
    uint8_t green_min, green_max;
    float rg_min, rg_max, bg_min, bg_max;
} esp_video_isp_awb_t;

typedef struct {
    void *agc;
} esp_ipa_config_t;

typedef struct {
    uint8_t luma_target;
    uint8_t luma_low;
    uint8_t luma_high;
} esp_ipa_agc_config_t;

esp_err_t esp_video_init_with_flags(const esp_video_init_config_t *cfg, int flags);
esp_err_t esp_video_deinit(void);
esp_err_t esp_video_isp_pipeline_set_agc_status(int st);
esp_err_t esp_video_isp_pipeline_set_ipa_config(const esp_ipa_config_t *cfg);
const esp_ipa_config_t *esp_ipa_pipeline_get_config(const char *name);
const esp_cam_sensor_format_t *imx708_format_by_index(size_t index);

#ifdef __cplusplus
}
#endif
