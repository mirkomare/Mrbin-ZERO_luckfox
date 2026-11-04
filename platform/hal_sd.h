#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDMMC_HOST_SLOT_0 0
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1

typedef struct { int dummy; } sdmmc_card_t;
typedef struct {
    int slot;
    int max_freq_khz;
    void *init;
    void *deinit;
    void *pwr_ctrl_handle;
} sdmmc_host_t;

typedef struct {
    int width;
    int clk, cmd, d0, d1, d2, d3;
    int flags;
} sdmmc_slot_config_t;

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;

#define SDMMC_HOST_DEFAULT() { 0, SDMMC_FREQ_DEFAULT, nullptr, nullptr, nullptr }
#define SDMMC_SLOT_CONFIG_DEFAULT() { 4, -1, -1, -1, -1, -1, -1, 0 }

typedef void *sd_pwr_ctrl_handle_t;
typedef struct { int ldo_chan_id; } sd_pwr_ctrl_ldo_config_t;

esp_err_t esp_vfs_fat_sdmmc_mount(const char *mp, const sdmmc_host_t *host,
                                  const sdmmc_slot_config_t *slot,
                                  const esp_vfs_fat_sdmmc_mount_config_t *cfg,
                                  sdmmc_card_t **out);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *mp, sdmmc_card_t *card);
esp_err_t esp_vfs_fat_sdcard_format(const char *mp, sdmmc_card_t *card);
esp_err_t esp_vfs_fat_info(const char *mp, uint64_t *total, uint64_t *free_b);
void sdmmc_card_print_info(void *fp, sdmmc_card_t *card);
esp_err_t sdmmc_host_init(void);
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *c, sd_pwr_ctrl_handle_t *h);
esp_err_t sd_pwr_ctrl_set_io_voltage(sd_pwr_ctrl_handle_t h, int mv);

bool hal_sd_probe_mount(const char *mp);
/* Kernel vfat spesso finisce in ro (errors=remount-ro) dopo un errore I/O. */
bool hal_sd_mount_is_readonly(const char *mp);
bool hal_sd_remount_rw(const char *mp);
/* fsck.vfat -a una sola volta, stdin da /dev/null, solo a volume smontato. */
bool hal_sd_repair_vfat(const char *mp);
bool hal_sd_format_vfat(const char *mp, const char *dev);
bool hal_sd_probe_write(const char *mp);

#ifdef __cplusplus
}
#endif
