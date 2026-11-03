#include "hal_time_sntp.h"
#include "hal_video.h"
#include "hal_encfs_vfs.h"
#include "CoreTime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#else
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

static int s_sntp;
static char s_sntp_srv[2][64];
static void (*s_sntp_cb)(struct timeval *);

int esp_sntp_enabled(void) { return s_sntp; }
void esp_sntp_stop(void) { s_sntp = 0; }
void esp_sntp_setoperatingmode(int) {}
void esp_sntp_setservername(int idx, const char *name) {
    if (idx >= 0 && idx < 2 && name) snprintf(s_sntp_srv[idx], 64, "%s", name);
}
void esp_sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *tv)) { s_sntp_cb = cb; }

#ifndef _WIN32
/* Minimal SNTP client — ntpdate/timedatectl are not on the fast ramdisk. */
static bool ntp_query_once(const char *server, struct timeval *out) {
    if (!server || !server[0] || !out) return false;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(server, "123", &hints, &res) != 0 || !res) {
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }
    struct timeval tv_to = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_to, sizeof(tv_to));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_to, sizeof(tv_to));

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1b; /* LI=0 VN=3 Mode=3 (client) */

    bool ok = false;
    if (sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof(pkt)) {
        uint8_t reply[48];
        ssize_t n = recvfrom(fd, reply, sizeof(reply), 0, nullptr, nullptr);
        if (n >= 48) {
            uint32_t sec = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) |
                           ((uint32_t)reply[42] << 8) | (uint32_t)reply[43];
            /* NTP epoch 1900 → Unix 1970 */
            const uint32_t ntp_unix = 2208988800u;
            if (sec > ntp_unix) {
                out->tv_sec = (time_t)(sec - ntp_unix);
                out->tv_usec = 0;
                ok = out->tv_sec >= 1704067200; /* >= 2024-01-01 */
            }
        }
    }
    close(fd);
    freeaddrinfo(res);
    return ok;
}

static bool ntp_sync_now(void) {
    struct timeval tv;
    const char *servers[4];
    int n = 0;
    if (s_sntp_srv[0][0]) servers[n++] = s_sntp_srv[0];
    if (s_sntp_srv[1][0]) servers[n++] = s_sntp_srv[1];
    servers[n++] = "pool.ntp.org";
    servers[n++] = "time.google.com";

    for (int i = 0; i < n; ++i) {
        if (!ntp_query_once(servers[i], &tv)) continue;
        if (settimeofday(&tv, nullptr) != 0) continue;
        if (s_sntp_cb) s_sntp_cb(&tv);
        return true;
    }
    return false;
}
#endif

void esp_sntp_init(void) {
    s_sntp = 1;
#ifndef _WIN32
    if (!ntp_sync_now()) {
        /* Leave wall clock unchanged; core_time_sync_sntp will time out. */
    }
#else
    if (s_sntp_cb) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        s_sntp_cb(&tv);
    }
#endif
}

esp_err_t esp_video_init_with_flags(const esp_video_init_config_t *, int) { return ESP_OK; }
esp_err_t esp_video_deinit(void) { return ESP_OK; }
esp_err_t esp_video_isp_pipeline_set_agc_status(int) { return ESP_OK; }
esp_err_t esp_video_isp_pipeline_set_ipa_config(const esp_ipa_config_t *) { return ESP_OK; }

static esp_ipa_agc_config_t s_agc = { 70, 20, 120 };
static esp_ipa_config_t s_ipa = { &s_agc };
const esp_ipa_config_t *esp_ipa_pipeline_get_config(const char *) { return &s_ipa; }

static esp_cam_sensor_format_t s_imx[3] = {
    { "IMX708_1920x1080", 0, 0, 0, 1920, 1080, nullptr, 0, 28, nullptr, {} },
    { "IMX708_1280x720", 0, 0, 0, 1280, 720, nullptr, 0, 30, nullptr, {} },
    { "IMX708_1024x768", 0, 0, 0, 1024, 768, nullptr, 0, 30, nullptr, {} },
};

const esp_cam_sensor_format_t *imx708_format_by_index(size_t index) {
    return index < 3 ? &s_imx[index] : nullptr;
}

esp_err_t esp_vfs_register(const char *base, const esp_vfs_t *, void *) {
    ESP_LOGI("encfs", "VFS %s: su Linux il muxer usa enc_writer diretto (stesso MRBI+AES)",
             base ? base : "?");
    return ESP_OK;
}
