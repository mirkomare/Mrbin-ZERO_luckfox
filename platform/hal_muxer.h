#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_MUXER_ERR_OK 0
#define ESP_MUXER_TYPE_MP4 1
#define ESP_MUXER_TYPE_AVI 2
#define ESP_MUXER_VDEC_H264  0
#define ESP_MUXER_VDEC_MJPEG 1
#define AVI_MUXER_INDEX_AT_END 1

typedef int esp_muxer_err_t;
typedef void *esp_muxer_handle_t;

typedef struct {
    char file_path[256];
    int len;
} esp_muxer_slice_info_t;

typedef int (*esp_muxer_url_pattern_ex_t)(esp_muxer_slice_info_t *info, void *ctx);

typedef struct {
    int muxer_type;
    uint32_t slice_duration;
    const char *url_pattern;
    esp_muxer_url_pattern_ex_t url_pattern_ex;
    void *ctx;
    uint32_t ram_cache_size;
    bool no_key_frame_verify;
} esp_muxer_config_t;

typedef struct {
    esp_muxer_config_t base_config;
    bool display_in_order;
    bool moov_before_mdat;
} mp4_muxer_config_t;

typedef struct {
    esp_muxer_config_t base_config;
    int index_type;
} avi_muxer_config_t;

typedef struct {
    void *(*on_open)(char *path);
    int (*on_write)(void *writer, void *buffer, int len);
    int (*on_seek)(void *writer, uint64_t pos);
    int (*on_close)(void *writer);
} esp_muxer_file_writer_t;

typedef struct {
    int codec;
    int width;
    int height;
    int fps;
    uint32_t min_packet_duration;
    const uint8_t *codec_spec_info;
    int spec_info_len;
} esp_muxer_video_stream_info_t;

typedef struct {
    uint8_t *data;
    int len;
    uint32_t pts;
    uint32_t dts;
    bool key_frame;
} esp_muxer_video_packet_t;

void mp4_muxer_register(void);
void avi_muxer_register(void);
esp_muxer_handle_t esp_muxer_open(esp_muxer_config_t *cfg, size_t cfg_size);
esp_muxer_err_t esp_muxer_set_file_writer(esp_muxer_handle_t h, const esp_muxer_file_writer_t *w);
esp_muxer_err_t esp_muxer_add_video_stream(esp_muxer_handle_t h,
                                           const esp_muxer_video_stream_info_t *info, int *idx);
esp_muxer_err_t esp_muxer_add_video_packet(esp_muxer_handle_t h, int idx,
                                           esp_muxer_video_packet_t *pkt);
esp_muxer_err_t esp_muxer_close(esp_muxer_handle_t h);

#ifdef __cplusplus
}
#endif
