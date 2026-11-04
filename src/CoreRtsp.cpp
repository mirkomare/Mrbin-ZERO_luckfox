#include "CoreRtsp.h"

#include "CoreConfig.h"
#include "CoreDemo.h"
#include "CoreIspCal.h"
#include "CoreLive.h"
#include "CoreVideo.h"

#include "hal_capture.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static const char *TAG = "core_rtsp";
static void remember(const core_settings_t *settings);

#ifdef LUCKFOX_RKMPI
void hal_capture_user_request(void);
#endif

#define RTSP_PORT_PRIMARY 554
#define RTSP_PORT_FALLBACK 8554
#define RTSP_MAX_CLIENTS 3
#define RTSP_PAYLOAD 1200
#define RTSP_SSRC 0x4d52424eu

static SemaphoreHandle_t s_mu = nullptr;
static core_settings_t s_cfg;
static int s_hold = 0;
static int s_rec = 0;
static int s_always = 0;
static int s_playing = 0;
static int s_opening = 0;
static int s_pipe_up = 0;
static int s_gen = 1;
static int s_sub_gen = 1;
static int s_applied = 0;
static int s_port = RTSP_PORT_PRIMARY;
static int s_started = 0;
static volatile int s_run = 0;

#ifndef _WIN32
typedef struct {
    int fd;
    int udp;
    int playing;
    int dead;
    int tcp_rtp;
    int ch;
    uint16_t seq;
    uint32_t ts;
    uint32_t gen;
    char acc[2048];
    int acc_n;
    struct sockaddr_in peer;
} rtsp_client_t;

static rtsp_client_t s_cli[RTSP_MAX_CLIENTS];
static int s_listen = -1;
static SemaphoreHandle_t s_send_mu = nullptr;
static esp_capture_handle_t s_cap = nullptr;
static esp_capture_sink_handle_t s_sink = nullptr;
static esp_capture_video_src_if_t *s_vsrc = nullptr;

static void lock_mu(void) {
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock_mu(void) {
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

static int send_all(int fd, const void *data, int n) {
    const uint8_t *p = (const uint8_t *)data;
    int off = 0;
    while (off < n) {
        int w = (int)send(fd, p + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        off += w;
    }
    return 0;
}

static void local_ip(char *out, size_t n) {
    snprintf(out, n, "0.0.0.0");
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    const char *names[] = {"eth0", "wlan0"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, names[i], IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
            if (a->sin_addr.s_addr != 0) {
                inet_ntop(AF_INET, &a->sin_addr, out, n);
                break;
            }
        }
    }
    close(fd);
}

static const char *hdr_value(const char *msg, const char *key, char *buf, size_t buflen) {
    size_t klen = strlen(key);
    const char *p = msg;
    while (*p) {
        if (strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i + 1 < buflen) {
                buf[i++] = *p++;
            }
            buf[i] = 0;
            return buf;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    buf[0] = 0;
    return buf;
}

static int cseq_of(const char *msg) {
    char buf[32];
    hdr_value(msg, "CSeq", buf, sizeof(buf));
    return atoi(buf);
}

static int path_ok(const char *msg) {
    const char *sp = strchr(msg, ' ');
    if (!sp) {
        return 1;
    }
    sp++;
    if (strncmp(sp, "rtsp://", 7) == 0) {
        const char *slash = strchr(sp + 7, '/');
        sp = slash ? slash : sp;
    }
    if (*sp == '*') {
        return 1;
    }
    return strstr(sp, "ch1") != nullptr || strstr(sp, "ch2") != nullptr ||
           strstr(sp, "live") != nullptr || strstr(sp, "track") != nullptr;
}

static int request_channel(const char *msg) {
    const char *sp = strchr(msg, ' ');
    if (!sp) {
        return 1;
    }
    sp++;
    if (strncmp(sp, "rtsp://", 7) == 0) {
        const char *slash = strchr(sp + 7, '/');
        sp = slash ? slash : sp;
    }
    if (strstr(sp, "ch2") != nullptr) {
        return 2;
    }
    return 1;
}

static void reply(rtsp_client_t *c, int code, const char *reason, const char *extra, const char *body) {
    char head[1024];
    int blen = body ? (int)strlen(body) : 0;
    int n = snprintf(head, sizeof(head),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "Server: MrBin\r\n"
                     "%s"
                     "Content-Length: %d\r\n"
                     "\r\n",
                     code, reason, cseq_of(c->acc), extra ? extra : "", blen);
    if (n < 0 || n >= (int)sizeof(head)) {
        return;
    }
    if (!s_send_mu || xSemaphoreTake(s_send_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (send_all(c->fd, head, n) == 0 && blen > 0) {
        (void)send_all(c->fd, body, blen);
    }
    xSemaphoreGive(s_send_mu);
}

static void client_mark_dead(rtsp_client_t *c) {
    lock_mu();
    if (c->playing) {
        c->playing = 0;
        if (s_playing > 0) {
            s_playing--;
        }
    }
    c->dead = 1;
    unlock_mu();
}

static int parse_client_port(const char *transport) {
    const char *p = strstr(transport, "client_port=");
    if (!p) {
        return 0;
    }
    return atoi(p + 12);
}

static void on_request(rtsp_client_t *c) {
    char *msg = c->acc;
    char method[16] = {0};
    sscanf(msg, "%15s", method);
    char extra[512];
    extra[0] = 0;

    if (strcmp(method, "OPTIONS") == 0) {
        snprintf(extra, sizeof(extra),
                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n");
        reply(c, 200, "OK", extra, nullptr);
        return;
    }
    if (!path_ok(msg) && strcmp(method, "TEARDOWN") != 0) {
        reply(c, 404, "Not Found", nullptr, nullptr);
        return;
    }
    if (strcmp(method, "DESCRIBE") == 0) {
        int ch = request_channel(msg);
        c->ch = ch;
        const char *base = (ch == 2) ? "ch2" : (strstr(msg, "ch1") ? "ch1" : "live");
        char sdp[512];
        int slen = snprintf(sdp, sizeof(sdp),
                            "v=0\r\n"
                            "o=- 0 0 IN IP4 0.0.0.0\r\n"
                            "s=MrBin\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n"
                            "a=fmtp:96 packetization-mode=1\r\n"
                            "a=control:track1\r\n");
        snprintf(extra, sizeof(extra),
                 "Content-Base: rtsp://0.0.0.0:%d/%s/\r\n"
                 "Content-Type: application/sdp\r\n",
                 s_port, base);
        if (slen < 0) {
            slen = 0;
            sdp[0] = 0;
        }
        reply(c, 200, "OK", extra, sdp);
        return;
    }
    if (strcmp(method, "SETUP") == 0) {
        c->ch = request_channel(msg);
        char transport[256];
        hdr_value(msg, "Transport", transport, sizeof(transport));
        int tcp = strstr(transport, "TCP") != nullptr;
        int cport = parse_client_port(transport);
        if (!tcp) {
            if (cport <= 0) {
                reply(c, 461, "Unsupported Transport", nullptr, nullptr);
                return;
            }
            int udp = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp < 0) {
                reply(c, 500, "Internal Server Error", nullptr, nullptr);
                return;
            }
            struct sockaddr_in dst = c->peer;
            dst.sin_port = htons((uint16_t)cport);
            if (connect(udp, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
                close(udp);
                reply(c, 500, "Internal Server Error", nullptr, nullptr);
                return;
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            setsockopt(udp, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (c->udp >= 0) {
                close(c->udp);
            }
            c->udp = udp;
            c->tcp_rtp = 0;
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n"
                     "Session: 1\r\n",
                     cport, cport + 1);
        } else {
            c->tcp_rtp = 1;
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                     "Session: 1\r\n");
        }
        reply(c, 200, "OK", extra, nullptr);
        return;
    }
    if (strcmp(method, "PLAY") == 0) {
        c->ch = request_channel(msg);
        if (!s_always) {
            reply(c, 453, "Not Enough Bandwidth", nullptr, nullptr);
            ESP_LOGI(TAG, "PLAY rifiutato: RTSP spento");
            return;
        }
        lock_mu();
        if (!c->playing && !c->dead) {
            c->playing = 1;
            s_playing++;
        }
        uint16_t seq = c->seq;
        uint32_t ts = c->ts;
        unlock_mu();
        snprintf(extra, sizeof(extra),
                 "Session: 1\r\n"
                 "RTP-Info: url=rtsp://0.0.0.0:%d/%s/track1;seq=%u;rtptime=%u\r\n",
                 s_port, c->ch == 2 ? "ch2" : "live", (unsigned)seq, (unsigned)ts);
        reply(c, 200, "OK", extra, nullptr);
        ESP_LOGI(TAG, "Client in play ch%d (%s)", c->ch, c->tcp_rtp ? "tcp" : "udp");
        return;
    }
    if (strcmp(method, "TEARDOWN") == 0) {
        reply(c, 200, "OK", "Session: 1\r\n", nullptr);
        client_mark_dead(c);
        return;
    }
    if (strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0) {
        reply(c, 200, "OK", nullptr, nullptr);
        return;
    }
    reply(c, 405, "Method Not Allowed", nullptr, nullptr);
}

static void client_consume(rtsp_client_t *c) {
    for (;;) {
        char *end = strstr(c->acc, "\r\n\r\n");
        if (!end) {
            return;
        }
        int header = (int)(end - c->acc) + 4;
        int body = 0;
        char cl[32];
        c->acc[header - 1] = 0;
        hdr_value(c->acc, "Content-Length", cl, sizeof(cl));
        c->acc[header - 1] = '\n';
        body = atoi(cl);
        if (body < 0) {
            body = 0;
        }
        if (c->acc_n < header + body) {
            return;
        }
        char saved = c->acc[header];
        c->acc[header] = 0;
        on_request(c);
        c->acc[header] = saved;
        int used = header + body;
        memmove(c->acc, c->acc + used, (size_t)(c->acc_n - used));
        c->acc_n -= used;
        c->acc[c->acc_n] = 0;
    }
}

static void close_client_fds(rtsp_client_t *c) {
    if (s_send_mu) {
        xSemaphoreTake(s_send_mu, portMAX_DELAY);
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    if (c->udp >= 0) {
        close(c->udp);
        c->udp = -1;
    }
    if (s_send_mu) {
        xSemaphoreGive(s_send_mu);
    }
}

static void reap_dead(void) {
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        lock_mu();
        int dead = s_cli[i].dead && s_cli[i].fd >= 0;
        unlock_mu();
        if (!dead) {
            continue;
        }
        close_client_fds(&s_cli[i]);
        lock_mu();
        s_cli[i].dead = 0;
        s_cli[i].playing = 0;
        s_cli[i].acc_n = 0;
        s_cli[i].tcp_rtp = 0;
        unlock_mu();
    }
}

static int accept_one(void) {
    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    int fd = accept(s_listen, (struct sockaddr *)&peer, &sl);
    if (fd < 0) {
        return -1;
    }
    int slot = -1;
    lock_mu();
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        if (s_cli[i].fd < 0 && !s_cli[i].dead) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        unlock_mu();
        close(fd);
        return -1;
    }
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 400000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&s_cli[slot], 0, sizeof(s_cli[slot]));
    s_cli[slot].fd = fd;
    s_cli[slot].udp = -1;
    s_cli[slot].peer = peer;
    s_cli[slot].gen++;
    unlock_mu();
    ESP_LOGI(TAG, "Client RTSP da %s", inet_ntoa(peer.sin_addr));
    return slot;
}

static void accept_task(void *arg) {
    (void)arg;
    while (s_run) {
        reap_dead();
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = s_listen;
        FD_SET(s_listen, &rfds);
        int fds[RTSP_MAX_CLIENTS];
        for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
            lock_mu();
            fds[i] = s_cli[i].dead ? -1 : s_cli[i].fd;
            unlock_mu();
            if (fds[i] >= 0) {
                FD_SET(fds[i], &rfds);
                if (fds[i] > maxfd) {
                    maxfd = fds[i];
                }
            }
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int ready = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno != EINTR) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }
        if (FD_ISSET(s_listen, &rfds)) {
            (void)accept_one();
        }
        for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rfds)) {
                continue;
            }
            lock_mu();
            int fd = s_cli[i].fd;
            int n = s_cli[i].acc_n;
            unlock_mu();
            if (fd < 0 || n >= (int)sizeof(s_cli[i].acc) - 1) {
                client_mark_dead(&s_cli[i]);
                continue;
            }
            int r = (int)recv(fd, s_cli[i].acc + n, sizeof(s_cli[i].acc) - 1 - (size_t)n, 0);
            if (r <= 0) {
                client_mark_dead(&s_cli[i]);
                continue;
            }
            s_cli[i].acc_n = n + r;
            s_cli[i].acc[s_cli[i].acc_n] = 0;
            client_consume(&s_cli[i]);
        }
    }
    vTaskDelete(nullptr);
}

static int next_start(const uint8_t *p, int n, int from, int *sc) {
    for (int i = from; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            *sc = 3;
            return i;
        }
        if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            *sc = 4;
            return i;
        }
    }
    return -1;
}

static int rtp_send(rtsp_client_t *c, const uint8_t *pkt, int n, int marker, uint16_t seq, uint32_t ts) {
    uint8_t buf[1600];
    if (n + 16 > (int)sizeof(buf)) {
        return -1;
    }
    uint8_t *rtp = buf;
    int off = 0;
    if (c->tcp_rtp) {
        rtp = buf + 4;
        off = 4;
    }
    rtp[0] = 0x80;
    rtp[1] = (uint8_t)((marker ? 0x80 : 0) | 96);
    rtp[2] = (uint8_t)(seq >> 8);
    rtp[3] = (uint8_t)seq;
    rtp[4] = (uint8_t)(ts >> 24);
    rtp[5] = (uint8_t)(ts >> 16);
    rtp[6] = (uint8_t)(ts >> 8);
    rtp[7] = (uint8_t)ts;
    uint32_t ssrc = RTSP_SSRC;
    rtp[8] = (uint8_t)(ssrc >> 24);
    rtp[9] = (uint8_t)(ssrc >> 16);
    rtp[10] = (uint8_t)(ssrc >> 8);
    rtp[11] = (uint8_t)ssrc;
    memcpy(rtp + 12, pkt, (size_t)n);
    int total = 12 + n;
    if (c->tcp_rtp) {
        buf[0] = '$';
        buf[1] = 0;
        buf[2] = (uint8_t)(total >> 8);
        buf[3] = (uint8_t)total;
        return send_all(c->fd, buf, off + total);
    }
    if (c->udp < 0) {
        return -1;
    }
    return send_all(c->udp, rtp, total);
}

static void send_nal(rtsp_client_t *c, const uint8_t *nal, int n, int marker) {
    if (n <= 0) {
        return;
    }
    uint8_t fu[RTSP_PAYLOAD];
    if (n <= RTSP_PAYLOAD) {
        if (rtp_send(c, nal, n, marker, c->seq, c->ts) != 0) {
            client_mark_dead(c);
            return;
        }
        c->seq++;
        return;
    }
    uint8_t ind = (uint8_t)((nal[0] & 0x60) | 28);
    uint8_t typ = (uint8_t)(nal[0] & 0x1f);
    int off = 1;
    int left = n - 1;
    while (left > 0 && !c->dead) {
        int chunk = left > (RTSP_PAYLOAD - 2) ? (RTSP_PAYLOAD - 2) : left;
        int last = chunk == left;
        fu[0] = ind;
        fu[1] = (uint8_t)((off == 1 ? 0x80 : 0) | (last ? 0x40 : 0) | typ);
        memcpy(fu + 2, nal + off, (size_t)chunk);
        if (rtp_send(c, fu, chunk + 2, marker && last, c->seq, c->ts) != 0) {
            client_mark_dead(c);
            return;
        }
        c->seq++;
        off += chunk;
        left -= chunk;
    }
}

static void broadcast_frame(const uint8_t *data, int n, uint32_t step, int channel) {
    if (!data || n <= 4 || !s_send_mu) {
        return;
    }
    rtsp_client_t snap[RTSP_MAX_CLIENTS];
    int use[RTSP_MAX_CLIENTS];
    lock_mu();
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        use[i] = s_always && s_cli[i].playing && !s_cli[i].dead && s_cli[i].fd >= 0 &&
                 s_cli[i].ch == channel;
        if (use[i]) {
            snap[i] = s_cli[i];
        }
    }
    unlock_mu();

    if (xSemaphoreTake(s_send_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        if (!use[i]) {
            continue;
        }
        int sc = 0;
        int at = next_start(data, n, 0, &sc);
        int nal_i = 0;
        int nals = 0;
        for (int j = at; j >= 0; ) {
            int sc2 = 0;
            int nx = next_start(data, n, j + sc, &sc2);
            nals++;
            if (nx < 0) {
                break;
            }
            j = nx;
            sc = sc2;
        }
        at = next_start(data, n, 0, &sc);
        while (at >= 0 && !snap[i].dead) {
            int sc2 = 0;
            int nx = next_start(data, n, at + sc, &sc2);
            int nal = at + sc;
            int nal_n = (nx < 0 ? n : nx) - nal;
            while (nal_n > 0 && data[nal + nal_n - 1] == 0) {
                nal_n--;
            }
            nal_i++;
            int marker = nal_i == nals;
            if (nal_n > 0) {
                send_nal(&snap[i], data + nal, nal_n, marker);
            }
            if (nx < 0) {
                break;
            }
            at = nx;
            sc = sc2;
        }
        snap[i].ts += step;
    }
    xSemaphoreGive(s_send_mu);

    lock_mu();
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        if (!use[i] || s_cli[i].fd != snap[i].fd) {
            continue;
        }
        s_cli[i].seq = snap[i].seq;
        s_cli[i].ts = snap[i].ts;
        if (snap[i].dead && s_cli[i].playing) {
            s_cli[i].playing = 0;
            s_cli[i].dead = 1;
            if (s_playing > 0) {
                s_playing--;
            }
        }
    }
    unlock_mu();
}

static int pipe_wanted(void) {
    /* Un client non accende l'encoder. Il pulsante accende entrambi i canali.
     * La registrazione tiene il canale 1 anche con RTSP spento. */
    return s_always || s_rec;
}

static int pipe_blocked(void) {
    int blocked;
    lock_mu();
    blocked = s_hold > 0 || !pipe_wanted();
    unlock_mu();
    return blocked;
}

static void pipe_close(void) {
    if (s_cap) {
        esp_capture_stop(s_cap);
        esp_capture_close(s_cap);
        s_cap = nullptr;
        s_sink = nullptr;
    } else {
        esp_capture_release_leftover();
    }
    if (s_vsrc) {
        free(s_vsrc);
        s_vsrc = nullptr;
    }
    esp_video_enc_unregister_default();
    if (!hal_preview_running() && !hal_sub_running() && !hal_live_running()) {
        core_video_deinit();
        core_video_isp_stop_3a();
        hal_mark_3a_down();
    }
    lock_mu();
    s_pipe_up = 0;
    s_opening = 0;
    unlock_mu();
}

static int pipe_open(void) {
    core_settings_t cfg;
    lock_mu();
    cfg = s_cfg;
    int held = s_hold > 0;
    unlock_mu();
    if (held) {
        return 0;
    }
    core_settings_video_normalize(&cfg);
    if (!core_video_is_ready() && !core_video_init()) {
        ESP_LOGW(TAG, "Init camera fallita");
        return 0;
    }
    if (!core_video_prepare_for_settings(&cfg)) {
        ESP_LOGW(TAG, "Preparazione sensore fallita");
        core_video_deinit();
        return 0;
    }
    if (pipe_blocked()) {
        core_video_deinit();
        return 0;
    }
    (void)core_isp_cal_apply_active();
    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = CORE_V4L2_BUF_COUNT,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);
    s_vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!s_vsrc) {
        esp_video_enc_unregister_default();
        core_video_deinit();
        return 0;
    }
    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = s_vsrc,
        .share_overlay = false,
    };
    if (esp_capture_open(&cap_cfg, &s_cap) != ESP_CAPTURE_ERR_OK || !s_cap) {
        free(s_vsrc);
        s_vsrc = nullptr;
        esp_video_enc_unregister_default();
        core_video_deinit();
        return 0;
    }
    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_H264,
            .width = cfg.video_width,
            .height = cfg.video_height,
            .fps = cfg.video_fps,
        },
    };
    if (esp_capture_sink_setup(s_cap, 0, &sink_cfg, &s_sink) != ESP_CAPTURE_ERR_OK) {
        pipe_close();
        return 0;
    }
    uint32_t bitrate = core_settings_video_bitrate(&cfg);
    uint32_t gop = core_settings_video_gop(&cfg);
    (void)esp_capture_sink_set_bitrate(s_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, bitrate);
    esp_capture_sink_enable(s_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    (void)esp_gmf_video_enc_set_gop(nullptr, gop);
#ifdef LUCKFOX_RKMPI
    hal_capture_user_request();
#endif
    if (esp_capture_start(s_cap) != ESP_CAPTURE_ERR_OK) {
        const char *mpi = esp_capture_last_mpi_error();
        ESP_LOGE(TAG, "VENC H.264 %ux%u@%u non partito%s%s",
                 (unsigned)cfg.video_width, (unsigned)cfg.video_height, (unsigned)cfg.video_fps,
                 (mpi && mpi[0]) ? ": " : "", (mpi && mpi[0]) ? mpi : "");
        pipe_close();
        return 0;
    }
    if (pipe_blocked()) {
        pipe_close();
        return 0;
    }
    core_video_reapply_flip();
    core_video_isp_live_after_vi();
    (void)core_isp_cal_apply_active();
    ESP_LOGI(TAG, "Canale 1 H.264 %ux%u@%u su rtsp://0.0.0.0:%d/ch1",
             (unsigned)cfg.video_width, (unsigned)cfg.video_height, (unsigned)cfg.video_fps, s_port);
    return 1;
}

#define RTSP_REC_Q 15
typedef struct {
    uint8_t *data;
    int len;
    uint32_t pts;
} rec_slot_t;
static rec_slot_t s_rq[RTSP_REC_Q];
static int s_rq_n;
static int s_rq_r;
static int s_rq_w;
static SemaphoreHandle_t s_rq_mu;
static SemaphoreHandle_t s_rq_sem;

static void rec_q_init(void) {
    if (!s_rq_mu) {
        s_rq_mu = xSemaphoreCreateMutex();
    }
    if (!s_rq_sem) {
        s_rq_sem = xSemaphoreCreateCounting(RTSP_REC_Q, 0);
    }
}

static void rec_q_flush(void) {
    if (!s_rq_mu) {
        return;
    }
    xSemaphoreTake(s_rq_mu, portMAX_DELAY);
    while (s_rq_n > 0) {
        free(s_rq[s_rq_r].data);
        s_rq[s_rq_r].data = nullptr;
        s_rq_r = (s_rq_r + 1) % RTSP_REC_Q;
        s_rq_n--;
    }
    s_rq_r = 0;
    s_rq_w = 0;
    xSemaphoreGive(s_rq_mu);
    if (s_rq_sem) {
        while (xSemaphoreTake(s_rq_sem, 0) == pdTRUE) {
        }
    }
}

static void rec_offer(const uint8_t *data, int n, uint32_t pts) {
    if (!data || n <= 0) {
        return;
    }
    rec_q_init();
    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy) {
        return;
    }
    memcpy(copy, data, (size_t)n);
    lock_mu();
    int want = s_rec;
    unlock_mu();
    if (!want) {
        free(copy);
        return;
    }
    xSemaphoreTake(s_rq_mu, portMAX_DELAY);
    int dropped = 0;
    if (s_rq_n == RTSP_REC_Q) {
        free(s_rq[s_rq_r].data);
        s_rq[s_rq_r].data = nullptr;
        s_rq_r = (s_rq_r + 1) % RTSP_REC_Q;
        s_rq_n--;
        dropped = 1;
    }
    s_rq[s_rq_w].data = copy;
    s_rq[s_rq_w].len = n;
    s_rq[s_rq_w].pts = pts;
    s_rq_w = (s_rq_w + 1) % RTSP_REC_Q;
    s_rq_n++;
    xSemaphoreGive(s_rq_mu);
    if (!dropped) {
        xSemaphoreGive(s_rq_sem);
    }
}

static void pipe_task(void *arg) {
    (void)arg;
    int open_local = 0;
    while (s_run) {
        int want_open = 0;
        int gen = 0;
        lock_mu();
        int run = s_hold == 0 && pipe_wanted();
        gen = s_gen;
        if (run && (!open_local || s_applied != gen)) {
            s_opening = 1;
            want_open = 1;
        }
        unlock_mu();
        if (!run) {
            if (open_local) {
                pipe_close();
                open_local = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (want_open) {
            if (open_local) {
                pipe_close();
                open_local = 0;
                lock_mu();
                s_opening = 1;
                unlock_mu();
            }
            int ok = pipe_open();
            lock_mu();
            s_opening = 0;
            s_pipe_up = ok ? 1 : 0;
            s_applied = gen;
            unlock_mu();
            open_local = ok;
            if (!ok) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }
        if (pipe_blocked()) {
            if (open_local) {
                pipe_close();
                open_local = 0;
            }
            continue;
        }
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        if (esp_capture_sink_acquire_frame(s_sink, &frame, true) != ESP_CAPTURE_ERR_OK ||
            !frame.data || frame.size <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t step = 3000;
        lock_mu();
        if (s_cfg.video_fps > 0) {
            step = 90000u / s_cfg.video_fps;
        }
        int rec = s_rec;
        unlock_mu();
        if (rec) {
            rec_offer(frame.data, frame.size, frame.pts);
        }
        if (s_always) {
            broadcast_frame(frame.data, frame.size, step, 1);
        }
        esp_capture_sink_release_frame(s_sink, &frame);
    }
    if (open_local) {
        pipe_close();
    }
    vTaskDelete(nullptr);
}

static void sub_stop_local(int *open_local) {
    if (open_local && *open_local) {
        hal_sub_stop();
        *open_local = 0;
    }
}

static void sub_task(void *arg) {
    (void)arg;
    int open_local = 0;
    int sub_applied = 0;
    while (s_run) {
        if (core_rec_fast_requested()) {
            sub_stop_local(&open_local);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        core_settings_t cfg;
        int gen = 0;
        int want = 0;
        int main_up = 0;
        lock_mu();
        want = s_always && s_hold == 0;
        main_up = s_pipe_up && !s_opening;
        gen = s_sub_gen;
        cfg = s_cfg;
        unlock_mu();
        if (!want || !main_up) {
            /* Il canale 1 (anche 8 MP) deve allocare la CMA prima del selfpath.
             * Se il canale 2 parte prima, il VENC grande fallisce e il teardown
             * blocca il processo. */
            sub_stop_local(&open_local);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (hal_preview_running()) {
            sub_stop_local(&open_local);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint16_t w = cfg.ch2_width ? cfg.ch2_width : 1280;
        uint16_t h = cfg.ch2_height ? cfg.ch2_height : 720;
        uint8_t fps = cfg.ch2_fps ? cfg.ch2_fps : 15;
        if (!core_settings_video_valid(w, h, fps)) {
            w = 1280;
            h = 720;
            fps = 15;
        }
        if (!open_local || sub_applied != gen) {
            sub_stop_local(&open_local);
            uint32_t br = core_settings_bitrate_for(w, h, fps);
            if (hal_sub_start((int)w, (int)h, (int)fps, br) != 0) {
                if (!(w == 640 && h == 360)) {
                    ESP_LOGW(TAG, "Canale 2 %ux%u non parte, riprovo 640x360",
                             (unsigned)w, (unsigned)h);
                    br = core_settings_bitrate_for(640, 360, fps);
                    if (hal_sub_start(640, 360, (int)fps, br) == 0) {
                        open_local = 1;
                        sub_applied = gen;
                        ESP_LOGI(TAG, "Canale 2 RTSP 640x360@%u", (unsigned)fps);
                        continue;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            open_local = 1;
            sub_applied = gen;
            ESP_LOGI(TAG, "Canale 2 RTSP %ux%u@%u", (unsigned)w, (unsigned)h, (unsigned)fps);
            continue;
        }
        uint8_t *data = nullptr;
        int n = 0;
        if (hal_sub_acquire(&data, &n, 200) != 0 || !data || n <= 0) {
            free(data);
            continue;
        }
        uint32_t step = fps > 0 ? (90000u / fps) : 6000u;
        broadcast_frame(data, n, step, 2);
        free(data);
    }
    sub_stop_local(&open_local);
    vTaskDelete(nullptr);
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif /* !_WIN32 */

void core_rtsp_url_ch(int ch, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    const char *path = (ch == 2) ? "ch2" : "ch1";
#ifndef _WIN32
    char ip[64];
    local_ip(ip, sizeof(ip));
    snprintf(out, out_len, "rtsp://%s:%d/%s", ip, s_port, path);
#else
    snprintf(out, out_len, "rtsp://127.0.0.1:%d/%s", RTSP_PORT_PRIMARY, path);
#endif
}

void core_rtsp_url(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
#ifndef _WIN32
    char ip[64];
    local_ip(ip, sizeof(ip));
    snprintf(out, out_len, "rtsp://%s:%d/live", ip, s_port);
#else
    snprintf(out, out_len, "rtsp://127.0.0.1:%d/live", RTSP_PORT_PRIMARY);
#endif
}

void core_rtsp_hold(void) {
#ifndef _WIN32
    if (!s_mu) {
        s_hold++;
        return;
    }
    lock_mu();
    s_hold++;
    unlock_mu();
    for (int i = 0; i < 200; ++i) {
        lock_mu();
        int busy = s_opening || s_pipe_up;
        unlock_mu();
        if (!busy) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "Camera RTSP ancora occupata dopo l'attesa");
#else
    s_hold++;
#endif
}

void core_rtsp_unhold_all(void) {
#ifndef _WIN32
    if (!s_mu) {
        s_hold = 0;
        return;
    }
    lock_mu();
    s_hold = 0;
    unlock_mu();
#else
    s_hold = 0;
#endif
}

int core_rtsp_pipe_up(void) {
#ifndef _WIN32
    if (!s_mu) {
        return s_pipe_up;
    }
    lock_mu();
    int up = s_pipe_up && !s_opening;
    unlock_mu();
    return up;
#else
    return 0;
#endif
}

int core_rtsp_pipe_busy(void) {
#ifndef _WIN32
    if (!s_mu) {
        return s_opening || s_pipe_up;
    }
    lock_mu();
    int busy = s_opening || s_pipe_up;
    unlock_mu();
    return busy;
#else
    return 0;
#endif
}

void core_rtsp_release(void) {
#ifndef _WIN32
    if (!s_mu) {
        if (s_hold > 0) {
            s_hold--;
        }
        return;
    }
    lock_mu();
    if (s_hold > 0) {
        s_hold--;
    }
    unlock_mu();
#else
    if (s_hold > 0) {
        s_hold--;
    }
#endif
}

int core_rtsp_rec_attach(const core_settings_t *settings) {
#ifndef _WIN32
    if (!s_mu || !settings) {
        return -1;
    }
    rec_q_init();
    remember(settings);
    lock_mu();
    s_rec = 1;
    int gen = s_gen;
    unlock_mu();
    for (int i = 0; i < 200; ++i) {
        lock_mu();
        int up = s_pipe_up && s_applied == gen && s_opening == 0;
        unlock_mu();
        if (up) {
            rec_q_flush();
            hal_capture_request_idr();
            ESP_LOGI(TAG, "Registrazione agganciata allo streamer %ux%u@%u",
                     (unsigned)settings->video_width, (unsigned)settings->video_height,
                     (unsigned)settings->video_fps);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    lock_mu();
    s_rec = 0;
    unlock_mu();
    ESP_LOGE(TAG, "Streamer non pronto per la registrazione");
    return -1;
#else
    (void)settings;
    return -1;
#endif
}

void core_rtsp_rec_detach(void) {
#ifndef _WIN32
    if (s_mu) {
        lock_mu();
        s_rec = 0;
        unlock_mu();
    } else {
        s_rec = 0;
    }
    rec_q_flush();
#else
    s_rec = 0;
#endif
}

int core_rtsp_rec_take(uint8_t **data, int *len, uint32_t *pts, int timeout_ms) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    if (pts) {
        *pts = 0;
    }
#ifndef _WIN32
    if (!data || !len || !s_rq_sem) {
        return -1;
    }
    TickType_t ticks = timeout_ms <= 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_rq_sem, ticks) != pdTRUE) {
        return -1;
    }
    xSemaphoreTake(s_rq_mu, portMAX_DELAY);
    if (s_rq_n <= 0) {
        xSemaphoreGive(s_rq_mu);
        return -1;
    }
    *data = s_rq[s_rq_r].data;
    *len = s_rq[s_rq_r].len;
    if (pts) {
        *pts = s_rq[s_rq_r].pts;
    }
    s_rq[s_rq_r].data = nullptr;
    s_rq_r = (s_rq_r + 1) % RTSP_REC_Q;
    s_rq_n--;
    xSemaphoreGive(s_rq_mu);
    return (*data && *len > 0) ? 0 : -1;
#else
    (void)timeout_ms;
    return -1;
#endif
}

static void remember(const core_settings_t *settings) {
    if (!settings) {
        return;
    }
    core_settings_t next = *settings;
    core_settings_video_normalize(&next);
#ifndef _WIN32
    lock_mu();
#endif
    int changed = s_cfg.video_width != next.video_width || s_cfg.video_height != next.video_height ||
                  s_cfg.video_fps != next.video_fps;
    int ch2_changed = s_cfg.ch2_width != next.ch2_width || s_cfg.ch2_height != next.ch2_height ||
                      s_cfg.ch2_fps != next.ch2_fps;
    s_cfg = next;
    s_always = next.rtsp_always ? 1 : 0;
    if (changed) {
        s_gen++;
    }
    if (ch2_changed) {
        s_sub_gen++;
    }
#ifndef _WIN32
    unlock_mu();
#endif
}

void core_rtsp_apply(const core_settings_t *settings) {
    remember(settings);
#ifndef _WIN32
    if (s_always) {
        if (!core_demo_is_active() && core_live_is_running()) {
            core_live_force_release();
        }
        ESP_LOGI(TAG, "RTSP acceso: canale 1 e canale 2");
    } else {
        ESP_LOGI(TAG, "RTSP spento");
    }
#endif
}

void core_rtsp_start(const core_settings_t *settings) {
    if (s_started) {
        core_rtsp_apply(settings);
        return;
    }
    s_started = 1;
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
    remember(settings);
#ifndef _WIN32
    if (!s_send_mu) {
        s_send_mu = xSemaphoreCreateMutex();
    }
    for (int i = 0; i < RTSP_MAX_CLIENTS; ++i) {
        s_cli[i].fd = -1;
        s_cli[i].udp = -1;
    }
    s_listen = listen_on(RTSP_PORT_PRIMARY);
    s_port = RTSP_PORT_PRIMARY;
    if (s_listen < 0) {
        s_listen = listen_on(RTSP_PORT_FALLBACK);
        s_port = RTSP_PORT_FALLBACK;
    }
    if (s_listen < 0) {
        ESP_LOGE(TAG, "Bind RTSP fallito su %d e %d", RTSP_PORT_PRIMARY, RTSP_PORT_FALLBACK);
        return;
    }
    s_run = 1;
    xTaskCreate(accept_task, "rtsp_acc", 8192, nullptr, 3, nullptr);
    xTaskCreate(pipe_task, "rtsp_pipe", 32768, nullptr, 4, nullptr);
    xTaskCreate(sub_task, "rtsp_sub", 16384, nullptr, 4, nullptr);
    char url[80];
    core_rtsp_url_ch(1, url, sizeof(url));
    ESP_LOGI(TAG, "RTSP in ascolto: %s e /ch2 (%s)", url, s_always ? "acceso" : "spento");
#else
    (void)settings;
#endif
}
