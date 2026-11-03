#pragma once

#include "CoreSidecar.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_CLIP_NONE = 0,
    CORE_CLIP_RECORDED_PLAIN,
    CORE_CLIP_SIDECAR_WRITTEN,
    CORE_CLIP_ENCRYPTED_LOCAL,
    CORE_CLIP_PENDING_UPLOAD,
    CORE_CLIP_UPLOADING,
    CORE_CLIP_UPLOADED_VERIFIED,
    CORE_CLIP_DELETE_PENDING,
    CORE_CLIP_DELETED,
    CORE_CLIP_ERROR,
} core_clip_state_t;

typedef struct {
    char     mp4_path[160];
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
} core_clip_finish_t;

typedef struct {
    int pending;
    int uploading;
    int verified;
    int error;
    int plaintext;
} core_clip_queue_counts_t;

FILE *core_clip_h264_open(const char *mp4_path);
bool core_clip_h264_rename(const char *old_mp4, const char *new_mp4);

bool core_clip_queue_on_segment(const core_clip_finish_t *info);
void core_clip_queue_process(volatile bool *cancel);
void core_clip_queue_counts(core_clip_queue_counts_t *out);
core_clip_state_t core_clip_queue_state_for_mp4(const char *mp4_path);
const char *core_clip_state_label(core_clip_state_t st);
bool core_clip_queue_clear_errors(void);

#ifdef __cplusplus
}
#endif
