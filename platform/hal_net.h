#pragma once

#include "hal_log.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PS_NONE 0
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP  2
#define WIFI_IF_STA   0
#define WIFI_IF_AP    1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_EVENT    ((void *)1)
#define IP_EVENT      ((void *)2)
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_EVENT_STA_CONNECTED 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define ESP_EVENT_ANY_ID (-1)
#define ESP_MAC_WIFI_STA 0

typedef void *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
} esp_netif_ip_info_t;

typedef void esp_netif_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 0 }

typedef struct {
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
    } sta;
    struct {
        uint8_t ssid[32];
        uint8_t ssid_len;
        uint8_t channel;
        uint8_t max_connection;
        uint8_t password[64];
        int authmode;
    } ap;
} wifi_config_t;

esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t cb, void *arg, void *inst);
void *esp_netif_create_default_wifi_sta(void);
void *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip);

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *cfg);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_ps(int ps);
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n);
esp_err_t esp_netif_dhcpc_start(esp_netif_t *n);
esp_err_t esp_wifi_get_mac(int iface, uint8_t mac[6]);
esp_err_t esp_read_mac(uint8_t mac[6], int type);

/* Pico Zero: radio solo se il core ha chiamato hal_wifi_allow(true) (CONFIG o RTC). */
void hal_wifi_allow(bool on);
bool hal_wifi_allowed(void);
void hal_wifi_radio_down(void);
bool hal_wifi_up_for_scan(void);
bool hal_wifi_sta_apply(const char *ssid, const char *pass);
/* Elenco SSID separati da newline. -1 se la radio non è ammessa. */
int hal_wifi_scan(char *out, size_t out_len);

bool hal_net_sta_has_ip(void);
/* Refresh s_has_ip / s_ip_addr from eth0 or wlan0 (mrbin-init may already own eth). */
bool hal_net_refresh_ip(void);
bool hal_net_bring_up(const char *ssid, const char *pass, uint32_t core_id, bool want_ap);
void hal_net_shutdown(void);
int hal_net_read_eth_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
