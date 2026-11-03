#include "CoreH264Buffer.h"

#include "CoreConfig.h"



#include "esp_heap_caps.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>

#include <string.h>



static const char *TAG = "core_h264_buf";



uint32_t core_h264_buffer_pool_bytes(void) {
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint64_t reserve = (uint64_t)CORE_PSRAM_FLUSH_RESERVE_BYTES + (uint64_t)CORE_PSRAM_TAIL_RESERVE_BYTES;
    uint64_t avail = free_spiram;
    if (avail > reserve) {
        avail -= reserve;
    } else if (avail > CORE_PSRAM_TAIL_RESERVE_BYTES) {
        avail -= CORE_PSRAM_TAIL_RESERVE_BYTES;
    } else {
        avail = avail / 2U;
    }
    uint64_t raw = (avail * (uint64_t)CORE_H264_PSRAM_USE_PCT) / 100ULL;
    if (raw < CORE_H264_PSRAM_MIN_BYTES) {
        raw = CORE_H264_PSRAM_MIN_BYTES;
    }
    if (raw > free_spiram) {
        raw = free_spiram;
    }
    return (uint32_t)raw;
}

uint32_t core_h264_buffer_pool_bytes_pir_pct(uint8_t use_pct) {
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint64_t reserve = (uint64_t)CORE_PSRAM_FLUSH_RESERVE_BYTES + (uint64_t)CORE_PSRAM_TAIL_RESERVE_BYTES
                     + (uint64_t)CORE_H264_ENCODER_RESERVE_BYTES;
    uint64_t avail = free_spiram;
    if (avail > reserve) {
        avail -= reserve;
    } else if (avail > CORE_PSRAM_TAIL_RESERVE_BYTES) {
        avail -= CORE_PSRAM_TAIL_RESERVE_BYTES;
    } else {
        avail = avail / 2U;
    }
    uint64_t raw = (avail * (uint64_t)use_pct) / 100ULL;
    if (raw < CORE_H264_PSRAM_MIN_BYTES) {
        raw = CORE_H264_PSRAM_MIN_BYTES;
    }
    if (raw > free_spiram) {
        raw = free_spiram;
    }
    return (uint32_t)raw;
}

uint32_t core_h264_buffer_pool_bytes_pir(void) {
    return core_h264_buffer_pool_bytes_pir_pct(CORE_H264_PSRAM_USE_PCT_PIR_ENC);
}



uint32_t core_h264_buffer_fill_percent(const core_h264_buffer_t *buf) {

    if (!buf || buf->pool_size == 0) {

        return 0;

    }

    return (core_h264_buffer_bytes_used(buf) * 100U) / buf->pool_size;

}



bool core_h264_buffer_should_flush(const core_h264_buffer_t *buf) {

    return core_h264_buffer_fill_percent(buf) >= (uint32_t)CORE_H264_PSRAM_FLUSH_PCT;

}



uint32_t core_h264_buffer_estimated_sec(const core_h264_buffer_t *buf) {

    if (!buf) {

        return 0;

    }

    uint32_t bytes = core_h264_buffer_bytes_used(buf);

    if (bytes == 0 || CORE_VIDEO_BITRATE == 0) {

        return 0;

    }

    return (uint32_t)(((uint64_t)bytes * 8ULL * 100ULL) / ((uint64_t)CORE_VIDEO_BITRATE * 100ULL));

}



static int find_nal_start(const uint8_t *data, uint32_t size, uint32_t offset, uint32_t *nal_off) {

    for (uint32_t i = offset; i + 3 < size; ++i) {

        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {

            *nal_off = i;

            return 3;

        }

        if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {

            *nal_off = i;

            return 4;

        }

    }

    return 0;

}



bool core_h264_is_idr_nal(const uint8_t *data, uint32_t size) {

    if (!data || size < 5) {

        return false;

    }

    uint32_t off = 0;

    uint32_t nal_off = 0;

    int sc = find_nal_start(data, size, 0, &nal_off);

    while (sc > 0) {

        uint32_t hdr = nal_off + sc;

        if (hdr < size) {

            uint8_t nal_type = data[hdr] & 0x1f;

            if (nal_type == 5) {

                return true;

            }

        }

        off = nal_off + sc + 1;

        sc = find_nal_start(data, size, off, &nal_off);

    }

    return false;

}



static void pool_compact(core_h264_buffer_t *buf) {

    if (!buf || buf->pool_reclaim == 0) {

        return;

    }

    uint32_t live = core_h264_buffer_bytes_used(buf);

    if (live > 0) {

        memmove(buf->pool, buf->pool + buf->pool_reclaim, live);

    }

    for (core_h264_frame_t *f = buf->head; f; f = f->next) {

        f->pool_off -= buf->pool_reclaim;

    }

    buf->pool_used -= buf->pool_reclaim;

    buf->pool_reclaim = 0;

}



static bool pool_ensure_space(core_h264_buffer_t *buf, uint32_t size) {

    if (core_h264_buffer_bytes_used(buf) + size > buf->pool_size) {

        return false;

    }

    if (buf->pool_used + size > buf->pool_size) {

        pool_compact(buf);

    }

    return buf->pool_used + size <= buf->pool_size;

}



uint32_t core_h264_buffer_head_pts(const core_h264_buffer_t *buf) {
    if (!buf || !buf->head) {
        return 0;
    }
    return buf->head->pts_ms;
}

uint32_t core_h264_buffer_tail_pts(const core_h264_buffer_t *buf) {
    if (!buf || !buf->tail) {
        return 0;
    }
    return buf->tail->pts_ms;
}

bool core_h264_buffer_init_pool(core_h264_buffer_t *buf, uint32_t pool_size) {

    if (!buf || pool_size == 0) {

        return false;

    }

    memset(buf, 0, sizeof(*buf));

    buf->pool_size = pool_size;

    buf->pool = (uint8_t *)heap_caps_malloc(buf->pool_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf->pool) {

        ESP_LOGE(TAG, "Buffer PSRAM %lu KB non allocabile", (unsigned long)(buf->pool_size / 1024));

        return false;

    }

    ESP_LOGI(TAG, "Buffer PSRAM staging: %lu KB", (unsigned long)(buf->pool_size / 1024));

    return true;

}



bool core_h264_buffer_init(core_h264_buffer_t *buf) {

    if (!buf) {

        return false;

    }

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    uint32_t pool_size = core_h264_buffer_pool_bytes();

    if (!core_h264_buffer_init_pool(buf, pool_size)) {

        return false;

    }

    ESP_LOGI(TAG, "Pool PSRAM: %lu KB (%u%% di %lu KB libera post-pipeline, %dx%d@%d)",
             (unsigned long)(buf->pool_size / 1024), (unsigned)CORE_H264_PSRAM_USE_PCT,
             (unsigned long)(free_before / 1024),
             CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS);

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "SPIRAM residua dopo pool: %lu KB (riservati %lu KB per flush SD/muxer)",
             (unsigned long)(free_after / 1024),
             (unsigned long)(CORE_PSRAM_FLUSH_RESERVE_BYTES / 1024));

    return true;

}

bool core_h264_buffer_init_recording(core_h264_buffer_t *buf, bool pir_fast, bool rec_encrypt) {
    if (!buf) {
        return false;
    }
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t use_pct = CORE_H264_PSRAM_USE_PCT;
    uint32_t pool_size;
    if (pir_fast) {
        /* Fixed encoded-H.264 preroll. Do not take a percent of free RAM:
         * a raw NV12 ring does not fit in 64 MB beside the 24 MB CMA. */
        (void)rec_encrypt;
        use_pct = 0;
        pool_size = CORE_PIR_H264_RING_BYTES;
    } else {
        pool_size = core_h264_buffer_pool_bytes();
    }
    if (!core_h264_buffer_init_pool(buf, pool_size)) {
        return false;
    }
    if (pir_fast) {
        ESP_LOGI(TAG, "Pool PSRAM: %lu KB (PIR preroll H.264 encoded, non NV12 raw; libera %lu KB)",
                 (unsigned long)(buf->pool_size / 1024),
                 (unsigned long)(free_before / 1024));
    } else {
        ESP_LOGI(TAG, "Pool PSRAM: %lu KB (%s %s, %u%% di %lu KB libera, encoder reserve %lu KB)",
                 (unsigned long)(buf->pool_size / 1024),
                 "web",
                 "config",
                 (unsigned)use_pct,
                 (unsigned long)(free_before / 1024),
                 0UL);
    }
    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SPIRAM residua dopo pool: %lu KB (riservati %lu KB per flush SD/muxer%s)",
             (unsigned long)(free_after / 1024),
             (unsigned long)(CORE_PSRAM_FLUSH_RESERVE_BYTES / 1024),
             pir_fast ? " + encoder" : "");
    return true;
}



void core_h264_buffer_deinit(core_h264_buffer_t *buf) {

    if (!buf) {

        return;

    }

    core_h264_buffer_clear(buf);

    if (buf->pool) {

        heap_caps_free(buf->pool);

        buf->pool = nullptr;

    }

    buf->pool_size = 0;

    buf->pool_used = 0;

    buf->pool_reclaim = 0;

}



void core_h264_buffer_clear(core_h264_buffer_t *buf) {

    if (!buf) {

        return;

    }

    core_h264_frame_t *cur = buf->head;

    while (cur) {

        core_h264_frame_t *next = cur->next;

        free(cur);

        cur = next;

    }

    buf->head = buf->tail = nullptr;

    buf->frame_count = 0;

    buf->pool_used = 0;

    buf->pool_reclaim = 0;

}



bool core_h264_buffer_pop_head(core_h264_buffer_t *buf) {

    if (!buf || !buf->head) {

        return false;

    }

    core_h264_frame_t *cur = buf->head;

    buf->head = cur->next;

    if (!buf->head) {

        buf->tail = nullptr;

    }

    buf->pool_reclaim += cur->size;

    buf->frame_count--;

    free(cur);



    if (!buf->head) {

        buf->pool_used = 0;

        buf->pool_reclaim = 0;

    }
    // Non compattare qui: memmove su MB di PSRAM sotto mutex ring bloccava JOB1/JOB2
    // per decine di ms (fps ~8–10). La compattazione avviene solo in pool_ensure_space
    // quando un push ha davvero bisogno di spazio fisico in coda al pool.

    return true;

}



bool core_h264_buffer_push(core_h264_buffer_t *buf, uint32_t pts_ms, const uint8_t *data, uint32_t size) {

    if (!buf || !buf->pool || !data || size == 0) {

        return false;

    }

    if (!pool_ensure_space(buf, size)) {

        ESP_LOGW(TAG, "Pool PSRAM pieno (%u%%, %lu / %lu KB, %lu frame)",

                 (unsigned)core_h264_buffer_fill_percent(buf),

                 (unsigned long)(core_h264_buffer_bytes_used(buf) / 1024),

                 (unsigned long)(buf->pool_size / 1024),

                 (unsigned long)buf->frame_count);

        return false;

    }



    core_h264_frame_t *frame = (core_h264_frame_t *)malloc(sizeof(*frame));

    if (!frame) {

        ESP_LOGE(TAG, "Alloc nodo frame fallita");

        return false;

    }



    frame->pool_off = buf->pool_used;

    frame->pts_ms = pts_ms;

    frame->size = size;

    frame->next = nullptr;

    memcpy(buf->pool + frame->pool_off, data, size);

    buf->pool_used += size;



    if (buf->tail) {

        buf->tail->next = frame;

    } else {

        buf->head = frame;

    }

    buf->tail = frame;

    buf->frame_count++;

    uint32_t pct = core_h264_buffer_fill_percent(buf);
    if (pct >= 80 && pct < (uint32_t)CORE_H264_PSRAM_FLUSH_PCT) {
        static int64_t s_last_pool_log_us = 0;
        static uint32_t s_last_pool_log_pct = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_pool_log_us >= 2000000 ||
            pct >= s_last_pool_log_pct + 5) {
            s_last_pool_log_us = now;
            s_last_pool_log_pct = pct;
            ESP_LOGI(TAG, "Pool PSRAM %u%% (~%u s stimati)",
                     (unsigned)pct, (unsigned)core_h264_buffer_estimated_sec(buf));
        }
    }

    return true;

}



bool core_h264_buffer_has_idr(const core_h264_buffer_t *buf) {

    if (!buf) {

        return false;

    }

    for (core_h264_frame_t *f = buf->head; f; f = f->next) {

        if (core_h264_is_idr_nal(core_h264_frame_data(buf, f), f->size)) {

            return true;

        }

    }

    return false;

}



bool core_h264_buffer_extract_h264_config(const core_h264_buffer_t *buf, uint8_t *out, int out_cap, int *out_len) {

    if (!buf || !out || !out_len || out_cap < 8) {

        return false;

    }

    *out_len = 0;



    for (core_h264_frame_t *f = buf->head; f; f = f->next) {

        const uint8_t *data = core_h264_frame_data(buf, f);

        uint32_t off = 0;

        uint32_t nal_off = 0;

        int sc = find_nal_start(data, f->size, 0, &nal_off);

        while (sc > 0) {

            uint32_t hdr = nal_off + sc;

            if (hdr >= f->size) {

                break;

            }

            uint8_t nal_type = data[hdr] & 0x1f;

            uint32_t next_off = f->size;

            uint32_t next_nal = 0;

            int next_sc = find_nal_start(data, f->size, hdr + 1, &next_nal);

            if (next_sc > 0) {

                next_off = next_nal;

            }

            uint32_t nal_size = next_off - nal_off;

            if ((nal_type == 7 || nal_type == 8) && (int)(*out_len + nal_size) <= out_cap) {

                memcpy(out + *out_len, data + nal_off, nal_size);

                *out_len += (int)nal_size;

            }

            off = hdr + 1;

            sc = find_nal_start(data, f->size, off, &nal_off);

        }

        if (*out_len > 0) {

            return true;

        }

    }

    return false;

}

