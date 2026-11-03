#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Da chiamare PRIMA del mount se ESP-Hosted è stato spento (esp_hosted_deinit):
// il controller SDMMC non è più inizializzato da hosted, quindi CoreSD deve
// fare la vera sdmmc_host_init invece della dummy.
void core_sd_set_hosted_owns_host(bool hosted_owns);

// Accende LDO_VO4 a 3,3 V (VDD_IO_5 = banco GPIO39-48, include SQW GPIO46).
// Va chiamata PRIMA di core_gpio_init(): finché l'LDO resta a 0 V i pad del banco
// non pilotano e la linea segue i pull-up esterni.
bool core_sd_power_io_early(void);

bool core_sd_init(void);
bool core_sd_init_pir(void);   // PIR: solo 40 MHz, niente fallback/remount a 20 MHz
bool core_sd_init_boot(void);  // config: più retry e delay tra tentativi
bool core_sd_reinit(void);     // smonta e rimonta (scheda inserita a caldo, retry)
bool core_sd_remount_default_freq(void);  // rimonta a 20 MHz (bus marginale a 40 MHz: CRC retry silenziosi)
bool core_sd_is_mounted(void);
/* Se vfat è ro (errno 30), tenta remount rw — necessario per MP4 e delete da GUI. */
bool core_sd_ensure_rw(void);
bool core_sd_format(void);
uint64_t core_sd_free_bytes(void);
bool core_sd_ensure_space(uint64_t min_free);

// Crea path directory DDMMAAAA sotto /sdcard
bool core_sd_make_day_dir(char *out_path, size_t out_len);

// Nome file: {CORE_ID}_{seq000-999}_{HHMMSS}.mp4 (path completo in out_path)
bool core_sd_make_recording_path(const char *day_dir, uint32_t core_id,
                                 char *out_path, size_t out_len);

// Sidecar {base}.meta: "duration_sec frames [fps]\n" (fps opzionale, 0 = assente/legacy).
bool core_sd_write_rec_meta(const char *media_path_or_base, uint32_t duration_sec, uint32_t frames,
                            uint32_t fps);
bool core_sd_read_rec_meta(const char *media_path_or_base, uint32_t *duration_sec, uint32_t *frames,
                           uint32_t *fps);

#ifdef __cplusplus
}
#endif
