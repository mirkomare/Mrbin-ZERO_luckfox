#pragma once



#include <stdbool.h>

#include <stdint.h>



#ifdef __cplusplus

extern "C" {

#endif



typedef struct core_h264_frame {

    uint32_t             pts_ms;

    uint32_t             size;

    uint32_t             pool_off;

    struct core_h264_frame *next;

} core_h264_frame_t;



typedef struct {

    uint8_t             *pool;

    uint32_t             pool_size;

    uint32_t             pool_used;

    uint32_t             pool_reclaim;

    core_h264_frame_t   *head;

    core_h264_frame_t   *tail;

    uint32_t             frame_count;

} core_h264_buffer_t;



// Pool = CORE_H264_PSRAM_USE_PCT % della SPIRAM libera al momento dell'init.

uint32_t core_h264_buffer_pool_bytes(void);



static inline uint32_t core_h264_buffer_bytes_used(const core_h264_buffer_t *buf) {

    if (!buf || buf->pool_used <= buf->pool_reclaim) {

        return 0;

    }

    return buf->pool_used - buf->pool_reclaim;

}



static inline const uint8_t *core_h264_frame_data(const core_h264_buffer_t *buf,

                                                  const core_h264_frame_t *f) {

    return buf->pool + f->pool_off;

}



uint32_t core_h264_buffer_fill_percent(const core_h264_buffer_t *buf);

bool core_h264_buffer_should_flush(const core_h264_buffer_t *buf);

uint32_t core_h264_buffer_estimated_sec(const core_h264_buffer_t *buf);



uint32_t core_h264_buffer_head_pts(const core_h264_buffer_t *buf);
uint32_t core_h264_buffer_tail_pts(const core_h264_buffer_t *buf);

bool core_h264_buffer_init(core_h264_buffer_t *buf);
bool core_h264_buffer_init_recording(core_h264_buffer_t *buf, bool pir_fast, bool rec_encrypt);
bool core_h264_buffer_init_pool(core_h264_buffer_t *buf, uint32_t pool_bytes);

void core_h264_buffer_deinit(core_h264_buffer_t *buf);

void core_h264_buffer_clear(core_h264_buffer_t *buf);

bool core_h264_buffer_push(core_h264_buffer_t *buf, uint32_t pts_ms, const uint8_t *data, uint32_t size);

bool core_h264_buffer_pop_head(core_h264_buffer_t *buf);

bool core_h264_buffer_extract_h264_config(const core_h264_buffer_t *buf, uint8_t *out, int out_cap, int *out_len);

bool core_h264_buffer_has_idr(const core_h264_buffer_t *buf);

bool core_h264_is_idr_nal(const uint8_t *data, uint32_t size);



#ifdef __cplusplus

}

#endif

