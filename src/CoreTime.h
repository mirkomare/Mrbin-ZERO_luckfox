#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void core_time_init(void);

bool core_time_is_valid(void);
bool core_time_get_local_tm(struct tm *out);
void core_time_format_local(char *buf, size_t buf_len);

bool core_time_sync_sntp(uint32_t timeout_ms);
bool core_time_set_manual(int year, int month, int day, int hour, int min, int sec);
bool core_time_set_from_tm(const struct tm *t);

#ifdef __cplusplus
}
#endif
