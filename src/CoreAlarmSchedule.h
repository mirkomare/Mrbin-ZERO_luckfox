#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORE_ALARM_MAX_ENTRIES  64

#define CORE_ALARM_FLAG_ENABLED       0x01
#define CORE_ALARM_FLAG_REPEAT_DAILY  0x02

typedef struct {
    uint8_t  flags;
    uint8_t  sec;
    uint8_t  min;
    uint8_t  hour;
    uint8_t  day;    // 1–31 (ignorato se REPEAT_DAILY)
    uint8_t  month;  // 1–12 (ignorato se REPEAT_DAILY)
    uint16_t year;   // es. 2026 (ignorato se REPEAT_DAILY)
} core_alarm_entry_t;

typedef struct {
    bool     master_enabled;
    uint16_t count;
    core_alarm_entry_t entries[CORE_ALARM_MAX_ENTRIES];
} core_alarm_schedule_t;

void core_alarm_schedule_init(void);
const core_alarm_schedule_t *core_alarm_schedule_get(void);

bool core_alarm_entry_valid(const core_alarm_entry_t *e);
void core_alarm_entry_normalize(core_alarm_entry_t *e);

bool core_alarm_schedule_load(core_alarm_schedule_t *out);
bool core_alarm_schedule_save(const core_alarm_schedule_t *in);

bool core_alarm_schedule_add(const core_alarm_entry_t *entry);
bool core_alarm_schedule_delete(unsigned index);
bool core_alarm_schedule_set_master(bool enabled);

// Trova il prossimo evento >= now. out_idx opzionale.
bool core_alarm_schedule_find_next(time_t now, core_alarm_entry_t *out, int *out_idx);

// true se modulo DS3231 + AT24C32 presenti (schedule su EEPROM).
bool core_alarm_schedule_hw_available(void);
bool core_alarm_schedule_rtc_available(void);

// Scrive sul DS3231 il prossimo allarme (se master ON e RTC ok).
bool core_alarm_schedule_program_rtc(void);

// Boot: gestisce allarme scattato (rimuove one-shot eseguiti + riprogramma DS3231).
void core_alarm_schedule_on_boot(void);

// Dopo clear bit A1F: rimuove one-shot eseguiti e scrive il prossimo allarme sul DS3231.
void core_alarm_schedule_after_a1f_clear(void);

// Aggiunge allarme one-shot tra delay_sec, abilita scheduling e programma DS3231 (riavvio RTC).
bool core_alarm_schedule_schedule_reboot(unsigned delay_sec);

// Spegnimento TPL: programma prossimo allarme.
// push_system_time_to_rtc=true solo se l'ora ESP è affidabile (es. post-SNTP in CONFIG).
// In PIR lasciare false: il DS3231 è più stabile dell'orologio ESP e non va sovrascritto.
void core_alarm_schedule_on_shutdown(bool push_system_time_to_rtc);

#ifdef __cplusplus
}
#endif
