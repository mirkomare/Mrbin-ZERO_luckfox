#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CAPTURE_ERR_OK 0
#define ESP_CAPTURE_SYNC_MODE_NONE 0
#define ESP_CAPTURE_FMT_ID_H264  1
#define ESP_CAPTURE_FMT_ID_MJPEG 2
#define ESP_CAPTURE_STREAM_TYPE_VIDEO 1
#define ESP_CAPTURE_RUN_MODE_ALWAYS 1
#define ESP_CAPTURE_EVENT_VIDEO_PIPELINE_BUILT 1
#define ESP_GMF_ERR_OK 0
#define COLOR_RGB565_BLACK  0
#define COLOR_RGB565_YELLOW 0xFFE0

typedef int esp_capture_err_t;
typedef int esp_capture_event_t;
typedef void *esp_capture_handle_t;
typedef void *esp_capture_sink_handle_t;
typedef void *esp_gmf_element_handle_t;

typedef struct {
    int dummy;
} esp_capture_video_src_if_t;

typedef struct esp_capture_overlay_if {
    int (*open)(struct esp_capture_overlay_if *self);
    void (*close)(struct esp_capture_overlay_if *self);
    void (*set_alpha)(struct esp_capture_overlay_if *self, uint8_t a);
} esp_capture_overlay_if_t;

typedef struct {
    int x, y, width, height;
} esp_capture_rgn_t;

typedef struct {
    int format_id;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
} esp_capture_video_info_t;

typedef struct {
    int sync_mode;
    void *audio_src;
    esp_capture_video_src_if_t *video_src;
    bool share_overlay;
} esp_capture_cfg_t;

typedef struct {
    esp_capture_video_info_t video_info;
} esp_capture_sink_cfg_t;

typedef struct {
    int stream_type;
    uint8_t *data;
    int size;
    uint32_t pts;
} esp_capture_stream_frame_t;

typedef struct {
    char dev_name[64];
    int buf_count;
} esp_capture_video_v4l2_src_cfg_t;

typedef struct {
    int color;
    int font;
    int font_size;
    int x, y;
} esp_capture_text_overlay_draw_info_t;

typedef struct {
    uint16_t x, y, width, height;
} esp_gmf_video_rgn_t;

typedef esp_capture_err_t (*esp_capture_event_cb_t)(esp_capture_event_t event, void *ctx);

void esp_video_enc_register_default(void);
void esp_video_enc_unregister_default(void);

esp_capture_video_src_if_t *esp_capture_new_video_v4l2_src(const esp_capture_video_v4l2_src_cfg_t *cfg);
esp_capture_err_t esp_capture_open(const esp_capture_cfg_t *cfg, esp_capture_handle_t *out);
esp_capture_err_t esp_capture_close(esp_capture_handle_t h);
esp_capture_err_t esp_capture_start(esp_capture_handle_t h);
esp_capture_err_t esp_capture_stop(esp_capture_handle_t h);
/* True while a VI+VENC start is in flight or the sensor is still owned. */
int esp_capture_mpi_busy(void);
/* Drop a leftover VI/VENC. No-op if a start task is inside EnableChn
 * (that task stops itself). Does not add a settle delay. */
void esp_capture_release_leftover(void);
/* Last RK_MPI_* failure from the in-flight start, e.g. "RK_MPI_VENC_CreateChn ret=0x...".
 * Empty if start succeeded or no MPI call failed. */
const char *esp_capture_last_mpi_error(void);
/* Live MJPEG: VI stays at the record rate; the JPEG encoder outputs this fps. */
void esp_capture_set_preview_fps(int fps);
/* 1 when VI is already streaming MJPEG at this picture size and pipe rate. */
int esp_capture_vi_matches(int w, int h, int keep_fps);
/* The open RKMPI pipeline, or NULL when VI is down. */
esp_capture_handle_t esp_capture_active_handle(void);
/* Replace MJPEG VENC with H.264 on the VI that is already streaming.
 * Does not DisableChn, SYS_Exit, or touch 3A. 0 on success. */
int esp_capture_retarget_h264(int w, int h, int keep_fps, uint32_t bitrate, uint32_t gop);
/* MJPEG on rkisp_selfpath (VI/VENC channel 1). Independent of the H.264 main pipe.
 * vi_user_depth: 0 = MJPEG only (Live). >0 = VI depth for GetChnFrame luma (DEMO motion, no VENC). */
int hal_preview_start(int w, int h, int fps, int vi_user_depth);
/* Dopo EnableChn / pir-lock: i flag GUI vanno sul canale VI, non solo sul subdev. */
void hal_preview_reapply_flip(void);
void hal_vi_apply_flip(int chn);
int hal_preview_acquire(uint8_t **data, int *len, int wait_ms);
int hal_preview_acquire_luma(uint8_t **y, int *w, int *h, int *stride, int wait_ms);
void hal_preview_stop(void);
void hal_preview_stop_force(void);
int hal_preview_running(void);
int hal_main_running(void);
void hal_mark_3a_down(void);
void hal_capture_request_idr(void);
/* Hand the next acquire back this frame (the IDR prime already pulled). */
void esp_capture_replay_next(const uint8_t *data, int len, uint32_t pts);
esp_capture_err_t esp_capture_set_event_cb(esp_capture_handle_t h, esp_capture_event_cb_t cb, void *ctx);

esp_capture_err_t esp_capture_sink_setup(esp_capture_handle_t h, int idx,
                                         const esp_capture_sink_cfg_t *cfg,
                                         esp_capture_sink_handle_t *out);
esp_capture_err_t esp_capture_sink_enable(esp_capture_sink_handle_t s, int mode);
esp_capture_err_t esp_capture_sink_set_bitrate(esp_capture_sink_handle_t s, int type, uint32_t bps);
esp_capture_err_t esp_capture_sink_acquire_frame(esp_capture_sink_handle_t s,
                                                 esp_capture_stream_frame_t *f, bool no_wait);
esp_capture_err_t esp_capture_sink_release_frame(esp_capture_sink_handle_t s,
                                                 esp_capture_stream_frame_t *f);
esp_capture_err_t esp_capture_sink_add_overlay(esp_capture_sink_handle_t s, esp_capture_overlay_if_t *ov);
esp_capture_err_t esp_capture_sink_enable_overlay(esp_capture_sink_handle_t s, bool en);
esp_capture_err_t esp_capture_sink_get_element_by_tag(esp_capture_sink_handle_t s, int stream,
                                                      const char *tag,
                                                      esp_gmf_element_handle_t *el);
int esp_gmf_video_ppa_set_cropped_rgn(esp_gmf_element_handle_t el, const esp_gmf_video_rgn_t *rgn);
int esp_gmf_video_fps_cvt_set_fps(esp_gmf_element_handle_t el, uint8_t fps);
int esp_gmf_video_enc_set_bitrate(esp_gmf_element_handle_t el, uint32_t bps);
int esp_gmf_video_enc_set_gop(esp_gmf_element_handle_t el, uint32_t gop);
int esp_gmf_video_enc_set_qp(esp_gmf_element_handle_t el, uint32_t qp_min, uint32_t qp_max);

esp_capture_overlay_if_t *esp_capture_new_text_overlay(const esp_capture_rgn_t *rgn);
esp_capture_err_t esp_capture_text_overlay_draw_start(esp_capture_overlay_if_t *ov);
esp_capture_err_t esp_capture_text_overlay_draw_finished(esp_capture_overlay_if_t *ov);
esp_capture_err_t esp_capture_text_overlay_clear(esp_capture_overlay_if_t *ov,
                                                 const esp_capture_rgn_t *rgn, uint16_t color);
esp_capture_err_t esp_capture_text_overlay_draw_text_fmt(esp_capture_overlay_if_t *ov,
                                                         const esp_capture_text_overlay_draw_info_t *info,
                                                         const char *fmt, ...);

#ifdef __cplusplus
}
#endif
