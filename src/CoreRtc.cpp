#include "CoreRtc.h"
#include "CoreAlarmSchedule.h"
#include "CoreConfig.h"
#include "CoreTime.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <string.h>
#include <sys/time.h>

static const char *TAG = "core_rtc";
static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static i2c_master_dev_handle_t s_eeprom_dev = nullptr;
static bool s_read_ok = false;
static int s_probe_state = 0;
static int s_eeprom_probe_state = 0;
static bool s_boot_alarm_wake = false;
static bool s_time_setup_failed = false;
static bool s_hw_status_monitoring = false;

typedef struct {
    uint8_t count;
    int64_t epoch[CORE_RTC_A1F_LOG_MAX];  // più recente in [0]
} core_rtc_a1f_log_store_t;

static core_rtc_a1f_log_store_t s_a1f_log = {};
static bool s_a1f_log_loaded = false;

static bool core_rtc_read_tm(struct tm *out);

static void core_rtc_a1f_log_load(void) {
    if (s_a1f_log_loaded) {
        return;
    }
    s_a1f_log_loaded = true;
    nvs_handle_t nvs = 0;
    if (nvs_open(CORE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    core_rtc_a1f_log_store_t blob = {};
    size_t len = sizeof(blob);
    if (nvs_get_blob(nvs, "rtc_a1f_log", &blob, &len) == ESP_OK && len == sizeof(blob)) {
        if (blob.count > CORE_RTC_A1F_LOG_MAX) {
            blob.count = CORE_RTC_A1F_LOG_MAX;
        }
        s_a1f_log = blob;
    }
    nvs_close(nvs);
}

static void core_rtc_a1f_log_save(void) {
    nvs_handle_t nvs = 0;
    if (nvs_open(CORE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_set_blob(nvs, "rtc_a1f_log", &s_a1f_log, sizeof(s_a1f_log));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static int64_t core_rtc_a1f_log_capture_time(void) {
    struct tm t = {};
    if (core_time_get_local_tm(&t)) {
        time_t ts = mktime(&t);
        if (ts >= 0) {
            return (int64_t)ts;
        }
    }
    if (core_rtc_read_tm(&t)) {
        time_t ts = mktime(&t);
        if (ts >= 0) {
            return (int64_t)ts;
        }
    }
    return 0;
}

void core_rtc_a1f_log_format_ts(int64_t epoch, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return;
    }
    if (epoch <= 0) {
        snprintf(buf, buf_len, "Orario non disponibile");
        return;
    }
    struct tm t = {};
    time_t ts = (time_t)epoch;
    if (!localtime_r(&ts, &t)) {
        snprintf(buf, buf_len, "Orario non disponibile");
        return;
    }
    strftime(buf, buf_len, "%d/%m/%Y %H:%M:%S", &t);
}

static void core_rtc_a1f_log_record_clear(void) {
    if (!s_hw_status_monitoring) {
        return;
    }
    core_rtc_a1f_log_load();
    const int64_t when = core_rtc_a1f_log_capture_time();
    if (s_a1f_log.count < CORE_RTC_A1F_LOG_MAX) {
        memmove(&s_a1f_log.epoch[1], &s_a1f_log.epoch[0],
                (size_t)s_a1f_log.count * sizeof(s_a1f_log.epoch[0]));
        s_a1f_log.count++;
    } else {
        memmove(&s_a1f_log.epoch[1], &s_a1f_log.epoch[0],
                (CORE_RTC_A1F_LOG_MAX - 1) * sizeof(s_a1f_log.epoch[0]));
    }
    s_a1f_log.epoch[0] = when;
    core_rtc_a1f_log_save();
    char when_str[32];
    core_rtc_a1f_log_format_ts(when, when_str, sizeof(when_str));
    ESP_LOGI(TAG, "A1F azzerato — registrato in storico: %s", when_str);
}

unsigned core_rtc_a1f_log_count(void) {
    core_rtc_a1f_log_load();
    return s_a1f_log.count;
}

bool core_rtc_a1f_log_get(unsigned idx, int64_t *epoch_out) {
    core_rtc_a1f_log_load();
    if (!epoch_out || idx >= s_a1f_log.count) {
        return false;
    }
    *epoch_out = s_a1f_log.epoch[idx];
    return true;
}

#define AT24C32_PAGE_SIZE 32
#define AT24C32_WRITE_MS  5

#define DS3231_REG_SECONDS   0x00
#define DS3231_REG_CONTROL   0x0E
#define DS3231_REG_STATUS    0x0F
#define DS3231_REG_A1_SEC    0x07

// Dopo rec lunga il bus I2C1 può restare in timeout HW: timeout più largo + retry.
#define CORE_RTC_I2C_XFER_MS  200
#define CORE_RTC_I2C_RETRIES  3

static uint8_t bcd_to_dec(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0f));
}

static uint8_t dec_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void core_rtc_bus_recover(void) {
    if (s_bus) {
        (void)i2c_master_bus_reset(s_bus);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}

static bool core_rtc_write_reg(uint8_t reg, const uint8_t *data, size_t len) {
    if (!s_dev || !data || len == 0 || len > 7) {
        return false;
    }
    uint8_t buf[8];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    for (int attempt = 0; attempt < CORE_RTC_I2C_RETRIES; ++attempt) {
        if (i2c_master_transmit(s_dev, buf, len + 1, CORE_RTC_I2C_XFER_MS) == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "I2C write reg 0x%02X fallita (tentativo %d/%d)",
                 (unsigned)reg, attempt + 1, CORE_RTC_I2C_RETRIES);
        core_rtc_bus_recover();
    }
    return false;
}

static bool core_rtc_read_reg(uint8_t reg, uint8_t *data, size_t len) {
    if (!s_dev || !data || len == 0) {
        return false;
    }
    for (int attempt = 0; attempt < CORE_RTC_I2C_RETRIES; ++attempt) {
        if (i2c_master_transmit(s_dev, &reg, 1, CORE_RTC_I2C_XFER_MS) == ESP_OK &&
            i2c_master_receive(s_dev, data, len, CORE_RTC_I2C_XFER_MS) == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "I2C read reg 0x%02X fallita (tentativo %d/%d)",
                 (unsigned)reg, attempt + 1, CORE_RTC_I2C_RETRIES);
        core_rtc_bus_recover();
    }
    return false;
}

static bool core_rtc_read_tm(struct tm *out) {
    if (!s_dev || !out) {
        return false;
    }

    uint8_t raw[7] = {};
    if (!core_rtc_read_reg(DS3231_REG_SECONDS, raw, sizeof(raw))) {
        return false;
    }

    if (raw[0] & 0x80) {
        return false;
    }

    struct tm t = {};
    t.tm_sec = bcd_to_dec(raw[0] & 0x7f);
    t.tm_min = bcd_to_dec(raw[1] & 0x7f);
    t.tm_hour = bcd_to_dec(raw[2] & 0x3f);
    t.tm_wday = bcd_to_dec(raw[3] & 0x07);
    t.tm_mday = bcd_to_dec(raw[4] & 0x3f);
    t.tm_mon = bcd_to_dec(raw[5] & 0x1f) - 1;
    t.tm_year = bcd_to_dec(raw[6]) + 100;
    t.tm_isdst = -1;
    *out = t;
    return true;
}

static bool core_rtc_probe_once(void) {
    if (s_probe_state < 0) {
        return false;
    }
    if (s_probe_state > 0) {
        return true;
    }

    int64_t t0 = esp_timer_get_time();
    if (!core_rtc_init()) {
        s_probe_state = -1;
        ESP_LOGI(TAG, "DS3231 assente — scheduling/allarmi disabilitati (%lld ms)",
                 (long long)((esp_timer_get_time() - t0) / 1000));
        return false;
    }

    uint8_t sec = 0;
    if (!core_rtc_read_reg(DS3231_REG_SECONDS, &sec, 1)) {
        core_rtc_deinit();
        s_probe_state = -1;
        ESP_LOGI(TAG, "DS3231 non risponde — scheduling/allarmi disabilitati (%lld ms)",
                 (long long)((esp_timer_get_time() - t0) / 1000));
        return false;
    }

    s_probe_state = 1;
    return true;
}

bool core_rtc_init(void) {
    if (s_dev) {
        return true;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (i2c_port_t)CORE_RTC_I2C_PORT_NUM,
        .sda_io_num = CORE_RTC_I2C_SDA,
        .scl_io_num = CORE_RTC_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_new_master_bus fallito: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_RTC_I2C_ADDR,
        .scl_speed_hz = CORE_RTC_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_device fallito: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return false;
    }
    return true;
}

static bool core_rtc_eeprom_dev_ready(void) {
    if (s_eeprom_dev) {
        return true;
    }
    if (!core_rtc_init()) {
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_RTC_EEPROM_ADDR,
        .scl_speed_hz = CORE_RTC_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_eeprom_dev) != ESP_OK) {
        return false;
    }
    return true;
}

bool core_rtc_eeprom_read(uint16_t offset, uint8_t *data, size_t len) {
    if (!data || len == 0 || (size_t)offset + len > CORE_ALARM_EEPROM_SIZE) {
        return false;
    }
    if (!core_rtc_eeprom_dev_ready()) {
        return false;
    }

    uint8_t addr[2] = {(uint8_t)(offset >> 8), (uint8_t)(offset & 0xff)};
    if (i2c_master_transmit(s_eeprom_dev, addr, sizeof(addr), CORE_RTC_I2C_XFER_MS) != ESP_OK) {
        return false;
    }
    return i2c_master_receive(s_eeprom_dev, data, len, CORE_RTC_I2C_XFER_MS) == ESP_OK;
}

bool core_rtc_eeprom_write(uint16_t offset, const uint8_t *data, size_t len) {
    if (!data || len == 0 || (size_t)offset + len > CORE_ALARM_EEPROM_SIZE) {
        return false;
    }
    if (!core_rtc_eeprom_dev_ready()) {
        return false;
    }

    size_t written = 0;
    while (written < len) {
        uint16_t cur_off = (uint16_t)(offset + written);
        size_t page_space = AT24C32_PAGE_SIZE - (cur_off % AT24C32_PAGE_SIZE);
        size_t chunk = len - written;
        if (chunk > page_space) {
            chunk = page_space;
        }

        uint8_t buf[2 + AT24C32_PAGE_SIZE];
        buf[0] = (uint8_t)(cur_off >> 8);
        buf[1] = (uint8_t)(cur_off & 0xff);
        memcpy(buf + 2, data + written, chunk);

        if (i2c_master_transmit(s_eeprom_dev, buf, chunk + 2, 100) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(AT24C32_WRITE_MS));
        written += chunk;
    }
    return true;
}

static bool core_rtc_eeprom_probe_once(void) {
    if (s_eeprom_probe_state < 0) {
        return false;
    }
    if (s_eeprom_probe_state > 0) {
        return true;
    }
    if (!core_rtc_eeprom_dev_ready()) {
        s_eeprom_probe_state = -1;
        return false;
    }
    uint8_t tmp = 0;
    if (!core_rtc_eeprom_read(0, &tmp, 1)) {
        if (s_eeprom_dev) {
            i2c_master_bus_rm_device(s_eeprom_dev);
            s_eeprom_dev = nullptr;
        }
        s_eeprom_probe_state = -1;
        ESP_LOGI(TAG, "AT24C32 assente — schedule EEPROM non disponibile");
        return false;
    }
    s_eeprom_probe_state = 1;
    return true;
}

void core_rtc_deinit(void) {
    if (s_eeprom_dev) {
        i2c_master_bus_rm_device(s_eeprom_dev);
        s_eeprom_dev = nullptr;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
    }
    s_eeprom_probe_state = 0;
    s_probe_state = 0;
}

bool core_rtc_eeprom_is_present(void) {
    return core_rtc_eeprom_probe_once();
}

bool core_rtc_is_present(void) {
    return core_rtc_probe_once();
}

bool core_rtc_boot_was_alarm_wake(void) {
    return s_boot_alarm_wake;
}

static bool core_rtc_status_osf(void) {
    uint8_t st = 0;
    if (!core_rtc_read_reg(DS3231_REG_STATUS, &st, 1)) {
        return true;
    }
    return (st & 0x80) != 0;
}

bool core_rtc_chip_time_valid(void) {
    if (!core_rtc_is_present()) {
        return false;
    }
    if (core_rtc_status_osf()) {
        return false;
    }
    struct tm t = {};
    if (!core_rtc_read_tm(&t)) {
        return false;
    }
    return (t.tm_year + 1900) >= 2024;
}

bool core_rtc_time_setup_failed(void) {
    return core_rtc_is_present() && (s_time_setup_failed || !core_rtc_chip_time_valid());
}

// Configurazione operativa MrBin: oscillatore on, SQW = interrupt (non 1 Hz), allarmi spenti.
static bool core_rtc_apply_base_config(void) {
    // Control 0x0E: EOSC=0 BBSQW=0 CONV=0 RS2=0 RS1=0 INTCN=1 A2IE=0 A1IE=0 → 0x04
    const uint8_t ctrl = 0x04;
    if (!core_rtc_write_reg(DS3231_REG_CONTROL, &ctrl, 1)) {
        ESP_LOGW(TAG, "Scrittura Control DS3231 fallita");
        return false;
    }
    // Non cancellare A1F/A2F qui: il latch boot (STATUS-BOOT / schedule) li legge
    // prima; il clear passa da core_rtc_clear_alarm_flags() in handle_programming_boot.
    return true;
}

static void core_rtc_setup_on_detect(void) {
    s_time_setup_failed = false;

    const bool osf = core_rtc_status_osf();
    struct tm t = {};
    const bool readable = core_rtc_read_tm(&t);
    const bool virgin = osf || !readable || (t.tm_year + 1900) < 2024;

    if (!core_rtc_apply_base_config()) {
        s_time_setup_failed = true;
        ESP_LOGE(TAG, "DS3231: configurazione base fallita");
        return;
    }

    if (virgin) {
        ESP_LOGI(TAG, "DS3231 nuovo/non inizializzato — config base applicata (INTCN, allarmi off)");
        // Solo se il chip è vergine: scrivi ora ESP→RTC (serve SNTP/CONFIG affidabile).
        // Se il chip ha già un'ora valida NON sovrascrivere mai qui: in PIR l'ESP
        // può avere un orologio driftato/non-SNTP e rovinerebbe il DS3231.
        if (core_time_is_valid()) {
            if (core_rtc_sync_from_system()) {
                ESP_LOGI(TAG, "DS3231: ora di sistema scritta sul chip (modulo vergine)");
                s_time_setup_failed = false;
                return;
            }
            ESP_LOGE(TAG, "DS3231: scrittura ora fallita");
            s_time_setup_failed = true;
            return;
        }
        s_time_setup_failed = true;
        ESP_LOGW(TAG, "DS3231: ora non impostabile (sistema non sincronizzato) — serve WiFi/SNTP");
        return;
    }

    ESP_LOGI(TAG, "DS3231: config base applicata (INTCN=1) — ora chip lasciata invariata");
    s_time_setup_failed = false;
}

void core_rtc_set_hw_status_monitoring(bool enabled) {
    s_hw_status_monitoring = enabled;
}

bool core_rtc_hw_status_monitoring_enabled(void) {
    return s_hw_status_monitoring;
}

void core_rtc_reset_probe_for_retry(void) {
    if (s_probe_state < 0) {
        s_probe_state = 0;
        core_rtc_deinit();
    }
}

bool core_rtc_probe_wake_routing(void) {
    s_boot_alarm_wake = false;
    s_read_ok = false;
    s_time_setup_failed = false;

    if (!core_rtc_probe_once()) {
        return false;
    }

    if (core_rtc_alarm1_fired()) {
        s_boot_alarm_wake = true;
    }
    return true;
}

bool core_rtc_handle_programming_boot(void) {
    if (!core_rtc_is_present()) {
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    if (s_boot_alarm_wake) {
        core_rtc_clear_alarm_flags();
        ESP_LOGI(TAG, "Wake RTC: A1F attivo — flag cancellato, SQW rilasciato (%lld ms)",
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }

    core_rtc_setup_on_detect();
    return true;
}

bool core_rtc_last_read_ok(void) {
    return s_read_ok && core_rtc_is_present();
}

bool core_rtc_sync_system_time(void) {
    s_read_ok = false;
    if (!core_rtc_is_present()) {
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    struct tm t = {};
    if (!core_rtc_read_tm(&t)) {
        ESP_LOGW(TAG, "Lettura DS3231 fallita (%lld ms)", (long long)((esp_timer_get_time() - t0) / 1000));
        return false;
    }

    if (!core_time_set_from_tm(&t)) {
        ESP_LOGW(TAG, "Ora DS3231 non valida (%lld ms)", (long long)((esp_timer_get_time() - t0) / 1000));
        return false;
    }

    s_read_ok = true;
    char when[32];
    core_time_format_local(when, sizeof(when));
    ESP_LOGI(TAG, "DS3231: %s (%lld ms)", when, (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

bool core_rtc_sync_from_system(void) {
    if (!core_rtc_is_present() || !core_time_is_valid()) {
        return false;
    }

    struct tm t;
    if (!core_time_get_local_tm(&t)) {
        return false;
    }

    uint8_t raw[7];
    raw[0] = dec_to_bcd((uint8_t)t.tm_sec);
    raw[1] = dec_to_bcd((uint8_t)t.tm_min);
    raw[2] = dec_to_bcd((uint8_t)t.tm_hour);
    raw[3] = dec_to_bcd((uint8_t)(t.tm_wday + 1));
    raw[4] = dec_to_bcd((uint8_t)t.tm_mday);
    raw[5] = dec_to_bcd((uint8_t)(t.tm_mon + 1));
    raw[6] = dec_to_bcd((uint8_t)(t.tm_year - 100));

    if (!core_rtc_write_reg(DS3231_REG_SECONDS, raw, sizeof(raw))) {
        ESP_LOGW(TAG, "Scrittura ora DS3231 fallita");
        s_time_setup_failed = true;
        return false;
    }
    // Cancella OSF (oscillator stop) così le letture successive sono valide.
    uint8_t st = 0;
    if (core_rtc_read_reg(DS3231_REG_STATUS, &st, 1)) {
        st &= (uint8_t)~0x83; // OSF + A1F + A2F
        (void)core_rtc_write_reg(DS3231_REG_STATUS, &st, 1);
    }
    s_read_ok = true;
    s_time_setup_failed = false;
    return true;
}

bool core_rtc_get_local_tm(struct tm *out) {
    if (!out || !core_rtc_is_present()) {
        return false;
    }
    return core_rtc_read_tm(out);
}

void core_rtc_format_local(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return;
    }
    if (!core_rtc_is_present()) {
        snprintf(buf, buf_len, "DS3231 assente");
        return;
    }
    struct tm t = {};
    if (!core_rtc_read_tm(&t)) {
        snprintf(buf, buf_len, "non leggibile");
        return;
    }
    uint8_t st = 0;
    bool osf = core_rtc_read_reg(DS3231_REG_STATUS, &st, 1) && (st & 0x80);
    char when[40];
    if (strftime(when, sizeof(when), "%d/%m/%Y %H:%M:%S", &t) == 0) {
        snprintf(buf, buf_len, "formato non valido");
        return;
    }
    if (osf) {
        snprintf(buf, buf_len, "%s (oscillatore era fermo)", when);
    } else if ((t.tm_year + 1900) < 2024) {
        snprintf(buf, buf_len, "%s (non inizializzato)", when);
    } else {
        snprintf(buf, buf_len, "%s", when);
    }
}

bool core_rtc_set_alarm1(const struct tm *when) {
    if (!when || !core_rtc_is_present()) {
        return false;
    }
    if (when->tm_mday < 1 || when->tm_mday > 31 || when->tm_hour > 23 || when->tm_min > 59 ||
        when->tm_sec > 59) {
        return false;
    }

    uint8_t alarm[4];
    alarm[0] = dec_to_bcd((uint8_t)when->tm_sec) & 0x7f;
    alarm[1] = dec_to_bcd((uint8_t)when->tm_min) & 0x7f;
    alarm[2] = dec_to_bcd((uint8_t)when->tm_hour) & 0x3f;
    alarm[3] = dec_to_bcd((uint8_t)when->tm_mday) & 0x3f;

    if (!core_rtc_write_reg(DS3231_REG_A1_SEC, alarm, sizeof(alarm))) {
        return false;
    }

    // Control: A1IE=1, INTCN=1 → SQW/INT open-drain basso solo allo scatto.
    // BBSQW=1 → stesso comportamento anche a sola VBAT (VCC assente / scheda spenta).
    // Non azzerare INTCN: con INTCN=0 e RS=00 il pin emette onda 1 Hz.
    uint8_t ctrl = 0;
    if (!core_rtc_read_reg(DS3231_REG_CONTROL, &ctrl, 1)) {
        return false;
    }
    ctrl |= 0x45;                 // BBSQW | A1IE | INTCN
    ctrl &= (uint8_t)~0x1A;       // clear A2IE, RS1, RS2 (lascia INTCN)
    if (!core_rtc_write_reg(DS3231_REG_CONTROL, &ctrl, 1)) {
        return false;
    }

    uint8_t st = 0;
    if (core_rtc_read_reg(DS3231_REG_STATUS, &st, 1)) {
        st &= (uint8_t)~0x03;
        (void)core_rtc_write_reg(DS3231_REG_STATUS, &st, 1);
    }
    return true;
}

void core_rtc_disable_alarms(void) {
    if (!core_rtc_is_present()) {
        return;
    }
    // Stesso standby della config base: INTCN=1, A1IE/A2IE/BBSQW=0.
    const uint8_t ctrl = 0x04;
    (void)core_rtc_write_reg(DS3231_REG_CONTROL, &ctrl, 1);
}

bool core_rtc_alarm1_fired(void) {
    if (!core_rtc_is_present()) {
        return false;
    }
    uint8_t st = 0;
    if (!core_rtc_read_reg(DS3231_REG_STATUS, &st, 1)) {
        return false;
    }
    return (st & 0x01) != 0;
}

void core_rtc_clear_alarm_flags(void) {
    if (!core_rtc_is_present()) {
        return;
    }
    uint8_t st = 0;
    if (!core_rtc_read_reg(DS3231_REG_STATUS, &st, 1)) {
        return;
    }
    const bool had_a1f = (st & 0x01) != 0;
    st &= (uint8_t)~0x03;
    if (!core_rtc_write_reg(DS3231_REG_STATUS, &st, 1)) {
        return;
    }
    if (had_a1f) {
        core_rtc_a1f_log_record_clear();
        core_alarm_schedule_after_a1f_clear();
    }
}
