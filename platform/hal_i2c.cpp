#include "hal_i2c.h"
#include "hal_system.h"
#include "CoreConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/i2c-dev.h>
#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703
#endif
#endif
#endif

typedef struct {
    int fd;
    char dev[64];
} i2c_bus_t;

typedef struct {
    i2c_bus_t *bus;
    uint16_t addr;
} i2c_dev_t;

static char s_i2c_dev[64];

#ifndef _WIN32
#ifdef __linux__
/* DTS node i2c-gpio0 (alias i2c5 → /dev/i2c-5). Fall back to any i2c-gpio*. */
static bool find_i2c_gpio_dev(char *out, size_t out_len) {
    static const char *candidates[] = {
        "/dev/i2c-5", "/dev/i2c-gpio0", "/dev/i2c-6", "/dev/i2c-7", nullptr,
    };
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], R_OK | W_OK) == 0) {
            snprintf(out, out_len, "%s", candidates[i]);
            return true;
        }
    }
    for (int n = 0; n < 16; ++n) {
        char name_path[64];
        char name[64] = {0};
        snprintf(name_path, sizeof(name_path), "/sys/class/i2c-adapter/i2c-%d/name", n);
        FILE *fp = fopen(name_path, "r");
        if (!fp) continue;
        if (!fgets(name, sizeof(name), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);
        if (strstr(name, "i2c-gpio") || strstr(name, "i2c_gpio")) {
            snprintf(out, out_len, "/dev/i2c-%d", n);
            return access(out, R_OK | W_OK) == 0;
        }
    }
    return false;
}
#endif
#endif

static void load_i2c_dev(void) {
    if (s_i2c_dev[0]) return;
    const char *e = getenv("CORE_I2C_DEV");
    if (e && e[0]) {
        snprintf(s_i2c_dev, sizeof(s_i2c_dev), "%s", e);
    }
    if (!s_i2c_dev[0]) {
        const char *paths[] = { "/etc/mrbin/pins.env", "overlay/etc/mrbin/pins.env", nullptr };
        FILE *fp = nullptr;
        for (int i = 0; paths[i]; ++i) {
            fp = fopen(paths[i], "r");
            if (fp) break;
        }
        if (fp) {
            char line[160];
            while (fgets(line, sizeof(line), fp)) {
                if (line[0] == '#' || line[0] == '\n') continue;
                char val[64];
                if (sscanf(line, " CORE_I2C_DEV=%63s", val) == 1 && val[0]) {
                    snprintf(s_i2c_dev, sizeof(s_i2c_dev), "%s", val);
                    break;
                }
            }
            fclose(fp);
        }
    }
#ifndef _WIN32
#ifdef __linux__
    if (!s_i2c_dev[0] || access(s_i2c_dev, R_OK | W_OK) != 0) {
        char found[64];
        if (find_i2c_gpio_dev(found, sizeof(found))) {
            snprintf(s_i2c_dev, sizeof(s_i2c_dev), "%s", found);
            return;
        }
    }
#endif
#endif
    if (!s_i2c_dev[0]) snprintf(s_i2c_dev, sizeof(s_i2c_dev), "/dev/i2c-5");
}

static const char *i2c_path(void) {
    load_i2c_dev();
    return s_i2c_dev;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *out) {
    (void)cfg;
    i2c_bus_t *b = (i2c_bus_t *)calloc(1, sizeof(*b));
    snprintf(b->dev, sizeof(b->dev), "%s", i2c_path());
#ifndef _WIN32
    b->fd = hal_open_timeout(b->dev, O_RDWR, CORE_VIDEO_OPEN_TIMEOUT_MS);
    if (b->fd < 0) {
        /* HOST_SIM / missing adapter / I2C hang */
        b->fd = -1;
    }
#else
    b->fd = -1;
#endif
    if (out) *out = b;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus) {
    i2c_bus_t *b = (i2c_bus_t *)bus;
    if (!b) return ESP_OK;
#ifndef _WIN32
    if (b->fd >= 0) close(b->fd);
#endif
    free(b);
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out) {
    i2c_dev_t *d = (i2c_dev_t *)calloc(1, sizeof(*d));
    d->bus = (i2c_bus_t *)bus;
    d->addr = cfg ? cfg->device_address : 0;
    if (out) *out = d;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) {
    free(dev);
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t) { return ESP_OK; }

#ifndef _WIN32
#ifdef __linux__
static int sel(i2c_dev_t *d) {
    if (!d || !d->bus || d->bus->fd < 0) return -1;
    return ioctl(d->bus->fd, I2C_SLAVE, d->addr);
}
#endif
#endif

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int) {
#ifndef _WIN32
#ifdef __linux__
    i2c_dev_t *d = (i2c_dev_t *)dev;
    if (sel(d) < 0) return ESP_FAIL;
    if (write(d->bus->fd, data, len) != (ssize_t)len) return ESP_FAIL;
    return ESP_OK;
#endif
#endif
    (void)dev;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *data, size_t len, int) {
#ifndef _WIN32
#ifdef __linux__
    i2c_dev_t *d = (i2c_dev_t *)dev;
    if (sel(d) < 0) return ESP_FAIL;
    if (read(d->bus->fd, data, len) != (ssize_t)len) return ESP_FAIL;
    return ESP_OK;
#endif
#endif
    (void)dev;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint8_t addr, int) {
#ifndef _WIN32
#ifdef __linux__
    i2c_bus_t *b = (i2c_bus_t *)bus;
    if (!b || b->fd < 0) return ESP_FAIL;
    if (ioctl(b->fd, I2C_SLAVE, addr) < 0) return ESP_FAIL;
    uint8_t x = 0;
    return (write(b->fd, &x, 0) >= 0) ? ESP_OK : ESP_FAIL;
#endif
#endif
    (void)bus;
    (void)addr;
    return ESP_FAIL;
}
