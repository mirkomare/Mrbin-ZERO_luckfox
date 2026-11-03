#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     video_file[80];
    char     sidecar_file[80];
    uint16_t width;
    uint16_t height;
    int      fps;
    int      fps_source;
    int      fps_target;
    int      fps_recorded;
    uint32_t duration_ms;
    uint32_t bitrate;
    uint32_t gop;
    uint32_t qp_min;
    uint32_t qp_max;
    uint32_t sequence;
    bool     pir_enabled;
    bool     motion_extend;
    bool     osd_enable;
    char     preset[32];
} core_sidecar_info_t;

bool core_sidecar_write(const char *json_path, const core_sidecar_info_t *info);

#ifdef __cplusplus
}
#endif
