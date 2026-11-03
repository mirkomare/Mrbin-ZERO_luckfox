#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int d1_level;
    int d2_level;
    int mode_level;
    int sqw_level;   // GPIO46: 0 = LOW al boot (wake SQW / altro P4)
    int d10_level;   // GPIO23 DONE verso TPL5110
    int modem_level; // GPIO5 modem PWR
} core_gpio_boot_snapshot_t;

bool core_gpio_init(void);
void core_gpio_save_boot_snapshot(void);
void core_gpio_get_boot_snapshot(core_gpio_boot_snapshot_t *out);
bool core_gpio_is_d1_wake(void);      // GPIO28 LOW
bool core_gpio_is_d2_end(void);       // GPIO21 LOW (ignorato se GPIO29 HIGH)
bool core_gpio_is_mode_config(void);  // GPIO29 HIGH = WiFi/Web manuale
void core_gpio_set_rec_pins(uint8_t start_gpio, uint8_t stop_gpio);
bool core_gpio_boot_was_pir_wake(void);    // snapshot boot: pin START (D1 o D2) LOW
bool core_gpio_boot_was_sqw_low(void);     // snapshot boot: GPIO46 LOW
void core_gpio_modem_pwr_set(bool on);     // GPIO5 HIGH/LOW
bool core_gpio_modem_is_on(void);          // GPIO5 HIGH (stato firmware)
void core_gpio_reapply_after_hosted(void); // ri-applica modem/LED dopo ESP-Hosted
void core_gpio_dump_pads(const char *phase); // stato reale pad DONE/SQW/MODEM/LED
void core_gpio_pin_selftest(void);           // no-op se CORE_GPIO_PIN_SELFTEST = 0
int core_gpio_modem_level(void);           // 0/1 per display
bool core_gpio_boot_was_rtc_programming(void);
bool core_gpio_hub_is_on(void);            // true solo dopo GPIO46 OUTPUT LOW confermato
void core_gpio_sqw_drive_low(void);        // wake RTC: GPIO46 → OUTPUT LOW (poi HUB ON)
void core_gpio_sqw_release_high(void);     // fine sessione: GPIO46 → OUTPUT HIGH (HUB OFF)
// ignore_stop=true (modalità timer): pin STOP non latched e nessun feedback LED.
void core_gpio_rec_stop_session_begin(bool ignore_stop);
void core_gpio_rec_stop_session_end(void);   // ferma task polling stop a fine sessione
bool core_gpio_is_rec_stop_triggered(void);  // true se pin stop LOW stabile >= 50 ms (latch)
void core_gpio_rec_stop_clear_latch(void);   // reset latch stop tra sessioni (timed mode)
bool core_gpio_wait_for_start_trigger(uint32_t timeout_ms); // 0 = attesa infinita; edge LOW su pin start
void core_gpio_mode_exit_session_begin(void); // timer polling GPIO29 (MODE) in config
void core_gpio_mode_exit_session_begin_rtc(void); // config forzata da RTC: esci solo dopo MODE HIGH→LOW
void core_gpio_mode_exit_session_end(void);
bool core_gpio_is_mode_exit_triggered(void);  // true se MODE LOW stabile (uscita config → DONE)
void core_gpio_rtc_programming_begin(void);   // GPIO46 LOW + GPIO5 HIGH (wake RTC)
void core_gpio_rtc_programming_end(void);     // GPIO5 LOW + GPIO46 HIGH (fine sessione)
void core_gpio_log_pin_edges(void);          // log seriale su transizioni HIGH/LOW di D1 e D2
bool core_gpio_is_rec_start_active(void);  // pin attivazione configurato LOW (runtime)
bool core_gpio_is_rec_stop_active(void);   // pin stop configurato LOW (ignorato se GPIO29 HIGH)
void core_gpio_signal_tpl_done(void); // GPIO23 HIGH permanente
void core_gpio_request_shutdown(void); // esci config / avvia sequenza DONE TPL
void core_gpio_log_inputs(void);
void core_gpio_hold_tpl_done(uint32_t delay_ms); // delay + DONE permanente (non ritorna)

#ifdef __cplusplus
}
#endif
