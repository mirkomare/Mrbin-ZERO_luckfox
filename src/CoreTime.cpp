#include "CoreTime.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "core_time";

static bool s_tz_ready = false;

void core_time_init(void) {
    if (s_tz_ready) {
        return;
    }
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    s_tz_ready = true;
}

static bool tm_year_valid(const struct tm *t) {
    return t && (t->tm_year + 1900) >= 2024;
}

bool core_time_is_valid(void) {
    time_t now = time(nullptr);
    if (now < 1704067200) {
        return false;
    }
    struct tm t;
    localtime_r(&now, &t);
    return tm_year_valid(&t);
}

bool core_time_get_local_tm(struct tm *out) {
    if (!out) {
        return false;
    }
    time_t now = time(nullptr);
    localtime_r(&now, out);
    return tm_year_valid(out);
}

void core_time_format_local(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return;
    }
    struct tm t;
    if (!core_time_get_local_tm(&t)) {
        snprintf(buf, buf_len, "Orario non sincronizzato");
        return;
    }
    strftime(buf, buf_len, "%d/%m/%Y %H:%M:%S", &t);
}

static void sntp_sync_notification(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(TAG, "SNTP sincronizzato");
}

bool core_time_sync_sntp(uint32_t timeout_ms) {
    core_time_init();

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification);
    esp_sntp_init();

    const uint32_t step_ms = 200;
    uint32_t waited = 0;
    while (!core_time_is_valid() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }

    if (!core_time_is_valid()) {
        ESP_LOGW(TAG, "SNTP timeout (%lu ms)", (unsigned long)timeout_ms);
        return false;
    }

    char when[32];
    core_time_format_local(when, sizeof(when));
    ESP_LOGI(TAG, "Ora locale: %s", when);
    return true;
}

bool core_time_set_from_tm(const struct tm *t) {
    if (!t || (t->tm_year + 1900) < 2024) {
        return false;
    }
    core_time_init();
    struct tm copy = *t;
    copy.tm_isdst = -1;
    time_t epoch = mktime(&copy);
    if (epoch < 1704067200) {
        return false;
    }
    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };
    return settimeofday(&tv, nullptr) == 0;
}

bool core_time_set_manual(int year, int month, int day, int hour, int min, int sec) {
    if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        return false;
    }

    core_time_init();
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;

    time_t epoch = mktime(&t);
    if (epoch < 0) {
        return false;
    }

    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "settimeofday fallito");
        return false;
    }

    char when[32];
    core_time_format_local(when, sizeof(when));
    ESP_LOGI(TAG, "Ora impostata manualmente: %s", when);
    return true;
}
