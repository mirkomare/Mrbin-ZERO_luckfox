#include "CoreGPIO.h"
#include "CoreConfig.h"
#include "CoreStatusLed.h"

extern "C" bool core_demo_is_active(void);

#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/gpio_struct.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdio.h>

static const char *TAG = "core_gpio";
static core_gpio_boot_snapshot_t s_boot_snapshot = {};
static gpio_num_t s_rec_start_gpio = CORE_GPIO_D1_WAKE;
static gpio_num_t s_rec_stop_gpio = CORE_GPIO_D2_END;
static int64_t s_rec_stop_low_since = -1;
static int s_prev_d1_level = -1;
static int s_prev_d2_level = -1;
static bool s_modem_pwr_on = false;
static bool s_rtc_programming_boot = false;
static bool s_hub_held_low = false;  // HUB ON solo dopo GPIO46 OUTPUT LOW confermato
static volatile bool s_shutdown_requested = false;

// gpio_output_enable() riscrive func_out_sel_cfg azzerando oen_sel: l'output enable
// torna sotto controllo periferica. Senza questo il pad può restare in alta impedenza.
static void core_gpio_force_oe_from_reg(gpio_num_t pin) {
    GPIO.func_out_sel_cfg[(int)pin].oen_sel = 1;
}

static void core_gpio_read_pad_regs(gpio_num_t pin, int *out, int *oe, int *in) {
    *out = *oe = *in = 0;
    if ((int)pin < 0 || (int)pin >= 64) return;
    if ((int)pin < 32) {
        const uint32_t bit = 1u << (uint32_t)pin;
        *out = (GPIO.out.val & bit) ? 1 : 0;
        *oe  = (GPIO.enable.val & bit) ? 1 : 0;
        *in  = (GPIO.in.val & bit) ? 1 : 0;
    } else {
        const uint32_t bit = 1u << ((uint32_t)pin - 32u);
        *out = (GPIO.out1.val & bit) ? 1 : 0;
        *oe  = (GPIO.enable1.val & bit) ? 1 : 0;
        *in  = (GPIO.in1.val & bit) ? 1 : 0;
    }
}

static void core_gpio_log_pad(const char *name, gpio_num_t pin);

static esp_err_t core_gpio_config_line(gpio_num_t pin, gpio_mode_t mode,
                                       int pull_up, int pull_down) {
    gpio_config_t cfg = {};
    cfg.mode = mode;
    cfg.pull_up_en = pull_up;
    cfg.pull_down_en = pull_down;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if ((unsigned)pin < 64u) {
        cfg.pin_bit_mask = 1ULL << (unsigned)pin;
    } else {
        cfg.pins[0] = (int)pin;
        cfg.npins = 1;
    }
    return gpio_config(&cfg);
}

static void core_gpio_log_pad(const char *name, gpio_num_t pin) {
    int out = 0, oe = 0, in = 0;
    int oen = 0, osel = 0;
    core_gpio_read_pad_regs(pin, &out, &oe, &in);
    if ((int)pin >= 0 && (int)pin < 160) {
        oen = (int)GPIO.func_out_sel_cfg[(int)pin].oen_sel;
        osel = (int)GPIO.func_out_sel_cfg[(int)pin].out_sel;
    }
    ESP_LOGI(TAG, "  %-6s GPIO%-2d out=%d oe=%d pad_in=%d oen_sel=%d out_sel=%d",
             name, (int)pin, out, oe, in, oen, osel);
}

void core_gpio_dump_pads(const char *phase) {
    // DONE (GPIO23) è il riferimento noto-funzionante: se 46/5 hanno gli stessi
    // bit ma tensione diversa sul pin, il problema è a valle del SoC.
    ESP_LOGI(TAG, "PAD DUMP [%s] (out=registro, oe=output enable, pad_in=livello reale)", phase);
    core_gpio_log_pad("DONE", CORE_GPIO_TPL_DONE);
    core_gpio_log_pad("SQW", CORE_GPIO_RTC_SQW);
    core_gpio_log_pad("MODEM", CORE_GPIO_MODEM_PWR);
    core_gpio_log_pad("LED", CORE_GPIO_STATUS_LED);
}

static void core_gpio_output_force(gpio_num_t pin) {
    (void)gpio_hold_dis(pin);
    (void)gpio_sleep_sel_dis(pin);
    // INPUT_OUTPUT: senza buffer d'ingresso gpio_get_level() torna sempre 0
    // e il "readback" nei log non dice nulla sul pad reale.
    esp_err_t err = core_gpio_config_line(pin, GPIO_MODE_INPUT_OUTPUT,
                                          GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d output config: %s", (int)pin, esp_err_to_name(err));
        return;
    }
    (void)gpio_output_enable(pin);
    core_gpio_force_oe_from_reg(pin);
    (void)gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
}

static void core_gpio_modem_ensure_output(void) {
    core_gpio_output_force(CORE_GPIO_MODEM_PWR);
}

void core_gpio_reapply_after_hosted(void) {
    const int modem_on = s_modem_pwr_on ? 1 : 0;
    core_gpio_modem_ensure_output();
    gpio_set_level(CORE_GPIO_MODEM_PWR, modem_on);
    const int rb = gpio_get_level(CORE_GPIO_MODEM_PWR);
    ESP_LOGI(TAG, "Post-Hosted: MODEM GPIO%d = %s (readback=%d)",
             (int)CORE_GPIO_MODEM_PWR, modem_on ? "HIGH" : "LOW", rb);
    if (modem_on && rb == 0) {
        ESP_LOGW(TAG, "GPIO%d pad ancora LOW dopo ESP-Hosted — dump IOMUX:",
                 (int)CORE_GPIO_MODEM_PWR);
        gpio_dump_io_configuration(stdout, (1ULL << CORE_GPIO_MODEM_PWR));
    }
}

bool core_gpio_init(void) {
    esp_err_t err = core_gpio_config_line(CORE_GPIO_D1_WAKE, GPIO_MODE_INPUT,
                                          GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio PIR D1 input config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = core_gpio_config_line(CORE_GPIO_D2_END, GPIO_MODE_INPUT,
                                GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio PIR D2 input config failed: %s", esp_err_to_name(err));
        return false;
    }

    // Luckfox Pico Plus: MODE = GPIO1_C5 pull-up, HIGH = CONFIG (jumper a GND = PIR).
    err = core_gpio_config_line(CORE_GPIO_MODE_CFG, GPIO_MODE_INPUT,
                                GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio MODE input config failed: %s", esp_err_to_name(err));
        return false;
    }

    // SQW / Timer (GPIO3_A5): INPUT + pull-up — campionare PRIMA di clear A1F.
    err = core_gpio_config_line(CORE_GPIO_RTC_SQW, GPIO_MODE_INPUT,
                                GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio SQW (GPIO%d) config failed: %s", (int)CORE_GPIO_RTC_SQW, esp_err_to_name(err));
        return false;
    }

    if (CORE_GPIO_DEMO != CORE_GPIO_D1_WAKE) {
        err = core_gpio_config_line((gpio_num_t)CORE_GPIO_DEMO, GPIO_MODE_INPUT,
                                    GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio DEMO (GPIO%d) config: %s", (int)CORE_GPIO_DEMO,
                     esp_err_to_name(err));
        }
    }

    err = core_gpio_config_line(CORE_GPIO_TPL_DONE, GPIO_MODE_OUTPUT,
                                GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio output config failed: %s", esp_err_to_name(err));
        return false;
    }
    gpio_set_level(CORE_GPIO_TPL_DONE, 0);
    core_gpio_modem_ensure_output();
    gpio_set_level(CORE_GPIO_MODEM_PWR, 0);  // modem spento a riposo (PIR)
    s_modem_pwr_on = false;
    s_rtc_programming_boot = false;
    s_hub_held_low = false;
    return true;
}

void core_gpio_save_boot_snapshot(void) {
    s_boot_snapshot.d1_level = gpio_get_level(CORE_GPIO_D1_WAKE);
    s_boot_snapshot.d2_level = gpio_get_level(CORE_GPIO_D2_END);
    s_boot_snapshot.mode_level = gpio_get_level(CORE_GPIO_MODE_CFG);
    s_boot_snapshot.sqw_level = gpio_get_level(CORE_GPIO_RTC_SQW);
    s_boot_snapshot.d10_level = gpio_get_level(CORE_GPIO_TPL_DONE);
    s_boot_snapshot.modem_level = gpio_get_level(CORE_GPIO_MODEM_PWR);
}

void core_gpio_get_boot_snapshot(core_gpio_boot_snapshot_t *out) {
    if (out) {
        *out = s_boot_snapshot;
    }
}

bool core_gpio_is_d1_wake(void) {
    return gpio_get_level(CORE_GPIO_D1_WAKE) == 0;
}

bool core_gpio_is_d2_end(void) {
    if (core_gpio_is_mode_config()) {
        return false;
    }
    return gpio_get_level(CORE_GPIO_D2_END) == 0;
}

void core_gpio_set_rec_pins(uint8_t start_gpio, uint8_t stop_gpio) {
    s_rec_start_gpio = (gpio_num_t)start_gpio;
    s_rec_stop_gpio = (gpio_num_t)stop_gpio;
    ESP_LOGI(TAG, "Pin registrazione: start=GPIO%d stop=GPIO%d",
             (int)s_rec_start_gpio, (int)s_rec_stop_gpio);
}

bool core_gpio_is_rec_start_active(void) {
    return gpio_get_level(s_rec_start_gpio) == 0;
}

void core_gpio_log_pin_edges(void) {
    int d1 = gpio_get_level(CORE_GPIO_D1_WAKE);
    int d2 = gpio_get_level(CORE_GPIO_D2_END);
    if (d1 != s_prev_d1_level) {
        ESP_LOGI(TAG, "D1 (GPIO%d) -> %s%s", (int)CORE_GPIO_D1_WAKE, d1 ? "HIGH" : "LOW",
                 (s_rec_stop_gpio == CORE_GPIO_D1_WAKE) ? " [pin STOP]" : "");
        s_prev_d1_level = d1;
    }
    if (d2 != s_prev_d2_level) {
        ESP_LOGI(TAG, "D2 (GPIO%d) -> %s%s", (int)CORE_GPIO_D2_END, d2 ? "HIGH" : "LOW",
                 (s_rec_stop_gpio == CORE_GPIO_D2_END) ? " [pin STOP]" : "");
        s_prev_d2_level = d2;
    }
}

bool core_gpio_is_rec_stop_active(void) {
    if (core_gpio_is_mode_config()) {
        return false;
    }
    return gpio_get_level(s_rec_stop_gpio) == 0;
}

bool core_gpio_boot_was_pir_wake(void) {
    // Wake TPL5110: snapshot del pin START configurato (D1 o D2) al reset.
    // Non usare lettura runtime: il PIR può già essere HIGH a fine boot.
    if (s_rec_start_gpio == CORE_GPIO_D2_END) {
        return s_boot_snapshot.d2_level == 0;
    }
    return s_boot_snapshot.d1_level == 0;
}

bool core_gpio_boot_was_sqw_low(void) {
    return s_boot_snapshot.sqw_level == 0;
}

void core_gpio_modem_pwr_set(bool on) {
    core_gpio_modem_ensure_output();
    gpio_set_level(CORE_GPIO_MODEM_PWR, on ? 1 : 0);
    s_modem_pwr_on = on;
    const int rb = gpio_get_level(CORE_GPIO_MODEM_PWR);
    ESP_LOGI(TAG, "Modem PWR (GPIO%d) = %s (pad=%d)", (int)CORE_GPIO_MODEM_PWR,
             on ? "HIGH (on)" : "LOW (off)", rb);
    if (on && rb == 0) {
        ESP_LOGW(TAG, "GPIO%d HIGH ma pad LOW: verificare carico sul pad o 0R verso C6_CHIP_PU",
                 (int)CORE_GPIO_MODEM_PWR);
        gpio_dump_io_configuration(stdout, (1ULL << CORE_GPIO_MODEM_PWR));
    }
    core_gpio_dump_pads(on ? "dopo MODEM ON" : "dopo MODEM OFF");
}

bool core_gpio_modem_is_on(void) {
    return s_modem_pwr_on;
}

int core_gpio_modem_level(void) {
    return gpio_get_level(CORE_GPIO_MODEM_PWR);
}

bool core_gpio_boot_was_rtc_programming(void) {
    return s_rtc_programming_boot;
}

bool core_gpio_hub_is_on(void) {
    // HUB ON solo se il firmware ha già mandato GPIO46 LOW e il pad è basso.
    // Lo SQW LOW dell'RTC al boot non basta.
    return s_hub_held_low && gpio_get_level(CORE_GPIO_RTC_SQW) == 0;
}

static void core_gpio_sqw_ensure_output(void) {
    const gpio_num_t pin = CORE_GPIO_RTC_SQW;
    (void)gpio_hold_dis(pin);
    (void)gpio_sleep_sel_dis(pin);
    // Open-drain: tira giù SQW/Hub via Schottky senza lottare col pull-up esterno.
    esp_err_t err = core_gpio_config_line(pin, GPIO_MODE_INPUT_OUTPUT_OD,
                                          GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SQW (GPIO%d) output config: %s", (int)pin, esp_err_to_name(err));
        return;
    }
    core_gpio_force_oe_from_reg(pin);
    (void)gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
}

void core_gpio_sqw_drive_low(void) {
    core_gpio_sqw_ensure_output();
    gpio_set_level(CORE_GPIO_RTC_SQW, 0);
    const int rb = gpio_get_level(CORE_GPIO_RTC_SQW);
    s_hub_held_low = (rb == 0);
    ESP_LOGI(TAG, "SQW (GPIO%d) = OUTPUT LOW (HUB) (readback=%d hub=%s)",
             (int)CORE_GPIO_RTC_SQW, rb, s_hub_held_low ? "ON" : "OFF (pad non LOW)");
    core_gpio_dump_pads("dopo SQW LOW");
}

void core_gpio_sqw_release_high(void) {
    // Fine sessione: rilascia la linea (OD HIGH-Z, pull-up → SQW alto).
    s_hub_held_low = false;
    core_gpio_sqw_ensure_output();
    gpio_set_level(CORE_GPIO_RTC_SQW, 1);
    ESP_LOGI(TAG, "SQW (GPIO%d) = OD rilasciato (HUB OFF, pre-DONE) (readback=%d)",
             (int)CORE_GPIO_RTC_SQW, gpio_get_level(CORE_GPIO_RTC_SQW));
}

static volatile bool      s_rec_stop_latched = false;
static volatile bool      s_ignore_rec_stop = false;  // timer: STOP completamente ignorato

static TaskHandle_t       s_stop_poll_task = nullptr;
static volatile bool      s_stop_poll_run = false;

static esp_timer_handle_t s_mode_timer = nullptr;
static int64_t            s_mode_low_since = -1;
static volatile bool      s_mode_exit_latched = false;
static volatile bool      s_mode_exit_require_high_first = false;
static volatile bool      s_mode_seen_high = false;
static int                s_prev_mode_level = -1;

static int s_prev_stop_level = -1;
static int s_prev_d2_poll_level = -1;
static int64_t s_d2_low_since = -1;
static bool s_d2_led_notified = false;

static void gpio_poll_stop_inputs(void) {
    int stop_level = gpio_get_level(s_rec_stop_gpio);
    if (s_prev_stop_level >= 0 && stop_level != s_prev_stop_level) {
        ESP_LOGI(TAG, "STOP (GPIO%d) -> %s", (int)s_rec_stop_gpio, stop_level ? "HIGH" : "LOW");
    }
    s_prev_stop_level = stop_level;

    int d2_level = gpio_get_level(CORE_GPIO_D2_END);
    if (s_prev_d2_poll_level >= 0 && d2_level != s_prev_d2_poll_level) {
        ESP_LOGI(TAG, "D2 (GPIO%d) -> %s", (int)CORE_GPIO_D2_END, d2_level ? "HIGH" : "LOW");
    }
    s_prev_d2_poll_level = d2_level;

    if (core_gpio_is_mode_config()) {
        s_rec_stop_low_since = -1;
        s_d2_low_since = -1;
        return;
    }

    int64_t now = esp_timer_get_time();

    // Feedback LED su D2 solo se lo STOP è attivo (non in modalità timer).
    if (!s_ignore_rec_stop) {
        if (d2_level == 0) {
            if (s_d2_low_since < 0) {
                s_d2_low_since = now;
            } else if (!s_d2_led_notified &&
                       (now - s_d2_low_since) >= ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000)) {
                s_d2_led_notified = true;
                ESP_LOGI(TAG, "D2 (GPIO%d) LOW stabile %d ms — feedback LED (2 lampeggi)",
                         (int)CORE_GPIO_D2_END, CORE_REC_STOP_DEBOUNCE_MS);
                core_status_led_notify_d2_detected();
            }
        } else {
            s_d2_low_since = -1;
            s_d2_led_notified = false;
        }
    }

    if (s_ignore_rec_stop) {
        // Modalità durata fissa: pin STOP non latched, nessun LED.
        s_rec_stop_low_since = -1;
        return;
    }

    if (s_rec_stop_latched) {
        return;
    }

    if (stop_level != 0) {
        s_rec_stop_low_since = -1;
        return;
    }

    if (s_rec_stop_low_since < 0) {
        s_rec_stop_low_since = now;
        return;
    }
    if ((now - s_rec_stop_low_since) >= ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000)) {
        s_rec_stop_latched = true;
        ESP_LOGI(TAG, "Pin STOP (GPIO%d) LOW stabile %d ms -> STOP",
                 (int)s_rec_stop_gpio, CORE_REC_STOP_DEBOUNCE_MS);
        core_status_led_notify_rec_stop();
    }
}

static void rec_stop_poll_task(void *arg) {
    (void)arg;
    while (s_stop_poll_run) {
        gpio_poll_stop_inputs();
        vTaskDelay(pdMS_TO_TICKS(CORE_REC_STOP_POLL_MS));
    }
    s_stop_poll_task = nullptr;
    vTaskDelete(nullptr);
}

void core_gpio_rec_stop_session_begin(bool ignore_stop) {
    s_ignore_rec_stop = ignore_stop;
    s_rec_stop_low_since = -1;
    s_rec_stop_latched = false;
    s_d2_low_since = -1;
    s_d2_led_notified = false;
    s_prev_d1_level = -1;
    s_prev_d2_level = -1;
    s_prev_stop_level = gpio_get_level(s_rec_stop_gpio);
    s_prev_d2_poll_level = gpio_get_level(CORE_GPIO_D2_END);

    if (ignore_stop) {
        ESP_LOGI(TAG, "Sessione: pin STOP ignorato (nessun latch/LED)");
    }

    s_stop_poll_run = true;
    if (!s_stop_poll_task) {
        if (xTaskCreate(rec_stop_poll_task, "gpio_stop", 3072, nullptr,
                        CORE_GPIO_STOP_POLL_PRIO, &s_stop_poll_task) != pdPASS) {
            ESP_LOGE(TAG, "Task polling STOP/D2 non avviato — fallback inline");
            s_stop_poll_task = nullptr;
            s_stop_poll_run = false;
        }
    }
}

void core_gpio_rec_stop_session_end(void) {
    s_stop_poll_run = false;
    for (int i = 0; i < 50 && s_stop_poll_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_ignore_rec_stop = false;
    s_rec_stop_low_since = -1;
    s_rec_stop_latched = false;
    s_d2_low_since = -1;
    s_d2_led_notified = false;
}

bool core_gpio_is_rec_stop_triggered(void) {
    if (s_rec_stop_latched) {
        return true;
    }
    if (!s_stop_poll_task) {
        gpio_poll_stop_inputs();
    }
    return s_rec_stop_latched;
}

void core_gpio_rec_stop_clear_latch(void) {
    s_rec_stop_latched = false;
    s_rec_stop_low_since = -1;
}

bool core_gpio_wait_for_start_trigger(uint32_t timeout_ms) {
    const int64_t deadline_us = timeout_ms == 0
                                    ? INT64_MAX
                                    : esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    // Attendi rilascio (HIGH) se il pin è ancora LOW dalla sessione precedente.
    if (core_gpio_is_rec_start_active()) {
        ESP_LOGI(TAG, "Attesa rilascio pin START (GPIO%d)...", (int)s_rec_start_gpio);
        while (core_gpio_is_rec_start_active()) {
            if (esp_timer_get_time() >= deadline_us) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(CORE_REC_START_POLL_MS));
        }
    }

    ESP_LOGI(TAG, "Attesa trigger pin START (GPIO%d)...", (int)s_rec_start_gpio);
    int64_t low_since = -1;
    while (esp_timer_get_time() < deadline_us) {
        if (core_gpio_is_rec_start_active()) {
            if (low_since < 0) {
                low_since = esp_timer_get_time();
            } else if ((esp_timer_get_time() - low_since) >=
                       ((int64_t)CORE_REC_START_DEBOUNCE_MS * 1000LL)) {
                ESP_LOGI(TAG, "Pin START (GPIO%d) LOW stabile %d ms — nuova sessione",
                         (int)s_rec_start_gpio, CORE_REC_START_DEBOUNCE_MS);
                return true;
            }
        } else {
            low_since = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(CORE_REC_START_POLL_MS));
    }
    return false;
}

static void mode_exit_poll_cb(void *arg) {
    (void)arg;
    int level = gpio_get_level(CORE_GPIO_MODE_CFG);
    if (s_prev_mode_level >= 0 && level != s_prev_mode_level) {
        ESP_LOGI(TAG, "MODE (GPIO%d) -> %s", (int)CORE_GPIO_MODE_CFG, level ? "HIGH" : "LOW");
    }
    s_prev_mode_level = level;

    if (s_mode_exit_latched) {
        return;
    }
    if (s_mode_exit_require_high_first) {
        if (level == 1) {
            s_mode_seen_high = true;
            s_mode_low_since = -1;
            return;
        }
        if (!s_mode_seen_high) {
            // MODE già LOW al boot RTC: resta in programmazione finché non sale HIGH poi scende.
            s_mode_low_since = -1;
            return;
        }
    } else if (core_gpio_is_mode_config()) {
        s_mode_low_since = -1;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_mode_low_since < 0) {
        s_mode_low_since = now;
        return;
    }
    if ((now - s_mode_low_since) >= ((int64_t)CORE_MODE_EXIT_DEBOUNCE_MS * 1000)) {
        s_mode_exit_latched = true;
        ESP_LOGI(TAG, "GPIO%d (MODE) LOW stabile %d ms -> esco config / DONE",
                 (int)CORE_GPIO_MODE_CFG, CORE_MODE_EXIT_DEBOUNCE_MS);
    }
}

static void mode_exit_session_begin_common(void) {
    s_mode_low_since = -1;
    s_mode_exit_latched = false;
    s_mode_exit_require_high_first = false;
    s_mode_seen_high = false;
    s_prev_mode_level = gpio_get_level(CORE_GPIO_MODE_CFG);
    if (!s_mode_timer) {
        esp_timer_create_args_t args = {
            .callback = mode_exit_poll_cb,
            .name = "modeexit",
        };
        if (esp_timer_create(&args, &s_mode_timer) != ESP_OK) {
            s_mode_timer = nullptr;
        }
    }
    if (s_mode_timer) {
        esp_timer_stop(s_mode_timer);
        esp_timer_start_periodic(s_mode_timer, (uint64_t)CORE_MODE_POLL_MS * 1000);
    }
}

void core_gpio_mode_exit_session_begin(void) {
    mode_exit_session_begin_common();
}

void core_gpio_mode_exit_session_begin_rtc(void) {
    mode_exit_session_begin_common();
    s_mode_exit_require_high_first = true;
    s_mode_seen_high = (s_prev_mode_level == 1);
    if (s_mode_seen_high) {
        ESP_LOGI(TAG, "Config RTC: MODE già HIGH — uscita su LOW stabile");
    } else {
        ESP_LOGI(TAG, "Config RTC: MODE LOW al boot — resto in programmazione (esci con HIGH poi LOW)");
    }
}

void core_gpio_mode_exit_session_end(void) {
    if (s_mode_timer) {
        esp_timer_stop(s_mode_timer);
    }
    s_mode_low_since = -1;
    s_mode_exit_latched = false;
    s_mode_exit_require_high_first = false;
    s_mode_seen_high = false;
}

void core_gpio_rtc_programming_begin(void) {
    s_rtc_programming_boot = true;
    // Prima GPIO5 HIGH: il router RUT241 deve accendersi prima del WiFi/ping.
    core_gpio_modem_pwr_set(true);
    core_gpio_sqw_drive_low();
}

void core_gpio_rtc_programming_end(void) {
    s_rtc_programming_boot = false;
    core_gpio_modem_pwr_set(false);
    core_gpio_sqw_release_high();
}

void core_gpio_request_shutdown(void) {
    if (core_demo_is_active()) {
        ESP_LOGW(TAG, "Spegnimento ignorato — modalità DEMO");
        return;
    }
    s_shutdown_requested = true;
    s_mode_exit_latched = true;
    ESP_LOGI(TAG, "Spegnimento richiesto da Web — D10 DONE / uscita config");
}

bool core_gpio_is_mode_exit_triggered(void) {
    if (core_demo_is_active()) {
        return false;
    }
    if (s_shutdown_requested) {
        return true;
    }
    if (s_mode_timer) {
        return s_mode_exit_latched;
    }
    if (core_gpio_is_mode_config()) {
        s_mode_low_since = -1;
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (s_mode_low_since < 0) {
        s_mode_low_since = now;
        return false;
    }
    return (now - s_mode_low_since) >= ((int64_t)CORE_MODE_EXIT_DEBOUNCE_MS * 1000);
}

bool core_gpio_is_mode_config(void) {
    return gpio_get_level(CORE_GPIO_MODE_CFG) == 1;
}

void core_gpio_signal_tpl_done(void) {
    gpio_set_level(CORE_GPIO_TPL_DONE, 1);
}

void core_gpio_log_inputs(void) {
    ESP_LOGI(TAG,
             "GPIO MODE(%d)=%d D1(%d)=%d D2(%d)=%d SQW(%d)=%d MODEM(%d)=%d DONE(%d)=%d "
             "rec_start=%d rec_stop=%d (0=LOW)",
             CORE_GPIO_MODE_CFG, gpio_get_level(CORE_GPIO_MODE_CFG),
             CORE_GPIO_D1_WAKE, gpio_get_level(CORE_GPIO_D1_WAKE),
             CORE_GPIO_D2_END, gpio_get_level(CORE_GPIO_D2_END),
             CORE_GPIO_RTC_SQW, gpio_get_level(CORE_GPIO_RTC_SQW),
             CORE_GPIO_MODEM_PWR, gpio_get_level(CORE_GPIO_MODEM_PWR),
             CORE_GPIO_TPL_DONE, gpio_get_level(CORE_GPIO_TPL_DONE),
             (int)s_rec_start_gpio, (int)s_rec_stop_gpio);
}

void core_gpio_hold_tpl_done(uint32_t delay_ms) {
    ESP_LOGI(TAG, "Attendo %lu ms prima del DONE", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "Loop spegnimento TPL: DONE (GPIO%d) pulsato HIGH %d ms / LOW %d ms fino a stacco alimentazione",
             CORE_GPIO_TPL_DONE, CORE_TPL_DONE_PULSE_MS, CORE_TPL_DONE_GAP_MS);
    // Loop di spegnimento: DONE HIGH 1 s, riposo 100 ms, all'infinito.
    // Si esce solo quando il TPL toglie alimentazione (power off fisico).
    while (true) {
        gpio_set_level(CORE_GPIO_TPL_DONE, 1);
        vTaskDelay(pdMS_TO_TICKS(CORE_TPL_DONE_PULSE_MS));
        gpio_set_level(CORE_GPIO_TPL_DONE, 0);
        vTaskDelay(pdMS_TO_TICKS(CORE_TPL_DONE_GAP_MS));
    }
}

void core_gpio_pin_selftest(void) {
#if CORE_GPIO_PIN_SELFTEST
    // Onda quadra lenta su MODEM (GPIO5, VDD_LP) e LED (GPIO52, VDD_IO_6).
    // Misurando i due pin si separano: pin sbagliato (non si muove), banco a
    // tensione ridotta (LED basso), rete modem caricata (solo GPIO5 basso).
    // DONE è escluso dal test: pulsarlo spegne la scheda.
    const gpio_num_t pins[]  = { CORE_GPIO_MODEM_PWR, CORE_GPIO_STATUS_LED };
    const char      *names[] = { "MODEM", "LED" };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        core_gpio_output_force(pins[i]);
        ESP_LOGW(TAG, "======== SELFTEST %s su GPIO%d: 4 cicli da 3 s ========",
                 names[i], (int)pins[i]);
        for (int k = 0; k < 4; ++k) {
            gpio_set_level(pins[i], 1);
            vTaskDelay(pdMS_TO_TICKS(50));  // oltre l'RC della linea
            ESP_LOGW(TAG, "SELFTEST %s GPIO%d = HIGH (pad=%d) — misura ORA",
                     names[i], (int)pins[i], gpio_get_level(pins[i]));
            vTaskDelay(pdMS_TO_TICKS(2950));
            gpio_set_level(pins[i], 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGW(TAG, "SELFTEST %s GPIO%d = LOW  (pad=%d)",
                     names[i], (int)pins[i], gpio_get_level(pins[i]));
            vTaskDelay(pdMS_TO_TICKS(2950));
        }
        gpio_set_level(pins[i], 0);
    }
    ESP_LOGW(TAG, "======== SELFTEST finito ========");
#endif
}
