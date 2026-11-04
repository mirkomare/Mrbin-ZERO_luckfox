#pragma once

#include "CoreSettings.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Listener on :554 (or :8554). Both encoders run only when rtsp_always is set.
 * A client does not start them. /live is channel 1.
 * La registrazione locale non usa questo streamer: RTSP e file non insieme. */
void core_rtsp_start(const core_settings_t *settings);
void core_rtsp_apply(const core_settings_t *settings);

/* Stop the encoder and wait until the camera is free. Nested. */
void core_rtsp_hold(void);
void core_rtsp_release(void);
/* Azzera i hold annidati così lo streamer può aprire la camera. */
void core_rtsp_unhold_all(void);
int core_rtsp_pipe_up(void);
int core_rtsp_pipe_busy(void);

/* Keep the H.264 pipe up and copy each encoded frame for the recorder.
 * The file and the RTSP clients receive the same NALs. 0 on success. */
int core_rtsp_rec_attach(const core_settings_t *settings);
void core_rtsp_rec_detach(void);
int core_rtsp_rec_take(uint8_t **data, int *len, uint32_t *pts, int timeout_ms);

/* rtsp://<ip>:<port>/live  (alias del canale 1) */
void core_rtsp_url(char *out, size_t out_len);
/* ch 2 → /ch2, altrimenti /ch1 */
void core_rtsp_url_ch(int ch, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
