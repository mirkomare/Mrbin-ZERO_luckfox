#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_LED_OFF = 0,
    CORE_LED_STA_CONNECTED,  // 3 lampeggi da 1 s appena c'è l'IP, poi OFF
    CORE_LED_AP,             // lampeggio per tutto il tempo in AP
    CORE_LED_ERROR,          // lampeggio rapido 100 ms continuo
    CORE_LED_RECORDING,      // acceso fisso solo mentre registra
    CORE_LED_D2_DETECT,      // 2 lampeggi su D2 LOW, poi ripristino (non RECORDING se stop)
    CORE_LED_SAVING,         // lampeggio veloce mentre chiude il file
} core_status_led_mode_t;

bool core_status_led_init(void);
void core_status_led_set_mode(core_status_led_mode_t mode);
core_status_led_mode_t core_status_led_current(void);
void core_status_led_notify_sta_connected(void);
void core_status_led_notify_d2_detected(void);
void core_status_led_notify_rec_stop(void);   // stop D2/web: spegni subito, no ripristino RECORDING
void core_status_led_notify_sd_ready(void);   // 1 lampeggio quando SD montata durante registrazione
void core_status_led_notify_rec_sd_writing(bool writing);  // RECORDING: lampeggio mentre JOB2 scrive su SD
void core_status_led_end_recording(bool config_session);  // fine sessione: OFF (PIR) o ripristino AP/STA

#ifdef __cplusplus
}
#endif
