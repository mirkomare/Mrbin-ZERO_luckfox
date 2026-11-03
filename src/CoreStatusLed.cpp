#include "CoreStatusLed.h"
#include "CoreConfig.h"
#include "CoreDemo.h"
#include "hal_gpio.h"

#include <cstdio>

#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/gpio_struct.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "core_status_led";

static TaskHandle_t s_led_task = nullptr;
static SemaphoreHandle_t s_mode_mtx = nullptr;
static core_status_led_mode_t s_mode = CORE_LED_OFF;
static core_status_led_mode_t s_restore_mode = CORE_LED_OFF;
static volatile bool s_rec_stop_pending = false;
static volatile bool s_sd_ready_pulse = false;
static volatile bool s_rec_sd_writing = false;
static const int s_off_level = CORE_STATUS_LED_ON_LEVEL ? 0 : 1;
static int s_led_sysfs_max = 1;

static void led_sysfs_probe_max(void) {
    const char *led = hal_gpio_sysfs_led();
    if (!led) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/../max_brightness", led);
    /* brightness path: .../mrbin:status/brightness → .../max_brightness */
    const char *slash = strrchr(led, '/');
    if (slash) {
        size_t prefix = (size_t)(slash - led);
        if (prefix + 20 < sizeof(path)) {
            snprintf(path, sizeof(path), "%.*s/max_brightness", (int)prefix, led);
            FILE *fp = fopen(path, "r");
            if (fp) {
                int v = 0;
                if (fscanf(fp, "%d", &v) == 1 && v > 0) {
                    s_led_sysfs_max = v;
                }
                fclose(fp);
            }
        }
    }
}

static bool led_write_sysfs(int level) {
    const char *led = hal_gpio_sysfs_led();
    if (!led) {
        return false;
    }
    const int on_val = CORE_STATUS_LED_ON_LEVEL ? s_led_sysfs_max : 0;
    const int off_val = CORE_STATUS_LED_ON_LEVEL ? 0 : s_led_sysfs_max;
    const int v = level ? on_val : off_val;
    FILE *fp = fopen(led, "w");
    if (!fp) {
        ESP_LOGW(TAG, "LED sysfs write fallita (%s)", led);
        return false;
    }
    fprintf(fp, "%d", v);
    fclose(fp);
    return true;
}

static void led_ensure_output(void) {
    gpio_hold_dis(CORE_GPIO_STATUS_LED);
    gpio_sleep_sel_dis(CORE_GPIO_STATUS_LED);
    gpio_set_direction(CORE_GPIO_STATUS_LED, GPIO_MODE_INPUT_OUTPUT);
    (void)gpio_output_enable(CORE_GPIO_STATUS_LED);
    GPIO.func_out_sel_cfg[(int)CORE_GPIO_STATUS_LED].oen_sel = 1;
    (void)gpio_set_drive_capability(CORE_GPIO_STATUS_LED, GPIO_DRIVE_CAP_3);
}

static void led_write(int level) {
    if (led_write_sysfs(level)) {
        return;
    }
    led_ensure_output();
    gpio_set_level(CORE_GPIO_STATUS_LED, level);
}

static core_status_led_mode_t led_get_mode(void) {
    core_status_led_mode_t mode = CORE_LED_OFF;
    if (s_mode_mtx && xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        mode = s_mode;
        xSemaphoreGive(s_mode_mtx);
    }
    return mode;
}

static void led_set_mode_locked(core_status_led_mode_t mode) {
    if (s_mode_mtx && xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        s_mode = mode;
        xSemaphoreGive(s_mode_mtx);
    }
}

static void led_apply_steady(core_status_led_mode_t mode) {
    switch (mode) {
    case CORE_LED_RECORDING:
        led_write(CORE_STATUS_LED_ON_LEVEL);
        break;
    case CORE_LED_SAVING:
        led_write(CORE_STATUS_LED_ON_LEVEL);
        break;
    case CORE_LED_OFF:
    default:
        led_write(s_off_level);
        break;
    }
}

static void led_pulse_ms(int on_ms) {
    led_write(CORE_STATUS_LED_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_write(s_off_level);
}

static void led_task(void *) {
    while (true) {
        core_status_led_mode_t mode = led_get_mode();

        switch (mode) {
        case CORE_LED_ERROR:
            led_pulse_ms(CORE_STATUS_LED_ERROR_BLINK_MS);
            vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_ERROR_BLINK_MS));
            break;

        case CORE_LED_AP:
            led_pulse_ms(150);
            vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_AP_BLINK_MS));
            break;

        case CORE_LED_STA_CONNECTED: {
            for (int i = 0; i < CORE_STATUS_LED_STA_BLINK_COUNT; ++i) {
                led_pulse_ms(CORE_STATUS_LED_STA_PULSE_MS);
                if (i + 1 < CORE_STATUS_LED_STA_BLINK_COUNT) {
                    vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_STA_BLINK_MS));
                }
            }
            led_set_mode_locked(CORE_LED_OFF);
            led_write(s_off_level);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        case CORE_LED_RECORDING:
            if (core_demo_is_active()) {
                led_write(CORE_STATUS_LED_ON_LEVEL);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            }
            if (s_sd_ready_pulse) {
                s_sd_ready_pulse = false;
                led_pulse_ms(CORE_STATUS_LED_D2_PULSE_MS);
            }
            if (s_rec_sd_writing) {
                led_pulse_ms(CORE_STATUS_LED_SAVING_ON_MS);
                vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_SAVING_OFF_MS));
            } else {
                led_write(CORE_STATUS_LED_ON_LEVEL);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            break;

        case CORE_LED_D2_DETECT: {
            core_status_led_mode_t restore = CORE_LED_OFF;
            if (s_mode_mtx && xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
                restore = s_rec_stop_pending ? CORE_LED_SAVING : s_restore_mode;
                xSemaphoreGive(s_mode_mtx);
            }
            for (int i = 0; i < CORE_STATUS_LED_D2_BLINK_COUNT; ++i) {
                led_pulse_ms(CORE_STATUS_LED_D2_PULSE_MS);
                if (i + 1 < CORE_STATUS_LED_D2_BLINK_COUNT) {
                    vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_D2_GAP_MS));
                }
            }
            core_status_led_mode_t final_mode = s_rec_stop_pending ? CORE_LED_SAVING : restore;
            led_set_mode_locked(final_mode);
            led_apply_steady(final_mode);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }

        case CORE_LED_SAVING:
            led_pulse_ms(CORE_STATUS_LED_SAVING_ON_MS);
            vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_SAVING_OFF_MS));
            break;

        case CORE_LED_OFF:
        default:
            led_write(s_off_level);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

bool core_status_led_init(void) {
    if (s_led_task) {
        return true;
    }

    led_sysfs_probe_max();
    if (!hal_gpio_sysfs_led()) {
        gpio_config_t led_cfg = {
            .pin_bit_mask = (1ULL << CORE_GPIO_STATUS_LED),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&led_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%d config fallita: %s", CORE_GPIO_STATUS_LED, esp_err_to_name(err));
            return false;
        }
        gpio_hold_dis(CORE_GPIO_STATUS_LED);
        gpio_sleep_sel_dis(CORE_GPIO_STATUS_LED);
        (void)gpio_output_enable(CORE_GPIO_STATUS_LED);
        GPIO.func_out_sel_cfg[(int)CORE_GPIO_STATUS_LED].oen_sel = 1;
        (void)gpio_set_drive_capability(CORE_GPIO_STATUS_LED, GPIO_DRIVE_CAP_3);
    } else {
        ESP_LOGI(TAG, "LED via sysfs (max=%d), GPIO%d non esportato",
                 s_led_sysfs_max, CORE_GPIO_STATUS_LED);
    }
    led_write(s_off_level);

    s_mode_mtx = xSemaphoreCreateMutex();
    if (!s_mode_mtx) {
        ESP_LOGE(TAG, "mutex LED fallito");
        return false;
    }

    if (xTaskCreate(led_task, "status_led", CORE_STATUS_LED_TASK_STACK, nullptr,
                    CORE_STATUS_LED_TASK_PRIO, &s_led_task) != pdPASS) {
        ESP_LOGE(TAG, "task LED fallita");
        vSemaphoreDelete(s_mode_mtx);
        s_mode_mtx = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "LED GPIO%d pronto (task prio %d)", CORE_GPIO_STATUS_LED, CORE_STATUS_LED_TASK_PRIO);
    return true;
}

void core_status_led_set_mode(core_status_led_mode_t mode) {
    if (!s_mode_mtx) {
        ESP_LOGW(TAG, "set_mode(%d) ignorato — LED non inizializzato", (int)mode);
        return;
    }
    if (xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        if (mode == CORE_LED_RECORDING) {
            s_rec_stop_pending = false;
            if (s_mode != CORE_LED_RECORDING && s_mode != CORE_LED_SAVING &&
                s_mode != CORE_LED_D2_DETECT) {
                s_restore_mode = s_mode;
            }
        } else if (mode == CORE_LED_AP || mode == CORE_LED_ERROR ||
                   mode == CORE_LED_STA_CONNECTED) {
            s_restore_mode = mode;
        }
        s_mode = mode;
        xSemaphoreGive(s_mode_mtx);
    }
    led_apply_steady(mode);
}

void core_status_led_notify_sta_connected(void) {
    core_status_led_set_mode(CORE_LED_STA_CONNECTED);
}

void core_status_led_notify_d2_detected(void) {
    if (!s_mode_mtx) {
        ESP_LOGW(TAG, "notify_d2 ignorato — LED non inizializzato");
        return;
    }
    if (xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        if (s_mode != CORE_LED_D2_DETECT) {
            s_restore_mode = s_mode;
            s_mode = CORE_LED_D2_DETECT;
        }
        xSemaphoreGive(s_mode_mtx);
    }
    ESP_LOGI(TAG, "LED: feedback D2 (2 lampeggi)");
}

void core_status_led_notify_rec_stop(void) {
    if (core_demo_is_active()) {
        return;
    }
    s_rec_stop_pending = true;
    if (!s_mode_mtx) {
        led_write(s_off_level);
        return;
    }
    if (xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        if (s_mode == CORE_LED_RECORDING || s_mode == CORE_LED_D2_DETECT) {
            s_mode = CORE_LED_SAVING;
        }
        xSemaphoreGive(s_mode_mtx);
    }
    led_apply_steady(CORE_LED_SAVING);
    ESP_LOGI(TAG, "LED: modalità salvataggio SD (lampeggio lento)");
}

void core_status_led_notify_sd_ready(void) {
    if (core_demo_is_active()) {
        return;
    }
    core_status_led_mode_t mode = led_get_mode();
    if (mode == CORE_LED_RECORDING || mode == CORE_LED_SAVING) {
        s_sd_ready_pulse = true;
        ESP_LOGI(TAG, "LED: SD pronta (1 lampeggio in registrazione)");
    }
}

void core_status_led_notify_rec_sd_writing(bool writing) {
    if (core_demo_is_active()) {
        return;
    }
    s_rec_sd_writing = writing;
}

void core_status_led_end_recording(bool config_session) {
    core_status_led_mode_t next = CORE_LED_OFF;
    if (config_session && core_demo_is_active()) {
        next = CORE_LED_OFF;
    } else if (config_session) {
        if (s_restore_mode == CORE_LED_AP || s_restore_mode == CORE_LED_STA_CONNECTED ||
            s_restore_mode == CORE_LED_ERROR) {
            next = s_restore_mode;
        } else {
            next = CORE_LED_AP;
        }
    }
    s_rec_stop_pending = false;
    s_rec_sd_writing = false;
    core_status_led_set_mode(next);
}
