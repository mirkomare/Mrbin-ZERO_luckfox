#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_BROWNOUT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_SDIO,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);

/* open() with a hard timeout. Returns fd >= 0 or -1 (errno set). */
int hal_open_timeout(const char *path, int flags, int timeout_ms);

#ifdef __cplusplus
}
#endif
