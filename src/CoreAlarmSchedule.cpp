#include "CoreAlarmSchedule.h"
#include "CoreConfig.h"
#include "CoreRtc.h"
#include "CoreTime.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "core_alarm";
static const uint32_t CORE_ALARM_BLOB_MAGIC = 0x414C524D; // "ALRM"
static const uint8_t CORE_ALARM_BLOB_VERSION = 2;

static core_alarm_schedule_t s_schedule;
static bool s_loaded = false;

typedef struct {
    uint32_t magic;
    uint8_t version;
    core_alarm_schedule_t sched;
} core_alarm_eeprom_blob_t;

bool core_alarm_entry_valid(const core_alarm_entry_t *e) {
    if (!e) {
        return false;
    }
    if (e->hour > 23 || e->min > 59 || e->sec > 59) {
        return false;
    }
    if (e->flags & CORE_ALARM_FLAG_REPEAT_DAILY) {
        return true;
    }
    if (e->year < 2024 || e->year > 2099 || e->month < 1 || e->month > 12 || e->day < 1) {
        return false;
    }
    static const int dims[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = dims[e->month - 1];
    if (e->month == 2) {
        bool leap = ((e->year % 4 == 0) && (e->year % 100 != 0)) || (e->year % 400 == 0);
        if (leap) {
            dim = 29;
        }
    }
    return e->day <= (uint8_t)dim;
}

void core_alarm_entry_normalize(core_alarm_entry_t *e) {
    if (!e) {
        return;
    }
    if (!core_alarm_entry_valid(e)) {
        e->flags &= (uint8_t)~CORE_ALARM_FLAG_ENABLED;
    }
}

static time_t entry_to_epoch(const core_alarm_entry_t *e) {
    struct tm t = {};
    t.tm_year = (int)e->year - 1900;
    t.tm_mon = (int)e->month - 1;
    t.tm_mday = (int)e->day;
    t.tm_hour = (int)e->hour;
    t.tm_min = (int)e->min;
    t.tm_sec = (int)e->sec;
    t.tm_isdst = -1;
    return mktime(&t);
}

static time_t entry_next_fire(const core_alarm_entry_t *e, time_t now) {
    if (!e || !(e->flags & CORE_ALARM_FLAG_ENABLED)) {
        return 0;
    }

    if (e->flags & CORE_ALARM_FLAG_REPEAT_DAILY) {
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        struct tm cand = now_tm;
        cand.tm_hour = e->hour;
        cand.tm_min = e->min;
        cand.tm_sec = e->sec;
        cand.tm_isdst = -1;
        time_t t = mktime(&cand);
        if (t <= now) {
            t += 86400;
        }
        return t;
    }

    time_t t = entry_to_epoch(e);
    if (t <= now) {
        return 0;
    }
    return t;
}

bool core_alarm_schedule_hw_available(void) {
    return core_rtc_is_present() && core_rtc_eeprom_is_present();
}

bool core_alarm_schedule_rtc_available(void) {
    return core_alarm_schedule_hw_available();
}

bool core_alarm_schedule_load(core_alarm_schedule_t *out) {
    if (!out) {
        return false;
    }
    core_alarm_schedule_t defaults = {};
    defaults.master_enabled = false;
    defaults.count = 0;
    *out = defaults;

    if (!core_alarm_schedule_hw_available()) {
        return true;
    }

    core_alarm_eeprom_blob_t blob = {};
    if (!core_rtc_eeprom_read(CORE_ALARM_EEPROM_OFF, (uint8_t *)&blob, sizeof(blob))) {
        ESP_LOGW(TAG, "Lettura schedule EEPROM fallita");
        return true;
    }

    if (blob.magic != CORE_ALARM_BLOB_MAGIC || blob.version != CORE_ALARM_BLOB_VERSION) {
        ESP_LOGI(TAG, "Schedule EEPROM vuota o non inizializzata");
        return true;
    }

    if (blob.sched.count > CORE_ALARM_MAX_ENTRIES) {
        blob.sched.count = CORE_ALARM_MAX_ENTRIES;
    }
    for (unsigned i = 0; i < blob.sched.count; ++i) {
        core_alarm_entry_normalize(&blob.sched.entries[i]);
    }
    *out = blob.sched;
    return true;
}

bool core_alarm_schedule_save(const core_alarm_schedule_t *in) {
    if (!in || in->count > CORE_ALARM_MAX_ENTRIES) {
        return false;
    }
    if (!core_alarm_schedule_hw_available()) {
        ESP_LOGW(TAG, "Salvataggio schedule impossibile — modulo RTC/EEPROM assente");
        return false;
    }

    core_alarm_eeprom_blob_t blob = {
        .magic = CORE_ALARM_BLOB_MAGIC,
        .version = CORE_ALARM_BLOB_VERSION,
        .sched = *in,
    };

    int64_t t0 = esp_timer_get_time();
    if (!core_rtc_eeprom_write(CORE_ALARM_EEPROM_OFF, (const uint8_t *)&blob, sizeof(blob))) {
        ESP_LOGE(TAG, "Scrittura schedule EEPROM fallita");
        return false;
    }

    s_schedule = *in;
    s_loaded = true;
    ESP_LOGI(TAG, "Allarmi salvati su AT24C32 (master=%d count=%u, %lld ms)",
             in->master_enabled ? 1 : 0, (unsigned)in->count,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

void core_alarm_schedule_init(void) {
    if (!s_loaded) {
        core_alarm_schedule_load(&s_schedule);
        s_loaded = true;
    }
}

const core_alarm_schedule_t *core_alarm_schedule_get(void) {
    core_alarm_schedule_init();
    return &s_schedule;
}

bool core_alarm_schedule_add(const core_alarm_entry_t *entry) {
    if (!core_alarm_schedule_hw_available()) {
        return false;
    }
    if (!entry || !core_alarm_entry_valid(entry)) {
        return false;
    }
    core_alarm_schedule_init();
    if (s_schedule.count >= CORE_ALARM_MAX_ENTRIES) {
        ESP_LOGW(TAG, "Lista allarmi piena (max %d)", CORE_ALARM_MAX_ENTRIES);
        return false;
    }
    s_schedule.entries[s_schedule.count++] = *entry;
    return core_alarm_schedule_save(&s_schedule);
}

bool core_alarm_schedule_delete(unsigned index) {
    if (!core_alarm_schedule_hw_available()) {
        return false;
    }
    core_alarm_schedule_init();
    if (index >= s_schedule.count) {
        return false;
    }
    for (unsigned i = index + 1; i < s_schedule.count; ++i) {
        s_schedule.entries[i - 1] = s_schedule.entries[i];
    }
    s_schedule.count--;
    return core_alarm_schedule_save(&s_schedule);
}

bool core_alarm_schedule_set_master(bool enabled) {
    if (!core_alarm_schedule_hw_available()) {
        return false;
    }
    core_alarm_schedule_init();
    s_schedule.master_enabled = enabled;
    return core_alarm_schedule_save(&s_schedule);
}

bool core_alarm_schedule_find_next(time_t now, core_alarm_entry_t *out, int *out_idx) {
    if (!out) {
        return false;
    }
    core_alarm_schedule_init();
    if (!s_schedule.master_enabled || s_schedule.count == 0) {
        return false;
    }

    time_t best = 0;
    int best_idx = -1;
    for (unsigned i = 0; i < s_schedule.count; ++i) {
        time_t t = entry_next_fire(&s_schedule.entries[i], now);
        if (t == 0) {
            continue;
        }
        if (best_idx < 0 || t < best) {
            best = t;
            best_idx = (int)i;
        }
    }

    if (best_idx < 0) {
        return false;
    }

    *out = s_schedule.entries[best_idx];
    if (out_idx) {
        *out_idx = best_idx;
    }
    return true;
}

/* Non-ricorrenti già scattati/scaduti: via dalla lista (i giornalieri restano). */
static void core_alarm_schedule_remove_fired_one_shots(void) {
    if (!core_time_is_valid()) {
        return;
    }

    time_t now = time(nullptr);
    bool changed = false;
    unsigned i = 0;
    while (i < s_schedule.count) {
        core_alarm_entry_t *e = &s_schedule.entries[i];
        if (e->flags & CORE_ALARM_FLAG_REPEAT_DAILY) {
            ++i;
            continue;
        }

        time_t t = entry_to_epoch(e);
        if (t != 0 && t <= now) {
            ESP_LOGI(TAG, "Allarme one-shot %u rimosso (eseguito/scaduto %04u-%02u-%02u %02u:%02u)",
                     (unsigned)i, (unsigned)e->year, (unsigned)e->month, (unsigned)e->day,
                     (unsigned)e->hour, (unsigned)e->min);
            for (unsigned j = i + 1; j < s_schedule.count; ++j) {
                s_schedule.entries[j - 1] = s_schedule.entries[j];
            }
            s_schedule.count--;
            changed = true;
            continue;
        }
        ++i;
    }

    if (changed) {
        core_alarm_schedule_save(&s_schedule);
    }
}

bool core_alarm_schedule_program_rtc(void) {
    if (!core_alarm_schedule_hw_available()) {
        return false;
    }

    // "now" dal chip (TCXO), non dall'ESP: in PIR l'orologio di sistema può aver driftato.
    if (core_rtc_is_present()) {
        (void)core_rtc_sync_system_time();
    }
    if (core_time_is_valid()) {
        core_alarm_schedule_remove_fired_one_shots();
    }

    if (!s_schedule.master_enabled) {
        core_rtc_disable_alarms();
        ESP_LOGI(TAG, "Scheduling disabilitato — allarmi DS3231 spenti");
        return true;
    }

    if (!core_time_is_valid()) {
        ESP_LOGW(TAG, "Ora non valida — allarme non programmato");
        core_rtc_disable_alarms();
        return false;
    }

    if (!core_rtc_is_present()) {
        ESP_LOGW(TAG, "DS3231 assente — allarme non programmato");
        return false;
    }

    time_t now = time(nullptr);
    core_alarm_entry_t next = {};
    int idx = -1;
    if (!core_alarm_schedule_find_next(now, &next, &idx)) {
        core_rtc_disable_alarms();
        ESP_LOGI(TAG, "Nessun allarme futuro — DS3231 spento");
        return true;
    }

    time_t fire = entry_next_fire(&next, now);
    struct tm fire_tm;
    localtime_r(&fire, &fire_tm);

    if (!core_rtc_set_alarm1(&fire_tm)) {
        ESP_LOGW(TAG, "Programmazione allarme DS3231 fallita");
        return false;
    }

    char when[40];
    strftime(when, sizeof(when), "%d/%m/%Y %H:%M:%S", &fire_tm);
    ESP_LOGI(TAG, "Prossimo allarme DS3231: %s (idx=%d daily=%d)", when, idx,
             (next.flags & CORE_ALARM_FLAG_REPEAT_DAILY) ? 1 : 0);
    return true;
}

void core_alarm_schedule_after_a1f_clear(void) {
    if (!core_alarm_schedule_hw_available()) {
        return;
    }
    core_alarm_schedule_init();
    if (core_rtc_is_present()) {
        (void)core_rtc_sync_system_time();
    }
    if (!core_time_is_valid()) {
        ESP_LOGW(TAG, "A1F azzerato — ora non valida, prossimo allarme non riconfigurato");
        return;
    }

    core_alarm_schedule_remove_fired_one_shots();
    if (core_alarm_schedule_program_rtc()) {
        ESP_LOGI(TAG, "A1F azzerato — prossimo allarme DS3231 riconfigurato");
    } else {
        ESP_LOGW(TAG, "A1F azzerato — riconfigurazione prossimo allarme fallita");
    }
}

void core_alarm_schedule_on_boot(void) {
    if (!core_alarm_schedule_hw_available() || !core_rtc_boot_was_alarm_wake()) {
        return;
    }
    core_alarm_schedule_after_a1f_clear();
}

bool core_alarm_schedule_schedule_reboot(unsigned delay_sec) {
    if (delay_sec < 5) {
        delay_sec = 5;
    }
    if (!core_alarm_schedule_hw_available()) {
        ESP_LOGW(TAG, "Reboot RTC: hardware schedule assente");
        return false;
    }
    if (!core_time_is_valid()) {
        ESP_LOGW(TAG, "Reboot RTC: ora di sistema non valida");
        return false;
    }

    time_t fire = time(nullptr) + (time_t)delay_sec;
    struct tm fire_tm = {};
    if (!localtime_r(&fire, &fire_tm)) {
        return false;
    }

    core_alarm_entry_t e = {};
    e.flags = CORE_ALARM_FLAG_ENABLED;
    e.sec = (uint8_t)fire_tm.tm_sec;
    e.min = (uint8_t)fire_tm.tm_min;
    e.hour = (uint8_t)fire_tm.tm_hour;
    e.day = (uint8_t)fire_tm.tm_mday;
    e.month = (uint8_t)(fire_tm.tm_mon + 1);
    e.year = (uint16_t)(fire_tm.tm_year + 1900);

    if (!core_alarm_entry_valid(&e) || !core_alarm_schedule_add(&e)) {
        ESP_LOGW(TAG, "Reboot RTC: aggiunta allarme fallita");
        return false;
    }

    (void)core_alarm_schedule_set_master(true);
    if (core_rtc_sync_from_system()) {
        ESP_LOGI(TAG, "Reboot RTC: ora sistema scritta sul DS3231");
    }
    if (!core_alarm_schedule_program_rtc()) {
        ESP_LOGW(TAG, "Reboot RTC: programmazione DS3231 fallita");
        return false;
    }

    char when[40];
    strftime(when, sizeof(when), "%d/%m/%Y %H:%M:%S", &fire_tm);
    ESP_LOGI(TAG, "Reboot RTC programmato: sveglia alle %s (+%u s)", when, delay_sec);
    return true;
}

void core_alarm_schedule_on_shutdown(bool push_system_time_to_rtc) {
    if (!core_alarm_schedule_hw_available()) {
        return;
    }
    core_alarm_schedule_init();
    if (push_system_time_to_rtc) {
        if (core_time_is_valid()) {
            if (core_rtc_sync_from_system()) {
                ESP_LOGI(TAG, "Ora sistema (affidabile) scritta sul DS3231");
            } else {
                ESP_LOGW(TAG, "Scrittura ora sistema→DS3231 fallita");
            }
        }
    } else {
        ESP_LOGI(TAG, "Shutdown: non sovrascrivo DS3231 con ora ESP (chip più stabile)");
    }
    (void)core_alarm_schedule_program_rtc();
}
