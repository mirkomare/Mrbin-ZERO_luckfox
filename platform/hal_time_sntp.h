#pragma once

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNTP_OPMODE_POLL 0

int esp_sntp_enabled(void);
void esp_sntp_stop(void);
void esp_sntp_setoperatingmode(int mode);
void esp_sntp_setservername(int idx, const char *name);
void esp_sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *tv));
void esp_sntp_init(void);

#ifdef __cplusplus
}
#endif
