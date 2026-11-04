#include "hal_gpio.h"
#include "CoreConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif

gpio_dev_t GPIO;

#define HAL_GPIO_LINE_MAX 160

static int s_lines[9];
static int s_levels[HAL_GPIO_LINE_MAX];
static int s_export[HAL_GPIO_LINE_MAX];
static int s_host;
static char s_led_class[128];

static int *map_for(int def) {
    if (def == CORE_GPIO_D1_WAKE) return &s_lines[0];
    if (def == CORE_GPIO_D2_END) return &s_lines[1];
    if (def == CORE_GPIO_TPL_DONE) return &s_lines[2];
    if (def == CORE_GPIO_MODE_CFG) return &s_lines[3];
    if (def == CORE_GPIO_STATUS_LED) return &s_lines[4];
    if (def == CORE_GPIO_RTC_SQW) return &s_lines[5];
    if (def == CORE_GPIO_MODEM_PWR) return &s_lines[6];
    if (def == CORE_GPIO_DEMO) return &s_lines[7];
    return nullptr;
}

static void defaults(void) {
    s_lines[0] = CORE_GPIO_D1_WAKE;
    s_lines[1] = CORE_GPIO_D2_END;
    s_lines[2] = CORE_GPIO_TPL_DONE;
    s_lines[3] = CORE_GPIO_MODE_CFG;
    s_lines[4] = CORE_GPIO_STATUS_LED;
    /* SQW is GPIO1_C7 (linux 55). linux 101 stays refused: it is SDMMC_CMD. */
    s_lines[5] = CORE_GPIO_RTC_SQW;
    s_lines[6] = CORE_GPIO_MODEM_PWR;
    s_lines[7] = CORE_GPIO_DEMO;
    /* Host-sim defaults keyed by compile-time linux numbers (may be >= 64). */
    if (CORE_GPIO_DEMO >= 0 && CORE_GPIO_DEMO < HAL_GPIO_LINE_MAX)
        s_levels[CORE_GPIO_DEMO] = 1;
    if (CORE_GPIO_MODE_CFG >= 0 && CORE_GPIO_MODE_CFG < HAL_GPIO_LINE_MAX)
        s_levels[CORE_GPIO_MODE_CFG] = 0;
    if (CORE_GPIO_D1_WAKE >= 0 && CORE_GPIO_D1_WAKE < HAL_GPIO_LINE_MAX)
        s_levels[CORE_GPIO_D1_WAKE] = 1;
    if (CORE_GPIO_D2_END >= 0 && CORE_GPIO_D2_END < HAL_GPIO_LINE_MAX)
        s_levels[CORE_GPIO_D2_END] = 1;
    if (CORE_GPIO_RTC_SQW >= 0 && CORE_GPIO_RTC_SQW < HAL_GPIO_LINE_MAX)
        s_levels[CORE_GPIO_RTC_SQW] = 1;
#ifdef HOST_SIM
    s_host = 1;
#endif
#ifndef _WIN32
    if (access("/sys/class/gpio", F_OK) != 0) s_host = 1;
    if (access("/sys/class/leds/mrbin:status", F_OK) == 0) {
        snprintf(s_led_class, sizeof(s_led_class), "/sys/class/leds/mrbin:status/brightness");
    }
#else
    s_host = 1;
#endif
}

void hal_gpio_load_pins_env(void) {
    defaults();
    const char *paths[] = { "/etc/mrbin/pins.env", "overlay/etc/mrbin/pins.env", nullptr };
    FILE *fp = nullptr;
    for (int i = 0; paths[i]; ++i) {
        fp = fopen(paths[i], "r");
        if (fp) break;
    }
    if (!fp) return;
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64];
        int val = 0;
        if (sscanf(line, " %63[A-Za-z0-9_]=%d", key, &val) != 2) continue;
        if (strcmp(key, "CORE_PIN_D1") == 0) s_lines[0] = val;
        else if (strcmp(key, "CORE_PIN_D2") == 0) s_lines[1] = val;
        else if (strcmp(key, "CORE_PIN_DONE") == 0) s_lines[2] = val;
        else if (strcmp(key, "CORE_PIN_MODE") == 0) s_lines[3] = val;
        else if (strcmp(key, "CORE_PIN_LED") == 0) s_lines[4] = val;
        else if (strcmp(key, "CORE_PIN_SQW") == 0) {
            /* GPIO3_A5 / linux 101 is SDMMC_CMD. Using it as SQW kills the TF card. */
            if (val == 101) val = -1;
            s_lines[5] = val;
        }
        else if (strcmp(key, "CORE_PIN_MODEM") == 0) s_lines[6] = val;
        else if (strcmp(key, "CORE_PIN_DEMO") == 0) s_lines[7] = val;
    }
    fclose(fp);
}

int hal_gpio_line(int logical_default) {
    int *p = map_for(logical_default);
    return p ? *p : logical_default;
}

const char *hal_gpio_sysfs_led(void) { return s_led_class[0] ? s_led_class : nullptr; }

#ifndef _WIN32
/* GPIO3_A5 — SDMMC_CMD on Pico Plus TF slot. */
#define HAL_GPIO_SDMMC_CMD 101

static void sysfs_unexport(int n) {
    if (n < 0) return;
    FILE *fp = fopen("/sys/class/gpio/unexport", "w");
    if (fp) {
        fprintf(fp, "%d", n);
        fclose(fp);
    }
    if (n < HAL_GPIO_LINE_MAX) s_export[n] = 0;
}

static int led_line(void) {
    return s_lines[4];
}

static bool led_kernel_owned(int n) {
    return s_led_class[0] && n == led_line();
}

static void sysfs_export(int n) {
    if (n < 0 || n >= HAL_GPIO_LINE_MAX || s_export[n] || s_host) return;
    if (led_kernel_owned(n)) {
        return;
    }
    if (n == HAL_GPIO_SDMMC_CMD) {
        sysfs_unexport(n);
        return;
    }
    FILE *fp = fopen("/sys/class/gpio/export", "w");
    if (fp) {
        fprintf(fp, "%d", n);
        fclose(fp);
    }
    s_export[n] = 1;
}

static void sysfs_dir(int n, const char *dir) {
    char p[80];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", n);
    FILE *fp = fopen(p, "w");
    if (fp) {
        fputs(dir, fp);
        fclose(fp);
    }
}

static int s_led_max_brightness = 1;

static void led_probe_max_brightness(void) {
    if (!s_led_class[0]) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "/sys/class/leds/mrbin:status/max_brightness");
    FILE *fp = fopen(path, "r");
    if (fp) {
        int m = 1;
        if (fscanf(fp, "%d", &m) == 1 && m > 0) {
            s_led_max_brightness = m;
        }
        fclose(fp);
    }
}

static void sysfs_write(int n, int v) {
    if (s_led_class[0] && n == s_lines[4]) {
        if (s_led_max_brightness <= 1) {
            led_probe_max_brightness();
        }
        const int out = v ? s_led_max_brightness : 0;
        FILE *fp = fopen(s_led_class, "w");
        if (fp) {
            fprintf(fp, "%d", out);
            fclose(fp);
        }
        return;
    }
    char p[80];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", n);
    FILE *fp = fopen(p, "w");
    if (fp) {
        fprintf(fp, "%d", v);
        fclose(fp);
    }
}

static int sysfs_read(int n) {
    char p[80], b[8] = {0};
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", n);
    FILE *fp = fopen(p, "r");
    if (!fp) return (n >= 0 && n < HAL_GPIO_LINE_MAX) ? s_levels[n] : 0;
    if (!fgets(b, sizeof(b), fp)) {
        fclose(fp);
        return (n >= 0 && n < HAL_GPIO_LINE_MAX) ? s_levels[n] : 0;
    }
    fclose(fp);
    return atoi(b) ? 1 : 0;
}
#endif

static int remap_line(int logical) {
    static int once;
    if (!once) {
        hal_gpio_load_pins_env();
#ifndef _WIN32
        /* GPIO3_A5 is SDMMC_CMD. A leftover export (value=1) kills the TF
         * card even when CORE_PIN_SQW=-1 and we never re-export it. */
        sysfs_unexport(HAL_GPIO_SDMMC_CMD);
        if (s_led_class[0]) {
            led_probe_max_brightness();
            sysfs_unexport(led_line());
        }
#endif
        once = 1;
    }
    return hal_gpio_line(logical);
}

/* RV1106 IOC (non il GRF). Il default del pin è pull-down: a riposo legge 0
 * e un filo a GND non è un evento. Pull-up = 1 nel campo a 2 bit, con
 * write-enable nei bit alti. Vale solo per il banco GPIO1. */
static void rv1106_gpio1_pull_up(int linux_gpio) {
#ifndef _WIN32
    if (linux_gpio < 32 || linux_gpio > 63 || s_host) {
        return;
    }
    const int pin = linux_gpio - 32;
    const uint32_t reg = 0x1C0u + (uint32_t)(pin / 8) * 4u;
    const int bit = (pin % 8) * 2;
    const uint32_t data = (0x3u << (bit + 16)) | (0x1u << bit);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        return;
    }
    void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xff538000);
    if (page != MAP_FAILED) {
        volatile uint32_t *addr = (volatile uint32_t *)((char *)page + reg);
        *addr = data;
        munmap(page, 4096);
    }
    close(fd);
#else
    (void)linux_gpio;
#endif
}

static void apply_one(int logical, gpio_mode_t mode) {
    int n = remap_line(logical);
    if (n < 0 || n >= HAL_GPIO_LINE_MAX) return;
#ifndef _WIN32
    if (!s_host) {
        sysfs_export(n);
        if (mode == GPIO_MODE_OUTPUT || mode == GPIO_MODE_INPUT_OUTPUT ||
            mode == GPIO_MODE_INPUT_OUTPUT_OD) {
            sysfs_dir(n, "out");
        } else {
            sysfs_dir(n, "in");
            /* D1, D2, MODE e DEMO a riposo HIGH: il filo a GND è il fronte.
             * Senza pull-up il banco GPIO1 legge 0 e il ponticello non si vede. */
            if (n == s_lines[0] || n == s_lines[1] || n == s_lines[3] || n == s_lines[7]) {
                rv1106_gpio1_pull_up(n);
            }
        }
    }
#else
    (void)mode;
#endif
}

esp_err_t gpio_config(const gpio_config_t *cfg) {
    if (!cfg) return ESP_FAIL;
    for (int i = 0; i < 64; ++i) {
        if (cfg->pin_bit_mask & (1ULL << i)) apply_one(i, cfg->mode);
    }
    int extra = cfg->npins;
    if (extra < 0) extra = 0;
    if (extra > 8) extra = 8;
    for (int i = 0; i < extra; ++i) apply_one(cfg->pins[i], cfg->mode);
    return ESP_OK;
}

int gpio_set_level(gpio_num_t pin, uint32_t level) {
    int n = remap_line((int)pin);
    if (n < 0 || n >= HAL_GPIO_LINE_MAX) return -1;
    s_levels[n] = level ? 1 : 0;
#ifndef _WIN32
    if (!s_host) sysfs_write(n, s_levels[n]);
#endif
    return 0;
}

int gpio_get_level(gpio_num_t pin) {
    int n = remap_line((int)pin);
    if (n < 0 || n >= HAL_GPIO_LINE_MAX) {
        /* Disabled SQW must look idle HIGH so boot does not take the sync path. */
        return ((int)pin == CORE_GPIO_RTC_SQW) ? 1 : 0;
    }
#ifndef _WIN32
    if (!s_host) return sysfs_read(n);
#endif
    return s_levels[n];
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) {
    apply_one((int)pin, mode);
    return ESP_OK;
}

esp_err_t gpio_hold_dis(gpio_num_t) { return ESP_OK; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t) { return ESP_OK; }
esp_err_t gpio_output_enable(gpio_num_t) { return ESP_OK; }
esp_err_t gpio_set_drive_capability(gpio_num_t, int) { return ESP_OK; }
void gpio_dump_io_configuration(FILE *fp, uint64_t) {
    if (fp) fprintf(fp, "(gpio dump n/a on linux sysfs)\n");
}
