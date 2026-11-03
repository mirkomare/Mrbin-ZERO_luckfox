#pragma once

#include "CoreSettings.h"
#include "esp_http_server.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Impostazioni video usate per allineare aspect/FOV live ↔ registrazione (puntatore NVS/web).
void core_live_bind_settings(const core_settings_t *s);

// Dimensioni MJPEG live entro CORE_LIVE_* con lo stesso aspect della registrazione
// (es. rec 1920×1080 → anteprima 640×360). Il sensore resta in 960 binning.
void core_live_preview_size(uint16_t rec_w, uint16_t rec_h, uint16_t *out_w, uint16_t *out_h);

bool core_live_start(void);
void core_live_stop(void);
void core_live_request_stop(void);
/* Ask the Live worker to stop MJPEG and 3A, then wait until VI is idle.
 * Does not close sockets — the Record POST must stay up and return JSON. */
void core_live_force_release(void);
/* True when Live VI is already at the record size, so Registra can swap
 * VENC to H.264 without a sensor stream-off. */
bool core_live_can_handoff(const core_settings_t *s);
/* Client disconnect should park VI (leave 3A up) instead of DisableChn. */
void core_live_expect_handoff(void);
/* Detach Live from a matching VI. Does not DisableChn and does not kill 3A. */
bool core_live_take_for_record(uint16_t w, uint16_t h, uint8_t fps);
/* Swap failed: DisableChn first, then stop 3A. */
void core_live_abandon_handoff(void);
/* One-shot: true after a /tmp/mrbin-force-busy drill release. */
bool core_live_consume_busy_drill(void);
bool core_live_is_running(void);
bool core_live_wait_idle(uint32_t timeout_ms);
bool core_live_async_init(void);
esp_err_t core_live_stream(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
