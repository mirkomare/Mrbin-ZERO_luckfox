#include "CoreSD.h"
#include "CoreConfig.h"
#include "hal_sd.h"
#include "CoreRtc.h"
#include "CoreSettings.h"

#include "dirent.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "sys/stat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
// ESP-Hosted init (constructor) already owns sdmmc_host_init on slot 1 (C6 WiFi).
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 1
#else
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

static const char *TAG = "core_sd";
static sdmmc_card_t *s_card = nullptr;
static bool s_mounted = false;
static bool s_vfat_repair_used = false;
static char s_last_mount_err[48] = {0};
static SemaphoreHandle_t s_mount_mtx = nullptr;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t s_pwr_ctrl = nullptr;
#endif

#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
static bool s_hosted_owns_host = true;

static esp_err_t sdmmc_host_init_dummy(void) {
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_dummy(void) {
    return ESP_OK;
}

// Usata quando hosted è stato spento: la sdmmc_host_init vera, tollerando
// il caso in cui il controller risulti già inizializzato.
static esp_err_t sdmmc_host_init_tolerant(void) {
    esp_err_t err = sdmmc_host_init();
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}
#endif

void core_sd_set_hosted_owns_host(bool hosted_owns) {
#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    s_hosted_owns_host = hosted_owns;
#else
    (void)hosted_owns;
#endif
}

#if SOC_SDMMC_IO_POWER_EXTERNAL
static bool core_sd_setup_ldo(void) {
    if (s_pwr_ctrl) return true;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CORE_SD_LDO_CHAN_ID,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO SD (canale %d) fallito: %s", CORE_SD_LDO_CHAN_ID, esp_err_to_name(err));
        return false;
    }
    // sd_pwr_ctrl_new_on_chip_ldo acquisisce il canale senza tensione (0 V): il banco
    // VDD_IO_5 resterebbe spento fino al mount. Portiamolo subito a 3,3 V.
    err = sd_pwr_ctrl_set_io_voltage(s_pwr_ctrl, 3300);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO SD: set 3300 mV fallito: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "LDO SD attivo (canale %d, 3300 mV)", CORE_SD_LDO_CHAN_ID);
    return true;
}
#endif

bool core_sd_power_io_early(void) {
#if SOC_SDMMC_IO_POWER_EXTERNAL
    return core_sd_setup_ldo();
#else
    return true;
#endif
}

static void core_sd_release(void) {
    if (s_mounted && s_card) {
        esp_vfs_fat_sdcard_unmount(CORE_SD_MOUNT_POINT, s_card);
    }
    s_card = nullptr;
    s_mounted = false;
}

static bool core_sd_mount_once(bool format_if_failed, int max_freq_khz) {
    if (s_mounted) {
        return true;
    }

#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (!core_sd_setup_ldo()) {
        return false;
    }
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_failed,
        .max_files = CORE_SD_MOUNT_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = max_freq_khz;
#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    // Se hosted è attivo possiede già il controller (init dummy); se è stato
    // spento per la sessione PIR, la init vera la facciamo noi.
    host.init = s_hosted_owns_host ? &sdmmc_host_init_dummy : &sdmmc_host_init_tolerant;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif
#if SOC_SDMMC_IO_POWER_EXTERNAL
    host.pwr_ctrl_handle = s_pwr_ctrl;
#endif

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;
    slot_cfg.clk = CORE_SD_PIN_CLK;
    slot_cfg.cmd = CORE_SD_PIN_CMD;
    slot_cfg.d0 = CORE_SD_PIN_D0;
    slot_cfg.d1 = CORE_SD_PIN_D1;
    slot_cfg.d2 = CORE_SD_PIN_D2;
    slot_cfg.d3 = CORE_SD_PIN_D3;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retry mount SD (%d/3, %s, %d kHz)...",
                     attempt + 1, format_if_failed ? "format ok" : "no format",
                     max_freq_khz);
            core_sd_release();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ret = esp_vfs_fat_sdmmc_mount(CORE_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
        if (ret == ESP_OK) {
            break;
        }
        snprintf(s_last_mount_err, sizeof(s_last_mount_err), "%s", esp_err_to_name(ret));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD fallito: %s (freq=%d kHz)", esp_err_to_name(ret), max_freq_khz);
        core_sd_release();
        return false;
    }
    s_mounted = true;
    s_last_mount_err[0] = 0;
    ESP_LOGI(TAG, "SD montata su %s (%s, %d kHz)",
             CORE_SD_MOUNT_POINT, format_if_failed ? "format se richiesto" : "solo mount",
             max_freq_khz);
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

static bool core_sd_mount_try(bool format_if_failed) {
    if (core_sd_mount_once(format_if_failed, SDMMC_FREQ_HIGHSPEED)) {
        return true;
    }
    ESP_LOGW(TAG, "Mount high-speed fallito — retry a frequenza default");
    return core_sd_mount_once(format_if_failed, SDMMC_FREQ_DEFAULT);
}

static bool core_sd_mount_with_lock(bool format_if_failed) {
    if (s_mounted) {
        return true;
    }

    if (!s_mount_mtx) {
        s_mount_mtx = xSemaphoreCreateMutex();
        if (!s_mount_mtx) {
            return core_sd_mount_try(format_if_failed);
        }
    }

    if (xSemaphoreTake(s_mount_mtx, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex mount SD");
        return s_mounted;
    }

    bool ok = core_sd_mount_try(format_if_failed);
    xSemaphoreGive(s_mount_mtx);
    return ok;
}

static void core_sd_sync_mounted_flag(void) {
    if (hal_sd_probe_mount(CORE_SD_MOUNT_POINT)) {
        s_card = (sdmmc_card_t *)1;
        s_mounted = true;
    }
}

static void core_sd_try_remount_rw(void) {
    core_sd_sync_mounted_flag();
    if (!hal_sd_mount_is_readonly(CORE_SD_MOUNT_POINT) &&
        hal_sd_probe_write(CORE_SD_MOUNT_POINT)) {
        return;
    }
    ESP_LOGW(TAG, "SD non scrivibile — remount rw…");
    if (hal_sd_remount_rw(CORE_SD_MOUNT_POINT) &&
        hal_sd_probe_write(CORE_SD_MOUNT_POINT)) {
        ESP_LOGI(TAG, "SD di nuovo in scrittura");
        core_sd_sync_mounted_flag();
        return;
    }
    if (s_vfat_repair_used) {
        ESP_LOGE(TAG, "SD ancora non scrivibile (niente altro fsck in questo avvio)");
        return;
    }
    s_vfat_repair_used = true;
    ESP_LOGW(TAG, "Remount rw fallito — fsck.fat -a una volta…");
    if (hal_sd_repair_vfat(CORE_SD_MOUNT_POINT) &&
        hal_sd_probe_write(CORE_SD_MOUNT_POINT)) {
        ESP_LOGI(TAG, "SD riparata e montata in scrittura");
        core_sd_sync_mounted_flag();
    } else {
        ESP_LOGE(TAG, "SD ancora non scrivibile");
    }
}

bool core_sd_ensure_rw(void) {
    if (!s_mounted) {
        (void)core_sd_init();
    }
    core_sd_sync_mounted_flag();
    if (!hal_sd_mount_is_readonly(CORE_SD_MOUNT_POINT) &&
        hal_sd_probe_write(CORE_SD_MOUNT_POINT)) {
        s_mounted = true;
        return true;
    }
    core_sd_try_remount_rw();
    if (hal_sd_probe_write(CORE_SD_MOUNT_POINT)) {
        s_mounted = true;
        return true;
    }
    return false;
}

bool core_sd_init(void) {
    if (!core_sd_mount_with_lock(false)) {
        return false;
    }
    core_sd_try_remount_rw();
    return s_mounted;
}

bool core_sd_init_pir(void) {
    if (s_mounted) {
        core_sd_try_remount_rw();
        return true;
    }
    if (!s_mount_mtx) {
        s_mount_mtx = xSemaphoreCreateMutex();
        if (!s_mount_mtx) {
            return core_sd_mount_once(false, SDMMC_FREQ_HIGHSPEED);
        }
    }
    if (xSemaphoreTake(s_mount_mtx, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex mount SD PIR");
        return s_mounted;
    }
    // EncoderPrimaDellaSD: the encoder task is already running. Retry the
    // mount until the card node exists. Do not format. A 1970 filename is
    // acceptable (OrologioDopo — this path does not wait on NTP).
    bool ok = false;
    for (int n = 0; n < 40 && !s_mounted; ++n) {
        ok = core_sd_mount_once(false, SDMMC_FREQ_HIGHSPEED);
        if (ok) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xSemaphoreGive(s_mount_mtx);
    return ok || s_mounted;
}

bool core_sd_reinit(void) {
    if (!s_mount_mtx) {
        s_mount_mtx = xSemaphoreCreateMutex();
    }
    if (s_mount_mtx && xSemaphoreTake(s_mount_mtx, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex reinit SD");
        return s_mounted;
    }

    ESP_LOGI(TAG, "Reinit SD (scheda inserita a caldo o retry mount)...");
    core_sd_release();
    vTaskDelay(pdMS_TO_TICKS(CORE_SD_BOOT_RETRY_MS));
    bool ok = core_sd_mount_try(false);
    if (ok) {
        core_sd_try_remount_rw();
    }

    if (s_mount_mtx) {
        xSemaphoreGive(s_mount_mtx);
    }
    return ok;
}

// A 40 MHz un bus marginale (pull-up deboli, piste lunghe) produce errori CRC
// che il driver ritenta in silenzio: throughput che crolla a poche centinaia di
// KB/s. A 20 MHz lo stesso bus spesso è molto più veloce perché non ritenta.
bool core_sd_remount_default_freq(void) {
    if (!s_mount_mtx) {
        s_mount_mtx = xSemaphoreCreateMutex();
    }
    if (s_mount_mtx && xSemaphoreTake(s_mount_mtx, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex remount SD");
        return false;
    }
    core_sd_release();
    vTaskDelay(pdMS_TO_TICKS(200));
    bool ok = core_sd_mount_once(false, SDMMC_FREQ_DEFAULT);
    if (s_mount_mtx) {
        xSemaphoreGive(s_mount_mtx);
    }
    return ok;
}

bool core_sd_init_boot(void) {
    if (s_mounted) return true;

    ESP_LOGI(TAG, "Boot config: mount SD (%d tentativi max)...", CORE_SD_BOOT_RETRY_COUNT);
    for (int attempt = 0; attempt < CORE_SD_BOOT_RETRY_COUNT; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Boot config: retry SD (%d/%d)...", attempt + 1, CORE_SD_BOOT_RETRY_COUNT);
            vTaskDelay(pdMS_TO_TICKS(CORE_SD_BOOT_RETRY_MS));
        }
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Heap pre-mount: int=%u KB dma=%u KB psram=%u KB",
                 (unsigned)(free_int / 1024), (unsigned)(free_dma / 1024),
                 (unsigned)(free_psram / 1024));
        if (core_sd_init()) {
            return true;
        }
    }
    return false;
}

bool core_sd_is_mounted(void) {
    return s_mounted;
}

bool core_sd_format(void) {
    if (!s_mount_mtx) {
        s_mount_mtx = xSemaphoreCreateMutex();
    }
    if (s_mount_mtx && xSemaphoreTake(s_mount_mtx, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex format SD");
        return false;
    }

    core_sd_release();
    s_mounted = false;
    s_card = nullptr;
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGW(TAG, "Formattazione FAT32 della SD (mkfs.vfat)…");
    esp_err_t err = esp_vfs_fat_sdcard_format(CORE_SD_MOUNT_POINT, nullptr);
    bool ok = (err == ESP_OK);
    if (ok) {
        s_card = (sdmmc_card_t *)1;
        s_mounted = true;
        core_sd_try_remount_rw();
        ok = !hal_sd_mount_is_readonly(CORE_SD_MOUNT_POINT);
    } else {
        ESP_LOGE(TAG, "Format SD fallito: %s", esp_err_to_name(err));
    }

    if (s_mount_mtx) {
        xSemaphoreGive(s_mount_mtx);
    }
    if (ok) {
        ESP_LOGI(TAG, "SD formattata e montata in scrittura");
    }
    return ok;
}

uint64_t core_sd_free_bytes(void) {
    if (!s_mounted) return 0;
    uint64_t total = 0, free_b = 0;
    esp_err_t err = esp_vfs_fat_info(CORE_SD_MOUNT_POINT, &total, &free_b);
    if (err != ESP_OK) return 0;
    return free_b;
}

static bool day_dir_name_valid(const char *name) {
    if (!name || strlen(name) != 8) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool mp4_recording_name(const char *name) {
    if (!name || name[0] == '.') {
        return false;
    }
    if (strstr(name, ".mp4.tmp") != nullptr) {
        return false;
    }
    size_t len = strlen(name);
    if (len >= 5 && strcmp(name + len - 5, ".part") == 0) {
        return false;
    }
    return len >= 5 && strcmp(name + len - 4, ".mp4") == 0;
}

// Nome sessione già usato se esiste .mp4, _bench.mp4 o _bench_part01.mp4 (benchmark PIR).
static bool recording_path_group_exists(const char *mp4_path) {
    if (!mp4_path) {
        return false;
    }
    struct stat st;
    if (stat(mp4_path, &st) == 0) {
        return true;
    }
    char variant[192];
    const char *dot = strrchr(mp4_path, '.');
    if (!dot || strcmp(dot, ".mp4") != 0) {
        return false;
    }
    size_t base_len = (size_t)(dot - mp4_path);
    if (base_len + 20 >= sizeof(variant)) {
        return false;
    }
    memcpy(variant, mp4_path, base_len);
    variant[base_len] = '\0';
    const char *suffixes[] = {
        "_part01.mp4", "_bench.mp4", "_bench_part01.mp4",
        "_foto0001.jpg", "_p0001.jpg",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(variant + base_len, sizeof(variant) - base_len, "%s", suffixes[i]);
        if (stat(variant, &st) == 0) {
            return true;
        }
    }
    return false;
}

// Libera spazio eliminando una singola registrazione .mp4 (la più vecchia),
// non l'intera cartella giorno: altrimenti con tutti i video dello stesso
// giorno sparirebbero in blocco prima della nuova registrazione.
static bool delete_oldest_mp4_file(void) {
    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        return false;
    }

    char oldest_rel[120] = {0};
    struct dirent *day;
    while ((day = readdir(root)) != nullptr) {
        if (!day_dir_name_valid(day->d_name)) {
            continue;
        }
        char daypath[64];
        snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat dst;
        if (stat(daypath, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            continue;
        }

        DIR *subdir = opendir(daypath);
        if (!subdir) {
            continue;
        }
        struct dirent *f;
        while ((f = readdir(subdir)) != nullptr) {
            if (!mp4_recording_name(f->d_name)) {
                continue;
            }
            char rel[120];
            snprintf(rel, sizeof(rel), "%.8s/%.80s", day->d_name, f->d_name);
            if (oldest_rel[0] == 0 || strcmp(rel, oldest_rel) < 0) {
                strncpy(oldest_rel, rel, sizeof(oldest_rel) - 1);
                oldest_rel[sizeof(oldest_rel) - 1] = 0;
            }
        }
        closedir(subdir);
    }
    closedir(root);

    if (oldest_rel[0] == 0) {
        return false;
    }

    char full[192];
    snprintf(full, sizeof(full), "%s/%s", CORE_SD_MOUNT_POINT, oldest_rel);
    ESP_LOGW(TAG, "Spazio SD: elimino registrazione più vecchia %s", full);
    if (remove(full) != 0) {
        ESP_LOGE(TAG, "remove %s fallito (errno=%d)", full, errno);
        return false;
    }

    char daypath[64];
    snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, oldest_rel);
    DIR *day_dir = opendir(daypath);
    if (day_dir) {
        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(day_dir)) != nullptr) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                empty = false;
                break;
            }
        }
        closedir(day_dir);
        if (empty) {
            rmdir(daypath);
        }
    }
    return true;
}

bool core_sd_ensure_space(uint64_t min_free) {
    if (!s_mounted && !core_sd_init()) return false;
    int guard = 0;
    while (core_sd_free_bytes() < min_free && guard < 128) {
        if (!delete_oldest_mp4_file()) break;
        guard++;
    }
    uint64_t free_b = core_sd_free_bytes();
    if (free_b < min_free) {
        ESP_LOGE(TAG, "Spazio insufficiente: %llu bytes liberi, richiesti %llu",
                 (unsigned long long)free_b, (unsigned long long)min_free);
        return false;
    }
    return true;
}

/* Prefer DS3231; system clock is often still 1970 at PIR/CONFIG boot. */
static bool core_sd_local_tm(struct tm *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (core_rtc_get_local_tm(out) && out->tm_year + 1900 >= 2020) {
        return true;
    }
    /* Best-effort: pull chip → system, then re-read. */
    if (core_rtc_sync_system_time() && core_rtc_get_local_tm(out) &&
        out->tm_year + 1900 >= 2020) {
        return true;
    }
    time_t now = time(nullptr);
    if (localtime_r(&now, out) && out->tm_year + 1900 >= 2020) {
        return true;
    }
    ESP_LOGW(TAG, "Ora non valida per nome file (RTC/sistema < 2020) — uso comunque tm");
    return localtime_r(&now, out) != nullptr;
}

bool core_sd_make_day_dir(char *out_path, size_t out_len) {
    if (!out_path || out_len < 16) return false;
    struct tm t;
    if (!core_sd_local_tm(&t)) {
        return false;
    }
    char day[9];
    snprintf(day, sizeof(day), "%02d%02d%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    if (out_len < strlen(CORE_SD_MOUNT_POINT) + 1 + strlen(day) + 1) return false;
    snprintf(out_path, out_len, "%s/%s", CORE_SD_MOUNT_POINT, day);
    struct stat st;
    if (stat(out_path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    if (mkdir(out_path, 0755) == 0) return true;
    ESP_LOGE(TAG, "mkdir %s fallito: errno=%d", out_path, errno);
    return false;
}

// Nome file: {CORE_ID}_{seq}_{YYYYMMDD}_{HHMMSS}.mp4 — dataora da RTC/sistema.
// Segmenti: {CORE_ID}_{seq}_{YYYYMMDD}_{HHMMSS}_part01.mp4, …
// Timelapse foto: stesso base + _foto0001.jpg
bool core_sd_make_recording_path(const char *day_dir, uint32_t core_id,
                                 char *out_path, size_t out_len) {
    if (!day_dir || !out_path || out_len < 56) {
        return false;
    }

    struct tm t;
    if (!core_sd_local_tm(&t)) {
        return false;
    }
    char ymd[9];
    char hhmmss[8];
    snprintf(ymd, sizeof(ymd), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    snprintf(hhmmss, sizeof(hhmmss), "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);

    uint16_t start_seq = CORE_REC_FILE_SEQ_MIN;
    if (!core_settings_rec_file_seq_peek(&start_seq)) {
        start_seq = CORE_REC_FILE_SEQ_MIN;
    }

    for (unsigned attempt = 0; attempt < CORE_REC_FILE_SEQ_COUNT; ++attempt) {
        uint16_t seq = (uint16_t)((start_seq + attempt) % CORE_REC_FILE_SEQ_COUNT);
        int n = snprintf(out_path, out_len, "%s/%05lu_%03u_%s_%s.mp4",
                         day_dir, (unsigned long)core_id, (unsigned)seq, ymd, hhmmss);
        if (n <= 0 || (size_t)n >= out_len) {
            return false;
        }
        if (!recording_path_group_exists(out_path)) {
            uint16_t next_seq = (seq >= CORE_REC_FILE_SEQ_MAX) ? CORE_REC_FILE_SEQ_MIN
                                                               : (uint16_t)(seq + 1);
            if (!core_settings_rec_file_seq_commit(next_seq)) {
                ESP_LOGW(TAG, "Commit seq NVS fallito (prossima %03u)", (unsigned)next_seq);
            }
            if (attempt > 0) {
                ESP_LOGW(TAG, "Seq %03u occupata su SD, uso %03u", (unsigned)start_seq,
                         (unsigned)seq);
            }
            ESP_LOGI(TAG, "Path registrazione: %s (seq %03u, %s_%s, prossima %03u)",
                     out_path, (unsigned)seq, ymd, hhmmss, (unsigned)next_seq);
            return true;
        }
    }

    ESP_LOGE(TAG, "Nessuna seq libera 000–999 in %s", day_dir);
    return false;
}

static bool rec_meta_path_from_media(const char *media_path_or_base, char *out, size_t out_len) {
    if (!media_path_or_base || !out || out_len < 8) {
        return false;
    }
    const char *dot = strrchr(media_path_or_base, '.');
    if (dot && (strcmp(dot, ".mp4") == 0 || strcmp(dot, ".avi") == 0 ||
                strcmp(dot, ".jpg") == 0 || strcmp(dot, ".meta") == 0)) {
        size_t base_len = (size_t)(dot - media_path_or_base);
        if (base_len + 6 >= out_len) {
            return false;
        }
        memcpy(out, media_path_or_base, base_len);
        out[base_len] = '\0';
        strncat(out, ".meta", out_len - base_len - 1);
        return true;
    }
    int n = snprintf(out, out_len, "%s.meta", media_path_or_base);
    return n > 0 && (size_t)n < out_len;
}

bool core_sd_write_rec_meta(const char *media_path_or_base, uint32_t duration_sec, uint32_t frames,
                            uint32_t fps) {
    char meta_path[192];
    if (!rec_meta_path_from_media(media_path_or_base, meta_path, sizeof(meta_path))) {
        return false;
    }
    FILE *fp = fopen(meta_path, "w");
    if (!fp) {
        ESP_LOGW(TAG, "Meta write fallita %s (errno=%d)", meta_path, errno);
        return false;
    }
    if (fps > 0) {
        fprintf(fp, "%lu %lu %lu\n", (unsigned long)duration_sec, (unsigned long)frames,
                (unsigned long)fps);
    } else {
        fprintf(fp, "%lu %lu\n", (unsigned long)duration_sec, (unsigned long)frames);
    }
    fclose(fp);
    return true;
}

bool core_sd_read_rec_meta(const char *media_path_or_base, uint32_t *duration_sec, uint32_t *frames,
                           uint32_t *fps) {
    if (duration_sec) {
        *duration_sec = 0;
    }
    if (frames) {
        *frames = 0;
    }
    if (fps) {
        *fps = 0;
    }
    char meta_path[192];
    if (!rec_meta_path_from_media(media_path_or_base, meta_path, sizeof(meta_path))) {
        return false;
    }
    FILE *fp = fopen(meta_path, "r");
    if (!fp) {
        return false;
    }
    unsigned long d = 0;
    unsigned long f = 0;
    unsigned long p = 0;
    int n = fscanf(fp, "%lu %lu %lu", &d, &f, &p);
    fclose(fp);
    if (n < 1) {
        return false;
    }
    if (duration_sec) {
        *duration_sec = (uint32_t)d;
    }
    if (frames && n >= 2) {
        *frames = (uint32_t)f;
    }
    if (fps && n >= 3) {
        *fps = (uint32_t)p;
    }
    return true;
}
