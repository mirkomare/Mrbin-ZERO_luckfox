#include "CoreAesUpload.h"
#include "CoreAuth.h"
#include "CoreClipQueue.h"
#include "CoreConfig.h"
#include "CoreDemo.h"
#include "CoreGPIO.h"
#include "CoreIspCal.h"
#include "CoreMrbinId.h"
#include "CoreRecorder.h"
#include "CoreRtsp.h"
#include "CoreRtc.h"
#include "CoreAlarmSchedule.h"
#include "CoreSD.h"
#include "CoreSettings.h"
#include "CoreStatusLed.h"
#include "CoreTime.h"
#include "CoreVideo.h"
#include "CoreWeb.h"
#include "hal_net.h"

#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mrbin_core";
static core_settings_t g_settings;
static bool s_force_config = false;
static bool s_rec_fast = false;
static bool s_cli_pir = false;
static int s_first_enc_logged = 0;
static int s_first_file_logged = 0;
static int64_t s_core_main_us = 0;
static int64_t s_pir_decision_ms = -1;

static bool env_truthy(const char *v) {
    if (!v || !v[0]) {
        return false;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return false;
    }
    if (v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F') {
        return false;
    }
    return true;
}

static bool pins_env_force_config(void) {
    const char *paths[] = { "/etc/mrbin/pins.env", "overlay/etc/mrbin/pins.env", nullptr };
    FILE *fp = nullptr;
    for (int i = 0; paths[i]; ++i) {
        fp = fopen(paths[i], "r");
        if (fp) {
            break;
        }
    }
    if (!fp) {
        return false;
    }
    char line[160];
    bool yes = false;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        char key[64];
        char val[64];
        if (sscanf(line, " %63[A-Za-z0-9_]=%63s", key, val) != 2) {
            continue;
        }
        if (strcmp(key, "FORCE_CONFIG") == 0 || strcmp(key, "MRBIN_FORCE_CONFIG") == 0) {
            yes = env_truthy(val);
        }
    }
    fclose(fp);
    return yes;
}

static bool core_force_config_requested(void) {
    /* Explicit --force-config / env always wins (do not break CONFIG). */
    if (env_truthy(getenv("MRBIN_FORCE_CONFIG")) || env_truthy(getenv("FORCE_CONFIG"))) {
        return true;
    }
    /* --pir from S00/S01: do not let pins.env FORCE_CONFIG override CLI rec. */
    if (env_truthy(getenv("MRBIN_PIR"))) {
        return false;
    }
    return pins_env_force_config();
}

int64_t g_core_boot_us = 0;

int64_t core_boot_elapsed_ms(void) {
    /* Linux CLOCK_MONOTONIC epoch is kernel boot (closest to power-on
     * without MCU/U-Boot stamps). g_core_boot_us==0 → elapsed = now. */
    return (esp_timer_get_time() - g_core_boot_us) / 1000;
}

int64_t core_main_elapsed_ms(void) {
    if (s_core_main_us <= 0) {
        return 0;
    }
    return (esp_timer_get_time() - s_core_main_us) / 1000;
}

static void load_pir_decision_stamp(void) {
    FILE *fp = fopen("/tmp/mrbin-pir-decision", "r");
    if (!fp) {
        return;
    }
    long v = 0;
    if (fscanf(fp, "%ld", &v) == 1 && v >= 0) {
        s_pir_decision_ms = (int64_t)v;
    }
    fclose(fp);
}

int64_t core_since_pir_decision_ms(void) {
    const int64_t now = core_boot_elapsed_ms();
    if (s_pir_decision_ms < 0) {
        return core_main_elapsed_ms();
    }
    int64_t d = now - s_pir_decision_ms;
    return d < 0 ? 0 : d;
}

static void pir_time_line(const char *kind, const char *where, unsigned long nbytes) {
    const int64_t kern = core_boot_elapsed_ms();
    const int64_t main_ms = core_main_elapsed_ms();
    const int64_t dec = core_since_pir_decision_ms();
    const char *src = where ? where : "?";
    ESP_LOGI(TAG, "PIRTIME %s %s %lu byte kernel_ms=%lld main_ms=%lld decision_ms=%lld",
             kind ? kind : "?", src, nbytes,
             (long long)kern, (long long)main_ms, (long long)dec);
    FILE *fp = fopen("/tmp/mrbin-boot-time.log", "a");
    if (!fp) {
        return;
    }
    fprintf(fp, "PIRTIME %s %s %lu kernel_ms=%lld main_ms=%lld decision_ms=%lld\n",
            kind ? kind : "?", src, nbytes,
            (long long)kern, (long long)main_ms, (long long)dec);
    fclose(fp);
}

static int pir_bench_sec(void) {
    const char *e = getenv("MRBIN_PIR_BENCH_SEC");
    if (!e || !e[0]) {
        return 0;
    }
    int n = atoi(e);
    if (n < 0) {
        return 0;
    }
    if (n > 15) {
        n = 15;
    }
    return n;
}

bool core_rec_fast_requested(void) {
    return s_rec_fast || env_truthy(getenv("MRBIN_REC_FAST")) ||
           env_truthy(getenv("MRBIN_PIR"));
}

void core_note_first_encoded_frame(const char *where, unsigned long nbytes) {
    if (s_first_enc_logged) {
        return;
    }
    s_first_enc_logged = 1;
    pir_time_line("encoded", where, nbytes);
    /* Budget is from the pin decision (S00), not from power-on.
     * Kernel time before userspace is logged separately and is not the budget. */
    const int64_t judge = core_since_pir_decision_ms();
    const char *src = where ? where : "encoder";
    if (judge > (int64_t)CORE_REC_START_BUDGET_MS) {
        ESP_LOGE(TAG,
                 "SUPERFASTBOOT FAIL: first encoded frame (%s, %lu byte) %lld ms after PIR decision (budget %d ms, kernel %lld ms)",
                 src, nbytes, (long long)judge, CORE_REC_START_BUDGET_MS,
                 (long long)core_boot_elapsed_ms());
    } else {
        ESP_LOGI(TAG,
                 "SUPERFASTBOOT OK: first encoded frame (%s, %lu byte) %lld ms after PIR decision (budget %d ms, kernel %lld ms)",
                 src, nbytes, (long long)judge, CORE_REC_START_BUDGET_MS,
                 (long long)core_boot_elapsed_ms());
    }
}

void core_note_first_file_bytes(const char *where, unsigned long nbytes) {
    if (s_first_file_logged) {
        return;
    }
    s_first_file_logged = 1;
    pir_time_line("file", where, nbytes);
}

// PIR/registrazione: solo sync ora + schedule (niente clear A1F, log, setup chip).
static void boot_rtc_recording_only(void) {
    core_rtc_set_hw_status_monitoring(false);
    if (!core_rtc_is_present()) {
        return;
    }
    if (!core_rtc_sync_system_time()) {
        ESP_LOGW(TAG, "DS3231 presente ma ora non letta — profilo colore predefinito NVS");
    }
    core_alarm_schedule_init();
    core_alarm_schedule_on_boot();
}

// Programmazione (GPIO29 HIGH o wake RTC): setup chip, clear A1F, storico, sync.
static void boot_rtc_programming_full(void) {
    core_rtc_set_hw_status_monitoring(true);
    if (!core_rtc_handle_programming_boot()) {
        ESP_LOGI(TAG, "DS3231 assente — allarmi e fasce orarie ignorate, profilo ISP predefinito");
        return;
    }
    if (!core_rtc_sync_system_time()) {
        ESP_LOGW(TAG, "DS3231 presente ma ora non letta — profilo colore predefinito NVS");
    }
    core_alarm_schedule_on_boot();
}

static void log_wake_source(bool config_mode, bool rtc_programming) {
    if (rtc_programming) {
        if (core_rtc_boot_was_alarm_wake()) {
            ESP_LOGI(TAG, "Wake: RTC allarme DS3231 — sync upload (GPIO5+WiFi+ping), MODE GPIO%d ignorato",
                     (int)CORE_GPIO_MODE_CFG);
        } else {
            ESP_LOGI(TAG, "Wake: SQW LOW GPIO%d — sync upload (anche senza RTC)",
                     (int)CORE_GPIO_RTC_SQW);
        }
        return;
    }
    if (config_mode) {
        ESP_LOGI(TAG, "Wake: CONFIG (GPIO%d MODE HIGH)", (int)CORE_GPIO_MODE_CFG);
        return;
    }
    if (core_gpio_boot_was_pir_wake()) {
        ESP_LOGI(TAG, "Wake: PIR/movimento");
        return;
    }
    ESP_LOGI(TAG, "Wake: TPL (non PIR, non SQW, non RTC)");
}

static void log_reset_reason(void) {
    esp_reset_reason_t r = esp_reset_reason();
    const char *s = "?";
    switch (r) {
        case ESP_RST_POWERON:   s = "POWERON (alimentazione applicata / TPL on)"; break;
        case ESP_RST_EXT:       s = "EXT (reset esterno pin)"; break;
        case ESP_RST_SW:        s = "SW (reset software)"; break;
        case ESP_RST_PANIC:     s = "PANIC (crash firmware!)"; break;
        case ESP_RST_INT_WDT:   s = "INT_WDT (watchdog interrupt!)"; break;
        case ESP_RST_TASK_WDT:  s = "TASK_WDT (watchdog task!)"; break;
        case ESP_RST_WDT:       s = "WDT (altro watchdog!)"; break;
        case ESP_RST_BROWNOUT:  s = "BROWNOUT (tensione caduta sotto soglia!)"; break;
        case ESP_RST_DEEPSLEEP: s = "DEEPSLEEP"; break;
        case ESP_RST_SDIO:      s = "SDIO"; break;
        default:                s = "UNKNOWN"; break;
    }
    ESP_LOGW(TAG, "Reset reason ultimo avvio: %s (code=%d)", s, (int)r);
}

static void disable_radio_for_recording(void) {
    hal_wifi_allow(false);
    hal_wifi_radio_down();
#if CONFIG_ESP_HOSTED_ENABLED
    int64_t t0 = esp_timer_get_time();
    esp_hosted_deinit();
    core_sd_set_hosted_owns_host(false);
    ESP_LOGI(TAG, "ESP-Hosted (WiFi C6) spento in %lld ms — bus SDMMC dedicato alla SD",
             (long long)((esp_timer_get_time() - t0) / 1000));
#else
    ESP_LOGI(TAG, "PIR: wlan0 down, niente scan né associazione");
#endif
}

static void recover_tmp_task(void *arg) {
    core_settings_t *settings = (core_settings_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(CORE_CONFIG_RECOVER_TMP_DELAY_MS));
    core_recorder_process_pending_encrypt(settings);
    for (;;) {
        core_recorder_recover_tmp_files(settings);
        vTaskDelay(pdMS_TO_TICKS(CORE_REC_TMP_SWEEP_MS));
    }
}

static volatile bool s_mrbin_upload_cancel = false;

static void mrbin_upload_task(void *arg) {
    (void)arg;
    const uint32_t wait_ms = 45000;
    uint32_t waited = 0;
    while (!s_mrbin_upload_cancel && !core_web_sta_has_ip() && waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    if (s_mrbin_upload_cancel) {
        vTaskDelete(nullptr);
        return;
    }
    if (!core_web_sta_has_ip()) {
        ESP_LOGI(TAG, "MrBin upload: niente STA — skip (AP o rete assente)");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "MrBin upload: STA up — enroll + coda clip");
    core_clip_queue_process(&s_mrbin_upload_cancel);
    vTaskDelete(nullptr);
}

// Wake RTC / GPIO46 LOW: GPIO5 HIGH (router) → WiFi → ping server → upload coda → DONE.
// Non resta in Web GUI. Il jumper MODE HIGH continua a usare run_config_mode().
static void run_sync_mode(void) {
    hal_wifi_allow(true);
    core_gpio_rtc_programming_begin();  // GPIO46 LOW, poi GPIO5 HIGH
    ESP_LOGI(TAG, "Sync: GPIO%d HIGH (router) prima del WiFi", CORE_GPIO_MODEM_PWR);

    if (g_settings.wifi_ssid[0] == 0) {
        ESP_LOGE(TAG, "Sync: SSID vuoto — impossibile raggiungere il server, DONE");
        core_gpio_rtc_programming_end();
        core_alarm_schedule_on_shutdown(false);
        core_gpio_hold_tpl_done(0);
        return;
    }

    const bool web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_STA, nullptr);
    core_gpio_reapply_after_hosted();
    core_gpio_modem_pwr_set(true);
    if (!web_ok) {
        ESP_LOGE(TAG, "Sync: WiFi STA non avviato — DONE");
        core_gpio_rtc_programming_end();
        core_alarm_schedule_on_shutdown(false);
        core_gpio_hold_tpl_done(0);
        return;
    }

    core_recorder_recover_tmp_files(&g_settings);
    core_recorder_process_pending_encrypt(&g_settings);

    volatile bool cancel = false;
    ESP_LOGI(TAG, "Sync: ping %s finché il router risponde",
             core_mrbin_id_get() && core_mrbin_id_get()->api_base_url[0]
                 ? core_mrbin_id_get()->api_base_url
                 : CORE_MRBIN_API_BASE_DEFAULT);
    if (!core_mrbin_wait_server_online(CORE_MRBIN_SYNC_TIMEOUT_MS, &cancel)) {
        ESP_LOGW(TAG, "Sync: server non raggiungibile — coda resta su SD, DONE");
    } else {
        ESP_LOGI(TAG, "Sync: server online — upload di tutte le clip in coda");
        core_clip_queue_process(&cancel);
        if (core_mrbin_config_pull()) {
            ESP_LOGI(TAG, "Sync: configurazione remota applicata");
        }
    }

    ESP_LOGI(TAG, "Sync: fine — D10 HIGH / DONE TPL");
    core_web_stop();
    core_gpio_rtc_programming_end();
    core_alarm_schedule_on_shutdown(true);
    core_gpio_hold_tpl_done(0);
}

static void run_config_mode(void) {
    hal_wifi_allow(true);
    core_gpio_sqw_drive_low();      // HUB ON anche in CONFIG da jumper MODE
    core_gpio_modem_pwr_set(true);  // GPIO5 3.3 V
    ESP_LOGI(TAG, "GPIO%d HIGH — configurazione (WiFi/Web), GPIO46 LOW (HUB) modem GPIO%d ON",
             CORE_GPIO_MODE_CFG, CORE_GPIO_MODEM_PWR);
    if (s_force_config) {
        // MODE is typically LOW when forced — do not exit until HIGH then LOW.
        core_gpio_mode_exit_session_begin_rtc();
    } else {
        core_gpio_mode_exit_session_begin();
    }

    bool web_ok = false;
    if (g_settings.wifi_ssid[0] != 0) {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_STA, nullptr);
    } else {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_AP, nullptr);
    }

    if (web_ok) {
        ESP_LOGI(TAG, "Web GUI attiva — GPIO%d LOW stabile → spegnimento (DONE TPL)", CORE_GPIO_MODE_CFG);
    } else {
        ESP_LOGW(TAG, "Web GUI non avviata — GPIO%d LOW → spegnimento", CORE_GPIO_MODE_CFG);
    }
    // Ri-applica dopo ESP-Hosted (SDIO/WiFi può toccare pad VDD_IO_6).
    core_gpio_reapply_after_hosted();
    core_gpio_modem_pwr_set(true);

    xTaskCreate(recover_tmp_task, "recov_tmp", 8192, &g_settings, 2, nullptr);
    s_mrbin_upload_cancel = false;
    xTaskCreate(mrbin_upload_task, "mb_up", 16384, nullptr, 3, nullptr);

    if (core_gpio_is_demo_jumper()) {
        core_rtsp_hold();
    }
    core_rtsp_start(&g_settings);
    core_demo_config_start(&g_settings);

    int64_t t0 = esp_timer_get_time();
    int64_t last_beat = t0;
    while (!core_gpio_is_mode_exit_triggered()) {
        vTaskDelay(pdMS_TO_TICKS(CORE_MODE_POLL_MS));

        int64_t now = esp_timer_get_time();
        if ((now - last_beat) >= 5000000) {
            last_beat = now;
            uint32_t up_s = (uint32_t)((now - t0) / 1000000);
            ESP_LOGI(TAG, "Config attiva: uptime %lu s (MODE=%d)",
                     (unsigned long)up_s, gpio_get_level(CORE_GPIO_MODE_CFG));
        }
    }

    ESP_LOGI(TAG, "Uscita config — stop Web/Live e loop DONE TPL");
    s_mrbin_upload_cancel = true;
    vTaskDelay(pdMS_TO_TICKS(200));
    core_web_stop();
    core_gpio_mode_exit_session_end();
    core_gpio_modem_pwr_set(false);
    core_gpio_sqw_release_high();
    // CONFIG: ora ESP tipicamente aggiornata via SNTP → ok scrivere sul chip.
    core_alarm_schedule_on_shutdown(true);
    core_gpio_hold_tpl_done(0);
}

static void run_pir_mode(void) {
    core_gpio_boot_snapshot_t snap;
    core_gpio_get_boot_snapshot(&snap);
    log_reset_reason();
    ESP_LOGI(TAG, "PIR boot (%lld ms) — D1=%d D2=%d, start GPIO%d stop GPIO%d, media=%s",
             (long long)core_boot_elapsed_ms(),
             snap.d1_level, snap.d2_level,
             (int)g_settings.rec_gpio_start, (int)g_settings.rec_gpio_stop,
             g_settings.rec_media_mode == CORE_REC_MEDIA_PHOTO ? "photo" : "video");

    disable_radio_for_recording();
    /* Rec-fast opens VI inside the recorder. A CIF pre-open here only
     * burns time before the first EnableChn. */
    if (!core_rec_fast_requested() && !core_video_is_ready()) {
        if (core_video_init()) {
            ESP_LOGI(TAG, "PIR: camera pre-init (%lld ms)",
                     (long long)core_boot_elapsed_ms());
        } else {
            ESP_LOGW(TAG, "PIR: pre-init camera fallita — retry in recorder");
        }
    }

    // Una sola sessione per wake, poi DONE (timer o pin STOP chiudono la rec).
    (void)core_recorder_run_session(&g_settings);

    // Chiudi camera prima dell'I2C RTC: dopo rec lunga il bus I2C1 a volte va in
    // timeout HW se CSI/ISP sono ancora attivi (allarme non programmato).
    core_video_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));

    if (pir_bench_sec() > 0) {
        ESP_LOGI(TAG, "PIR bench — stop senza DONE (decision %lld ms, main %lld ms)",
                 (long long)core_since_pir_decision_ms(),
                 (long long)core_main_elapsed_ms());
        return;
    }

    // PIR: non scrivere ora ESP→DS3231 (l'ESP può aver driftato durante la sessione).
    core_alarm_schedule_on_shutdown(false);
    core_gpio_hold_tpl_done(g_settings.d2_post_delay_ms);
}

extern "C" void app_main(void) {
#ifdef _WIN32
    g_core_boot_us = esp_timer_get_time();
#else
    /* CLOCK_MONOTONIC since kernel boot ≈ power-on minus U-Boot. */
    g_core_boot_us = 0;
#endif
    s_core_main_us = esp_timer_get_time();
    /* Fino al routing: radio spenta. CONFIG e RTC la riaccendono. */
    hal_wifi_allow(false);
    hal_wifi_radio_down();
    load_pir_decision_stamp();
    s_cli_pir = env_truthy(getenv("MRBIN_PIR"));
    s_rec_fast = env_truthy(getenv("MRBIN_REC_FAST")) || s_cli_pir;
    ESP_LOGI(TAG, "Boot clock %lld ms since kernel main_ms=0 decision_ms=%lld (rec-fast=%d pir=%d bench=%d)",
             (long long)core_boot_elapsed_ms(),
             (long long)core_since_pir_decision_ms(),
             (int)s_rec_fast, (int)s_cli_pir, pir_bench_sec());
    {
        FILE *fp = fopen("/tmp/mrbin-boot-time.log", "a");
        if (fp) {
            fprintf(fp, "main kernel_ms=%lld decision_ms=%lld rec_fast=%d pir=%d bench=%d\n",
                    (long long)core_boot_elapsed_ms(),
                    (long long)core_since_pir_decision_ms(),
                    (int)s_rec_fast, (int)s_cli_pir, pir_bench_sec());
            fclose(fp);
        }
    }

    // Il banco VDD_IO_5 (GPIO39-48, quindi SQW GPIO46) è alimentato da LDO_VO4.
    // Finché resta a 0 V quei pad non pilotano e la linea segue i pull-up esterni:
    // va acceso prima di toccare qualsiasi GPIO, non al mount della SD.
    core_sd_power_io_early();

    if (!core_settings_init()) {
        ESP_LOGE(TAG, "NVS init fallita");
        return;
    }
    core_auth_init();
    core_settings_load(&g_settings);
    core_settings_ensure_id(&g_settings);
    core_settings_ensure_aes_key(&g_settings);
    (void)core_mrbin_id_init();
    core_mrbin_id_set_defaults(g_settings.core_id);

    s_force_config = core_force_config_requested();
    if (!core_gpio_init()) {
        ESP_LOGE(TAG, "GPIO init fallita");
        if (!s_force_config) {
            return;
        }
        /* A bad pad must not hide the GUI. Config was requested explicitly. */
        ESP_LOGW(TAG, "GPIO init fallita — la GUI :%d parte lo stesso", CORE_WEB_PORT);
    } else {
        core_gpio_pin_selftest();  // no-op salvo CORE_GPIO_PIN_SELFTEST = 1
        core_gpio_dump_pads("post gpio_init");

        // Snapshot D1/D2/MODE/SQW subito (SQW prima di clear A1F in programmazione).
        core_gpio_save_boot_snapshot();
        core_gpio_set_rec_pins(g_settings.rec_gpio_start, g_settings.rec_gpio_stop);
    }

    /* Rec-fast / --pir: skip I2C settle + RTC routing. Camera first. */
    if (s_rec_fast || s_cli_pir) {
        ESP_LOGI(TAG, "Rec-fast boot — skip RTC/SQW routing, pre-open camera");
        core_video_set_bayer_mode(g_settings.video_bayer_mode);
        core_video_set_flip(g_settings.video_vflip, g_settings.video_hflip);
        (void)core_status_led_init();
        log_wake_source(false, false);
        ESP_LOGI(TAG, "MrBin CORE avvio — modalità registrazione (rec-fast)");
        run_pir_mode();
        return;
    }

    // Latch A1F PRIMA di ogni clear/setup — serve a STATUS-BOOT su Stato HW e al routing.
    core_time_init();
    vTaskDelay(pdMS_TO_TICKS(25));  // DS3231/I2C stabili dopo accensione TPL
    if (!core_rtc_probe_wake_routing()) {
        core_rtc_reset_probe_for_retry();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!core_rtc_probe_wake_routing()) {
            ESP_LOGI(TAG, "DS3231 assente — wake RTC/allarmi non disponibili");
        }
    }
    ESP_LOGI(TAG, "Boot A1F latch: present=%d A1F_wake=%d (Stato HW STATUS-BOOT)",
             core_rtc_is_present() ? 1 : 0, core_rtc_boot_was_alarm_wake() ? 1 : 0);

    if (s_force_config) {
        core_demo_latch_from_boot(true);
        /* Solo skip routing SQW/PIR — A1F già latched sopra per STATUS-BOOT. */
        ESP_LOGW(TAG, "FORCE_CONFIG — skip SQW/PIR routing, avvio CONFIG con Stato HW");
        boot_rtc_programming_full();
        ESP_LOGI(TAG, "Boot init (%lld ms)", (long long)core_boot_elapsed_ms());
        core_isp_cal_init();
        core_alarm_schedule_init();
        core_video_set_bayer_mode(g_settings.video_bayer_mode);
        core_video_set_flip(g_settings.video_vflip, g_settings.video_hflip);
        core_video_detect_sensor();
        if (!core_status_led_init()) {
            ESP_LOGW(TAG, "LED GPIO%d non disponibile — feedback visivo disabilitato",
                     CORE_GPIO_STATUS_LED);
        }
        ESP_LOGI(TAG, "MrBin CORE avvio — modalità CONFIG (FORCE_CONFIG)");
        log_reset_reason();
        core_gpio_log_inputs();
        run_config_mode();
        return;
    }

    // MODE HIGH vince su A1F/SQW→sync: altrimenti la sveglia RTC azzera A1F in sync
    // (senza GUI) e in CONFIG successivo STATUS-BOOT resta CLEAR (0) con solo il log NVS.
    const bool a1f_wake = core_rtc_boot_was_alarm_wake();
    const bool sqw_wake = core_gpio_boot_was_sqw_low();
    const bool config_mode = core_gpio_is_mode_config();
    ESP_LOGI(TAG, "Routing boot: SQW=%d A1F_wake=%d MODE=%d",
             sqw_wake ? 1 : 0, a1f_wake ? 1 : 0, config_mode ? 1 : 0);

    if (config_mode) {
        core_demo_latch_from_boot(true);
        boot_rtc_programming_full();
        ESP_LOGI(TAG, "Boot init (%lld ms)", (long long)core_boot_elapsed_ms());
        core_isp_cal_init();
        core_alarm_schedule_init();
        core_video_set_bayer_mode(g_settings.video_bayer_mode);
        core_video_set_flip(g_settings.video_vflip, g_settings.video_hflip);
        log_wake_source(true, false);
        if (a1f_wake) {
            ESP_LOGI(TAG, "CONFIG + A1F: STATUS-BOOT A1F=SET (1), clear già registrato");
        }
        if (!core_status_led_init()) {
            ESP_LOGW(TAG, "LED GPIO%d non disponibile — feedback visivo disabilitato",
                     CORE_GPIO_STATUS_LED);
        }
        ESP_LOGI(TAG, "MrBin CORE avvio — modalità CONFIG%s",
                 core_demo_is_active() ? " (DEMO)" : "");
        log_reset_reason();
        core_gpio_log_inputs();
        run_config_mode();
        return;
    }

    // MODE LOW: RTC (A1F) o SQW LOW → sync upload poi DONE (niente GUI).
    if (sqw_wake || a1f_wake) {
        core_gpio_rtc_programming_begin();
        boot_rtc_programming_full();
        ESP_LOGI(TAG, "Boot init (%lld ms)", (long long)core_boot_elapsed_ms());
        core_isp_cal_init();
        core_alarm_schedule_init();
        core_video_set_bayer_mode(g_settings.video_bayer_mode);
        core_video_set_flip(g_settings.video_vflip, g_settings.video_hflip);
        log_wake_source(false, true);
        if (!core_status_led_init()) {
            ESP_LOGW(TAG, "LED GPIO%d non disponibile — feedback visivo disabilitato",
                     CORE_GPIO_STATUS_LED);
        }
        ESP_LOGI(TAG, "MrBin CORE avvio — sync GPIO46/RTC (GPIO5 HIGH → ping → upload → DONE)");
        log_reset_reason();
        core_gpio_log_inputs();
        run_sync_mode();
        return;
    }

    // PIR: solo wake legittimo (pin START LOW al boot).
    const bool pir_wake = core_gpio_boot_was_pir_wake();
    if (!pir_wake) {
        core_gpio_boot_snapshot_t snap = {};
        core_gpio_get_boot_snapshot(&snap);
        ESP_LOGW(TAG,
                 "PIR: wake non valido — start=GPIO%d D1=%d D2=%d SQW=%d — DONE TPL immediato",
                 (int)g_settings.rec_gpio_start, snap.d1_level, snap.d2_level, snap.sqw_level);
        core_gpio_hold_tpl_done(0);
        return;
    }
    // Wake PIR: sync ora leggero — niente monitoraggio HW / clear A1F / log.
    boot_rtc_recording_only();
    log_wake_source(false, false);
    ESP_LOGI(TAG, "Boot init (%lld ms)", (long long)core_boot_elapsed_ms());
    core_isp_cal_init();
    core_video_set_bayer_mode(g_settings.video_bayer_mode);
    core_video_set_flip(g_settings.video_vflip, g_settings.video_hflip);
    if (!core_status_led_init()) {
        ESP_LOGW(TAG, "LED GPIO%d non disponibile — feedback visivo disabilitato",
                 CORE_GPIO_STATUS_LED);
    }
    ESP_LOGI(TAG, "MrBin CORE avvio — modalità registrazione");
    run_pir_mode();
}
