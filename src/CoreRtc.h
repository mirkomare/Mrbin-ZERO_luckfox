#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_rtc_init(void);
void core_rtc_deinit(void);

// Probe una volta per boot: false se modulo assente o bus I2C non risponde.
bool core_rtc_is_present(void);

// Probe I2C + lettura A1F per routing boot (no clear, no log, no setup chip).
bool core_rtc_probe_wake_routing(void);
void core_rtc_reset_probe_for_retry(void);
// Setup DS3231, clear A1F e storico — solo modalità programmazione / wake RTC.
bool core_rtc_handle_programming_boot(void);
bool core_rtc_boot_was_alarm_wake(void);
void core_rtc_set_hw_status_monitoring(bool enabled);
bool core_rtc_hw_status_monitoring_enabled(void);

bool core_rtc_sync_system_time(void);
bool core_rtc_last_read_ok(void);
bool core_rtc_sync_from_system(void);

// True se DS3231 risponde e l'ora sul chip è utilizzabile (anno >= 2024, OSF spento).
bool core_rtc_chip_time_valid(void);
// True se il modulo c'è ma l'ora non è stata impostata (GUI: banner rosso).
bool core_rtc_time_setup_failed(void);

// Legge l'ora dal chip senza modificare l'orologio di sistema.
bool core_rtc_get_local_tm(struct tm *out);
void core_rtc_format_local(char *buf, size_t buf_len);

bool core_rtc_set_alarm1(const struct tm *when);
void core_rtc_disable_alarms(void);
bool core_rtc_alarm1_fired(void);
void core_rtc_clear_alarm_flags(void);

// Storico ultimi azzeramenti bit A1F (allarme 1 scattato) sul DS3231.
#define CORE_RTC_A1F_LOG_MAX 10
unsigned core_rtc_a1f_log_count(void);
bool core_rtc_a1f_log_get(unsigned idx, int64_t *epoch_out);  // 0 = più recente
void core_rtc_a1f_log_format_ts(int64_t epoch, char *buf, size_t buf_len);

bool core_rtc_eeprom_is_present(void);
bool core_rtc_eeprom_read(uint16_t offset, uint8_t *data, size_t len);
bool core_rtc_eeprom_write(uint16_t offset, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
