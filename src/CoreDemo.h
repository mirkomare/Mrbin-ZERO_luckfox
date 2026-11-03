#pragma once

#include "CoreSettings.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Jumper DEMO: GPIO1_D3 linux 59 → GND, MODE CONFIG. Non GPIO2_A0 (bus TF). */
bool core_gpio_is_demo_jumper(void);

void core_demo_latch_from_boot(bool mode_config);
bool core_demo_is_active(void);

/* CONFIG: streamer sempre on, preview ch.2, registrazione su movimento (ch.1 H.264). */
void core_demo_config_start(const core_settings_t *settings);

const char *core_demo_html_banner(void);

/* Stop da Live / GUI: blocca riavvio movimento DEMO (cooldown lungo). */
void core_demo_notify_external_stop(void);

/* true: RTSP acceso e movimento in pausa. Non persiste al riavvio. */
bool core_demo_rtsp_view(void);
bool core_demo_set_rtsp_view(bool on, const core_settings_t *settings);

#ifdef __cplusplus
}
#endif
