#pragma once

#include "hal_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT_OD,
} gpio_mode_t;

#define GPIO_PULLUP_DISABLE   0
#define GPIO_PULLUP_ENABLE    1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE  1
#define GPIO_INTR_DISABLE     0
#define GPIO_DRIVE_CAP_3      3

typedef struct {
    uint64_t pin_bit_mask; /* pins 0..63 only */
    gpio_mode_t mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
    int pins[8];           /* extra linux lines >= 64 (MODEM=73) */
    int npins;
} gpio_config_t;

/* Fake pad registers so CoreGPIO dump compiles. */
typedef struct {
    int oen_sel;
    int out_sel;
} gpio_func_out_sel_cfg_t;

typedef struct {
    uint32_t val;
} gpio_reg32_t;

typedef struct {
    gpio_reg32_t out;
    gpio_reg32_t enable;
    gpio_reg32_t in;
    gpio_reg32_t out1;
    gpio_reg32_t enable1;
    gpio_reg32_t in1;
    gpio_func_out_sel_cfg_t func_out_sel_cfg[160];
} gpio_dev_t;

extern gpio_dev_t GPIO;

void hal_gpio_load_pins_env(void);
int hal_gpio_line(int logical_default);
const char *hal_gpio_sysfs_led(void);

esp_err_t gpio_config(const gpio_config_t *cfg);
int gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_hold_dis(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_output_enable(gpio_num_t pin);
esp_err_t gpio_set_drive_capability(gpio_num_t pin, int cap);
void gpio_dump_io_configuration(FILE *fp, uint64_t mask);

#ifdef __cplusplus
}
#endif
