#include "hal_net.h"
#include "CoreConfig.h"
#include "CoreStatusLed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#ifdef __linux__
#include <linux/if.h>
#include <sys/stat.h>
#endif
#endif

static volatile bool s_has_ip;
static bool s_wifi_allowed;
static esp_event_handler_t s_ip_cb;
static void *s_ip_arg;
static uint32_t s_ip_addr;
static char s_ssid[32];
static char s_pass[64];
static uint32_t s_core_id;
static int s_mode;

static int read_mac_iface(const char *iface, uint8_t mac[6]) {
#ifndef _WIN32
#ifdef __linux__
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    int r = ioctl(fd, SIOCGIFHWADDR, &ifr);
    close(fd);
    if (r != 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
#endif
#endif
    (void)iface;
    memset(mac, 0, 6);
    return -1;
}

int hal_net_read_eth_mac(uint8_t mac[6]) {
    if (read_mac_iface("eth0", mac) == 0) return 0;
    if (read_mac_iface("wlan0", mac) == 0) return 0;
    return -1;
}

static bool iface_has_ip(const char *iface, uint32_t *out) {
#ifndef _WIN32
#ifdef __linux__
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    int r = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (r != 0) return false;
    struct sockaddr_in *in = (struct sockaddr_in *)&ifr.ifr_addr;
    if (out) *out = in->sin_addr.s_addr;
    return in->sin_addr.s_addr != 0;
#endif
#endif
    (void)iface;
    (void)out;
    return false;
}

static void notify_got_ip(void) {
    s_has_ip = true;
    /* Il callback della GUI accende i 3 lampeggi. Senza callback li accende qui. */
    if (s_ip_cb) {
        s_ip_cb(s_ip_arg, IP_EVENT, IP_EVENT_STA_GOT_IP, nullptr);
    } else {
        core_status_led_notify_sta_connected();
    }
}

bool hal_net_sta_has_ip(void) { return s_has_ip; }

bool hal_net_refresh_ip(void) {
#ifndef _WIN32
#ifdef __linux__
    uint32_t ip = 0;
    if (iface_has_ip("eth0", &ip) || iface_has_ip("wlan0", &ip)) {
        s_ip_addr = ip;
        if (!s_has_ip) notify_got_ip();
        else s_has_ip = true;
        return true;
    }
#endif
#endif
    return s_has_ip && s_ip_addr != 0;
}

void hal_wifi_allow(bool on) {
    s_wifi_allowed = on;
}

bool hal_wifi_allowed(void) {
    return s_wifi_allowed;
}

void hal_wifi_radio_down(void) {
#ifndef _WIN32
#ifdef __linux__
    (void)system("wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true");
    (void)system("killall hostapd >/dev/null 2>&1 || true");
    if (access("/tmp/mrbin-dnsmasq.pid", F_OK) == 0) {
        (void)system("kill $(cat /tmp/mrbin-dnsmasq.pid) >/dev/null 2>&1 || true");
        (void)unlink("/tmp/mrbin-dnsmasq.pid");
    }
    (void)system("ip link set wlan0 down >/dev/null 2>&1 || ifconfig wlan0 down >/dev/null 2>&1 || true");
#endif
#endif
    s_has_ip = false;
}

static void hal_wifi_strip(char *s) {
    if (!s) return;
    for (char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\n' || *p == '\r' || *p == '\\') *p = ' ';
    }
}

static void hal_wifi_store(const char *ssid, const char *pass) {
#ifndef _WIN32
#ifdef __linux__
    const char *paths[] = {"/userdata/mrbin/wifi.env", "/etc/mrbin/wifi.env"};
    (void)system("mkdir -p /userdata/mrbin /etc/mrbin >/dev/null 2>&1 || true");
    for (int i = 0; i < 2; ++i) {
        FILE *fp = fopen(paths[i], "w");
        if (!fp) continue;
        fprintf(fp, "SSID=%s\nPSK=%s\n", ssid ? ssid : "", pass ? pass : "");
        fclose(fp);
        (void)chmod(paths[i], 0600);
    }
#else
    (void)ssid;
    (void)pass;
#endif
#else
    (void)ssid;
    (void)pass;
#endif
}

bool hal_wifi_up_for_scan(void) {
    if (!s_wifi_allowed) {
        hal_wifi_radio_down();
        return false;
    }
#ifndef _WIN32
#ifdef __linux__
    (void)system("ip link set wlan0 up >/dev/null 2>&1 || ifconfig wlan0 up >/dev/null 2>&1 || true");
    return access("/sys/class/net/wlan0", F_OK) == 0;
#endif
#endif
    return true;
}

bool hal_wifi_sta_apply(const char *ssid, const char *pass) {
    if (!s_wifi_allowed) {
        hal_wifi_radio_down();
        return false;
    }
    char ss[33] = {0};
    char pw[65] = {0};
    if (ssid) snprintf(ss, sizeof(ss), "%s", ssid);
    if (pass) snprintf(pw, sizeof(pw), "%s", pass);
    hal_wifi_strip(ss);
    hal_wifi_strip(pw);
    hal_wifi_store(ss, pw);
    if (ss[0] == 0) {
        return hal_wifi_up_for_scan();
    }
#ifndef _WIN32
#ifdef __linux__
    if (access("/sys/class/net/wlan0", F_OK) != 0) {
        return false;
    }
    (void)system("mkdir -p /userdata/mrbin /etc/mrbin /var/run/wpa_supplicant >/dev/null 2>&1 || true");
    const char *conf = "/userdata/mrbin/wpa.conf";
    FILE *fp = fopen(conf, "w");
    if (!fp) {
        conf = "/etc/mrbin/wpa.conf";
        fp = fopen(conf, "w");
    }
    if (!fp) {
        conf = "/tmp/mrbin-wpa.conf";
        fp = fopen(conf, "w");
    }
    if (!fp) return false;
    fprintf(fp,
            "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\nnetwork={\n"
            "  ssid=\"%s\"\n  psk=\"%s\"\n  scan_ssid=1\n}\n",
            ss, pw);
    fclose(fp);
    (void)chmod(conf, 0600);
    char cmd[256];
    (void)system("ip link set wlan0 up >/dev/null 2>&1 || ifconfig wlan0 up >/dev/null 2>&1 || true");
    (void)system("wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true");
    snprintf(cmd, sizeof(cmd), "wpa_supplicant -B -i wlan0 -c %s >/dev/null 2>&1 || true", conf);
    (void)system(cmd);
    (void)system("udhcpc -i wlan0 -b -q -t 2 -T 2 >/dev/null 2>&1 || true");
    return true;
#endif
#endif
    return true;
}

int hal_wifi_scan(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = 0;
    if (!s_wifi_allowed) return -1;
#ifndef _WIN32
#ifdef __linux__
    if (!hal_wifi_up_for_scan()) return -1;
    FILE *fp = popen("iw dev wlan0 scan 2>/dev/null | sed -n 's/.*SSID: //p'", "r");
    if (!fp) return -1;
    size_t used = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0 || used + n + 2 >= out_len) continue;
        if (used) out[used++] = '\n';
        memcpy(out + used, line, n);
        used += n;
        out[used] = 0;
    }
    (void)pclose(fp);
    return (int)used;
#endif
#endif
    snprintf(out, out_len, "HOST_SIM");
    return 0;
}

static bool hal_wifi_start_ap(uint32_t core_id) {
#ifndef _WIN32
#ifdef __linux__
    if (access("/sys/class/net/wlan0", F_OK) != 0) {
        ESP_LOGE("hal_net", "AP: wlan0 assente");
        return false;
    }
    (void)system("wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true");
    (void)system("killall hostapd >/dev/null 2>&1 || true");
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "MrBin-%05lu", (unsigned long)core_id);
    FILE *fp = fopen("/tmp/mrbin-hostapd.conf", "w");
    if (!fp) {
        return false;
    }
    fprintf(fp,
            "interface=wlan0\n"
            "driver=nl80211\n"
            "ssid=%s\n"
            "hw_mode=g\n"
            "channel=6\n"
            "wpa=2\n"
            "wpa_passphrase=12345678\n"
            "wpa_key_mgmt=WPA-PSK\n"
            "rsn_pairwise=CCMP\n",
            ssid);
    fclose(fp);
    (void)chmod("/tmp/mrbin-hostapd.conf", 0600);
    (void)system("ip link set wlan0 up >/dev/null 2>&1 || ifconfig wlan0 up >/dev/null 2>&1 || true");
    int rc = system("hostapd -B /tmp/mrbin-hostapd.conf >/tmp/mrbin-hostapd.log 2>&1");
    if (rc != 0) {
        ESP_LOGE("hal_net", "hostapd non partito (rc=%d)", rc);
        return false;
    }
    (void)system("ip addr flush dev wlan0 >/dev/null 2>&1 || true");
    (void)system("ip addr add 192.168.4.1/24 dev wlan0 >/dev/null 2>&1 || "
                 "ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up >/dev/null 2>&1 || true");
    fp = fopen("/tmp/mrbin-dnsmasq.conf", "w");
    if (fp) {
        fputs("interface=wlan0\n"
              "bind-interfaces\n"
              "port=0\n"
              "dhcp-range=192.168.4.10,192.168.4.40,255.255.255.0,12h\n",
              fp);
        fclose(fp);
        (void)system("dnsmasq -C /tmp/mrbin-dnsmasq.conf -x /tmp/mrbin-dnsmasq.pid "
                     ">/tmp/mrbin-dnsmasq.log 2>&1 || true");
    }
    ESP_LOGI("hal_net", "AP %s su 192.168.4.1", ssid);
    return true;
#else
    (void)core_id;
    return false;
#endif
#else
    (void)core_id;
    return true;
#endif
}

bool hal_net_bring_up(const char *ssid, const char *pass, uint32_t core_id, bool want_ap) {
    s_core_id = core_id;
    if (ssid) snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    if (pass) snprintf(s_pass, sizeof(s_pass), "%s", pass);

#ifndef _WIN32
#ifdef __linux__
    if (!s_wifi_allowed) {
        hal_wifi_radio_down();
        ESP_LOGI("hal_net", "WiFi spento: modalità senza radio");
        return false;
    }
    /* Pico Zero non ha eth0. Niente AP: solo stazione, o interfaccia su per lo scan. */
    (void)want_ap;
    if (!ssid || ssid[0] == 0) {
        return hal_wifi_up_for_scan();
    }
    if (!hal_wifi_sta_apply(ssid, pass)) {
        return false;
    }
    (void)hal_net_refresh_ip();
    if (!s_has_ip) {
        ESP_LOGW("hal_net", "STA avviata senza IP ancora — HTTP 0.0.0.0:%d", CORE_WEB_PORT);
    }
    return true;
#endif
#endif
    (void)want_ap;
    s_has_ip = true;
    return true;
}

void hal_net_shutdown(void) { s_has_ip = false; }

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t cb, void *arg, void *) {
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == ESP_EVENT_ANY_ID)) {
        s_ip_cb = cb;
        s_ip_arg = arg;
    }
    return ESP_OK;
}
void *esp_netif_create_default_wifi_sta(void) { return (void *)1; }
void *esp_netif_create_default_wifi_ap(void) { return (void *)2; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *) { return (esp_netif_t *)1; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *ip) {
    (void)hal_net_refresh_ip();
    if (ip) ip->ip.addr = s_ip_addr ? s_ip_addr : (s_has_ip ? 0x0100A8C0 : 0);
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { s_mode = mode; return ESP_OK; }
esp_err_t esp_wifi_set_config(int, wifi_config_t *cfg) {
    if (!cfg) return ESP_OK;
    if (s_mode == WIFI_MODE_STA) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", (char *)cfg->sta.ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", (char *)cfg->sta.password);
    } else if (s_mode == WIFI_MODE_AP) {
        unsigned long id = 0;
        if (sscanf((char *)cfg->ap.ssid, "MrBin-%lu", &id) == 1) {
            s_core_id = (uint32_t)id;
        }
    }
    return ESP_OK;
}
esp_err_t esp_wifi_start(void) {
    bool ap = (s_mode == WIFI_MODE_AP);
    return hal_net_bring_up(s_ssid, s_pass, s_core_id, ap) ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_connect(void) {
    return hal_net_bring_up(s_ssid, s_pass, s_core_id, false) ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_disconnect(void) {
#ifndef _WIN32
#ifdef __linux__
    (void)system("wpa_cli -i wlan0 disconnect 2>/dev/null || true");
#endif
#endif
    s_has_ip = false;
    return ESP_OK;
}
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *) { return ESP_OK; }
esp_err_t esp_netif_dhcpc_start(esp_netif_t *) { return ESP_OK; }
esp_err_t esp_wifi_set_ps(int) { return ESP_OK; }
esp_err_t esp_wifi_get_mac(int, uint8_t mac[6]) {
    return hal_net_read_eth_mac(mac) == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_read_mac(uint8_t mac[6], int) {
    return hal_net_read_eth_mac(mac) == 0 ? ESP_OK : ESP_FAIL;
}
