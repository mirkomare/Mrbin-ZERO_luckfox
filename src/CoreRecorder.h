#pragma once

#include "CoreSettings.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;
    bool     starting;
    bool     stopping;
    bool     finalizing;
    bool     last_saved;
    uint32_t frame_count;
    char     last_path[128];
    char     error[96];
    uint16_t enc_w;
    uint16_t enc_h;
    uint8_t  enc_fps;
} core_recorder_manual_status_t;

typedef void (*core_recorder_export_progress_fn)(int done, int total, void *user);

// Registra fino a D2: capture immediato in PSRAM, SD in parallelo, mux MP4 su SD
bool core_recorder_run_session(const core_settings_t *settings);

// Recupera i .mp4.tmp orfani (registrazioni interrotte da spegnimento improvviso):
// cifra e finalizza quelli validi, elimina quelli corrotti. Ritorna il numero di
// file rimossi/recuperati. Da chiamare quando NON si sta registrando (config mode).
int core_recorder_recover_tmp_files(const core_settings_t *settings);

// Processa marker "*.encrypt-later" (PIR tbrec / PIR CoreRecorder): cifra MRBI se
// rec_encrypt+chiave, altrimenti rimuove solo il flag. Da chiamare in CONFIG.
int core_recorder_process_pending_encrypt(const core_settings_t *settings);

// Registrazione manuale da Web (stop esplicito, nessun DONE TPL)
bool core_recorder_manual_start(const core_settings_t *settings);
bool core_recorder_manual_stop(void);
bool core_recorder_manual_wait_done(uint32_t timeout_ms);
bool core_recorder_manual_is_active(void);
void core_recorder_manual_get_status(core_recorder_manual_status_t *out);
// Lock CSI/status before Live stop so /live/stream cannot restart mid-start.
bool core_recorder_manual_mark_starting(void);
void core_recorder_manual_clear_starting(void);

/* DEMO: apre VI/encoder al 2° frame movimento così la clip non perde 5 s di pre-roll. */
void core_demo_rec_prep_kick(const core_settings_t *settings);
void core_demo_rec_prep_cancel(void);
bool core_demo_rec_prep_is_ready(void);
bool core_demo_rec_prep_wait_warm(uint32_t timeout_ms);

// Esporta sequenza JPEG (timelapse foto) in AVI MJPEG su SD.
// jpeg_abs_paths: path assoluti ordinati; fps tipicamente 1.
// aes_key: se non null, decifra eventuali JPEG MRBI in ingresso.
// encrypt_out: se true richiede aes_key e scrive AVI con header MRBI+AES.
bool core_recorder_export_jpegs_avi(const char *out_abs_avi,
                                    char jpeg_abs_paths[][192],
                                    int jpeg_count,
                                    uint8_t fps,
                                    const uint8_t *aes_key,
                                    bool encrypt_out,
                                    core_recorder_export_progress_fn progress,
                                    void *progress_user);

#ifdef __cplusplus
}
#endif
