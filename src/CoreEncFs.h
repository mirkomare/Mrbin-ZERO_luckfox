#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Prefisso del filesystem virtuale cifrante. Un file aperto come
// "/enc/sdcard/<...>.mp4" viene scritto CIFRATO (AES-128-CTR) su
// "/sdcard/<...>.mp4" con header [MRBI(4)][IV(16)] e payload cifrato.
// La cifratura è on-the-fly (nessun file in chiaro, nessun secondo passaggio).
#define CORE_ENCFS_PREFIX "/enc"

// Registra il VFS cifrante (idempotente).
bool core_encfs_register(void);

// Imposta la chiave AES-128 usata per le prossime aperture in scrittura/lettura.
// Va chiamata prima di aprire il file tramite il muxer.
void core_encfs_set_key(const uint8_t key[16]);

// Costruisce in `out` il path "/enc" + real_path (es. "/sdcard/..").
bool core_encfs_make_path(const char *real_path, char *out, unsigned out_len);

#ifdef __cplusplus
}
#endif
