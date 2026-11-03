#pragma once

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_NVS_NO_FREE_PAGES       0x1101
#define ESP_ERR_NVS_NEW_VERSION_FOUND   0x1102
#define ESP_ERR_NVS_NOT_FOUND           0x1103

typedef int32_t esp_err_t;
const char *esp_err_to_name(esp_err_t err);
#define ESP_ERROR_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { fprintf(stderr, "ESP_ERROR_CHECK %s\n", esp_err_to_name(_e)); } } while (0)

void hal_log(const char *level, const char *tag, const char *fmt, ...);

#define ESP_LOGE(tag, fmt, ...) hal_log("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) hal_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) hal_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) hal_log("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#ifdef __cplusplus
}
#endif
