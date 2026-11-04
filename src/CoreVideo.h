#pragma once

#include "CoreSettings.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_VIDEO_SENSOR_UNKNOWN = 0,
    CORE_VIDEO_SENSOR_OV5647,
    CORE_VIDEO_SENSOR_IMX708,
    CORE_VIDEO_SENSOR_SC3336,
    CORE_VIDEO_SENSOR_IMX415,
} core_video_sensor_kind_t;

bool core_video_init(void);
bool core_video_is_ready(void);
// FD persistente su /dev/video0 (reference VFS > 0). -1 se non aperto.
int core_video_device_fd(void);
void core_video_deinit(void);
// Init camera per live: 960 binning + Bayer (s ignorato; 1080p solo in registrazione).
bool core_video_prepare_for_stream(const core_settings_t *s);

// Imposta capovolgimento (vflip) e specchiatura (hflip) del sensore.
// Aggiorna anche il Bayer ISP (il flip sensore cambia la fase 2×2).
// Se c'è uno stream attivo va fermato prima, altrimenti S_SENSOR_FMT lo spezza.
void core_video_set_flip(bool vflip, bool hflip);
bool core_video_get_vflip(void);
bool core_video_get_hflip(void);
// True se l'ultimo apply ha scritto HFLIP/VFLIP sul subdev SC3336 (o video0).
bool core_video_flip_hw_applied(void);
/* Online wrap (H.264 sopra 1080p) non accetta enMirror: 1 = gira il sensore,
 * 0 = azzera il sensore e lascia il giro all'encoder. */
void core_video_orient_via_sensor(int via_sensor);
int core_video_orient_on_sensor(void);

// Riapplica l'orientamento corrente al sensore (serve dopo esp_capture_start,
// perché VIDIOC_S_FMT ricarica la tabella registri OV5647 e azzera flip/mirror).
void core_video_reapply_flip(void);
// Stock rkaiq 3A + CPROC/AE from /etc/mrbin/isp.env (set-fmt can reset ISP).
void core_video_reapply_isp(void);
/* Rec: CPROC + REC_FLAG before VI. 3A only after VI (rec-3a). */
void core_video_isp_rec_begin(void);
void core_video_isp_rec_after_vi(void);
void core_video_isp_rec_end(void);
/* PIR: 3A already started or a short arm, then direct AE. No 8 s wait. */
void core_video_isp_pir_arm(void);
/* After EnableChn: if exposure fell under 200, write 816/635 once. */
void core_video_isp_pir_lock(void);
void core_video_isp_demo_motion_stabilize(void);
/* Live: one niced 3A after VI configure and before EnableChn, so rkisp
 * gets IQ at stream-on. A 3A started after stream-on leaves UV at 128. */
void core_video_isp_live_pre(void);
/* Settle AWB/CCM on that 3A, then stop it. Does not start a late 3A. */
void core_video_isp_live_after_vi(void);
/* Stop rkaiq_3A before MPI/VI teardown — params stream-off race. */
void core_video_isp_stop_3a(void);
/* Monotonic ms stamp in /tmp/mrbin-snap.log (stderr is /dev/null on device). */
void core_snap(const char *msg);

// Riapplica formato sensore con Bayer modulo + flip (AF=BGGR, fisso=GRBG, poi H/V).
bool core_video_sync_bayer_order(bool vflip, bool hflip);

// Fase Bayer ISP: auto (modulo) o manuale RGGB/GRBG/GBRG/BGGR (NVS calibrazione).
void core_video_set_bayer_mode(uint8_t mode);
uint8_t core_video_get_bayer_mode(void);
const char *core_video_bayer_mode_label(uint8_t mode);
const char *core_video_bayer_effective_name(bool vflip, bool hflip);
bool core_video_apply_bayer_sync(void);

// Dopo esp_video_init: true se presente VCM (DW9714 su OV5647, DW9807 su IMX708).
void core_video_set_module_af(bool is_af);
void core_video_reset_module_probe(void);
bool core_video_module_is_af(void);
bool core_video_module_probed(void);

// Chip sul bus CSI: SC3336 @ 0x30, OV5647 @ 0x36, IMX708 @ 0x1A.
void core_video_detect_sensor(void);
core_video_sensor_kind_t core_video_sensor_kind(void);
const char *core_video_sensor_name(void);
const char *core_video_sensor_label(void);

// Focus assoluto VCM (0–1023). False se non AF / camera non pronta.
bool core_video_focus_set(uint16_t pos);
bool core_video_focus_get(uint16_t *out_pos);
bool core_video_focus_get_range(uint16_t *out_min, uint16_t *out_max);

// Allinea il sensore alla registrazione.
// OV5647: 1280×960. IMX708: 1920×1080. SC3336: rkisp 2304×1296, VENC fino a 3MP@30.
bool core_video_prepare_for_settings(const core_settings_t *s);

// Dimensioni correnti del sensore ( dopo prepare / init ).
bool core_video_get_sensor_size(uint16_t *out_w, uint16_t *out_h);
bool core_video_get_sensor_fps(uint8_t *out_fps);

// IMX708: indice modo (0=1080, 1=720p, 2=1024x768) e encode senza bordo morto.
size_t core_video_imx708_mode_index(uint16_t want_w, uint16_t want_h);
void core_video_imx708_encode_size(uint16_t want_w, uint16_t want_h,
                                   uint16_t *out_w, uint16_t *out_h);
bool core_video_imx708_trim_rgn(uint16_t sw, uint16_t sh,
                                uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif
