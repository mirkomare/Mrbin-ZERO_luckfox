#include "hal_httpd.h"
#include "CoreConfig.h"
#include "hal_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SOCK int
#define CLOSESOCK closesocket
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#define SOCK int
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#endif

#define MAX_URIS 64

typedef struct {
    httpd_uri_t uris[MAX_URIS];
    int n_uris;
    volatile int run;
    TaskHandle_t th;
    int port;
    SOCK listen_fd;
} httpd_srv_t;

static httpd_srv_t *s_srv;

static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, nullptr, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size) {
    if (!qry || !key || !val || !val_size) return ESP_FAIL;
    val[0] = 0;
    size_t klen = strlen(key);
    const char *p = qry;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < val_size) val[i++] = *p++;
            val[i] = 0;
            url_decode(val);
            return ESP_OK;
        }
        while (*p && *p != '&') ++p;
        if (*p == '&') ++p;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    if (r && type) snprintf(r->content_type, sizeof(r->content_type), "%s", type);
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
    if (!r || !status) return ESP_FAIL;
    r->status = atoi(status);
    if (r->status == 0) r->status = 200;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) {
    if (!r || r->extra_hdr_n >= 16) return ESP_FAIL;
    snprintf(r->extra_hdr[r->extra_hdr_n++], 160, "%s: %s", field, value);
    return ESP_OK;
}

static void resp_grow(httpd_req_t *r, size_t add) {
    size_t need = r->resp_len + add + 1;
    if (need <= r->resp_cap) return;
    size_t cap = r->resp_cap ? r->resp_cap * 2 : 4096;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(r->resp, cap);
    if (!p) return;
    r->resp = p;
    r->resp_cap = cap;
}

static void write_sock(httpd_req_t *r, const char *buf, size_t n) {
    SOCK fd = (SOCK)(intptr_t)r->sess;
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
        int w = send(fd, buf + off, (int)(n - off), 0);
#else
        ssize_t w = send(fd, buf + off, n - off, 0);
#endif
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void flush_headers(httpd_req_t *r) {
    if (r->chunked < 0) return; /* already sent */
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d OK\r\nContent-Type: %s\r\n",
                     r->status ? r->status : 200,
                     r->content_type[0] ? r->content_type : "text/html; charset=utf-8");
    write_sock(r, hdr, (size_t)n);
    for (int i = 0; i < r->extra_hdr_n; ++i) {
        write_sock(r, r->extra_hdr[i], strlen(r->extra_hdr[i]));
        write_sock(r, "\r\n", 2);
    }
    if (r->chunked) {
        /* Must be 30 bytes: "Transfer-Encoding: chunked\r\n\r\n".
         * Writing 28 left the header unterminated → browser/curl hang
         * ("Header without colon") on chunked pages such as /rec_video. */
        const char *te = "Transfer-Encoding: chunked\r\n\r\n";
        write_sock(r, te, strlen(te));
    } else {
        char cl[40];
        snprintf(cl, sizeof(cl), "Content-Length: %u\r\n\r\n", (unsigned)r->resp_len);
        write_sock(r, cl, strlen(cl));
    }
    r->chunked = -1;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len) {
    if (!r) return ESP_FAIL;
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    if (buf && len > 0) {
        resp_grow(r, (size_t)len);
        if (r->resp) {
            memcpy(r->resp + r->resp_len, buf, (size_t)len);
            r->resp_len += (size_t)len;
        }
    }
    r->chunked = 0;
    flush_headers(r);
    if (r->resp && r->resp_len) write_sock(r, r->resp, r->resp_len);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str) {
    return httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len) {
    if (!r) return ESP_FAIL;
    if (r->chunked >= 0) {
        r->chunked = 1;
        flush_headers(r);
    }
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    if (len == 0) {
        write_sock(r, "0\r\n\r\n", 5);
        return ESP_OK;
    }
    char sz[24];
    int n = snprintf(sz, sizeof(sz), "%x\r\n", (unsigned)len);
    write_sock(r, sz, (size_t)n);
    write_sock(r, buf, (size_t)len);
    write_sock(r, "\r\n", 2);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str) {
    return httpd_resp_send_chunk(r, str, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg) {
    if (r) r->status = code;
    return httpd_resp_send(r, msg ? msg : "error", HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size) {
    if (!r || !field || !val) return ESP_FAIL;
    val[0] = 0;
    const char *hdrs = (const char *)r->user_ctx;
    if (!hdrs) return ESP_FAIL;
    size_t fl = strlen(field);
    const char *p = hdrs;
    while (*p) {
        int match = 1;
        for (size_t i = 0; i < fl; ++i) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)field[i])) {
                match = 0;
                break;
            }
        }
        if (match && p[fl] == ':') {
            p += fl + 1;
            while (*p == ' ') ++p;
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i + 1 < val_size) val[i++] = *p++;
            val[i] = 0;
            return ESP_OK;
        }
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    return ESP_FAIL;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r) { return r ? strlen(r->query) : 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len) {
    if (!r || !buf) return ESP_FAIL;
    snprintf(buf, buf_len, "%s", r->query);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) {
    if (!r || !buf) return -1;
    size_t n = r->body_len;
    if (n > buf_len) n = buf_len;
    if (n && r->body) memcpy(buf, r->body, n);
    if (r->body) r->body += n;
    r->body_len -= n;
    return (int)n;
}

int httpd_req_to_sockfd(httpd_req_t *r) {
    return r ? (int)(intptr_t)r->sess : -1;
}

esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out) {
    if (!r || !out) return ESP_FAIL;
    httpd_req_t *c = (httpd_req_t *)malloc(sizeof(*c));
    if (!c) return ESP_FAIL;
    *c = *r;
    c->keep_sock = 1;
    r->keep_sock = 1;
    *out = c;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *r) {
    if (!r) return ESP_FAIL;
    if (r->sess) {
        CLOSESOCK((SOCK)(intptr_t)r->sess);
        r->sess = nullptr;
    }
    free(r->resp);
    r->resp = nullptr;
    /* Shallow copy from handle_client: user_ctx is the request buffer. */
    free(r->user_ctx);
    r->user_ctx = nullptr;
    free(r);
    return ESP_OK;
}

int httpd_send(httpd_req_t *r, const char *buf, size_t buf_len) {
    if (!r || !buf || !r->sess) return -1;
    SOCK fd = (SOCK)(intptr_t)r->sess;
    size_t off = 0;
    while (off < buf_len) {
#ifdef _WIN32
        int w = send(fd, buf + off, (int)(buf_len - off), 0);
#else
        ssize_t w = send(fd, buf + off, buf_len - off, 0);
#endif
        if (w <= 0) return off ? (int)off : -1;
        off += (size_t)w;
    }
    return (int)off;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri) {
    httpd_srv_t *s = (httpd_srv_t *)h;
    if (!s || !uri || s->n_uris >= MAX_URIS) return ESP_FAIL;
    s->uris[s->n_uris++] = *uri;
    return ESP_OK;
}

static httpd_uri_t *find_uri(httpd_srv_t *s, const char *path, int method) {
    for (int i = 0; i < s->n_uris; ++i) {
        if (s->uris[i].method == method && strcmp(s->uris[i].uri, path) == 0) {
            return &s->uris[i];
        }
    }
    return nullptr;
}

static void handle_client(httpd_srv_t *s, SOCK fd) {
    char *reqbuf = (char *)malloc(64 * 1024);
    if (!reqbuf) {
        CLOSESOCK(fd);
        return;
    }
    size_t got = 0;
    while (got < 64 * 1024 - 1) {
#ifdef _WIN32
        int n = recv(fd, reqbuf + got, (int)(64 * 1024 - 1 - got), 0);
#else
        ssize_t n = recv(fd, reqbuf + got, 64 * 1024 - 1 - got, 0);
#endif
        if (n <= 0) break;
        got += (size_t)n;
        reqbuf[got] = 0;
        if (strstr(reqbuf, "\r\n\r\n")) break;
    }
    reqbuf[got] = 0;

    httpd_req_t req = {};
    req.sess = (void *)(intptr_t)fd;
    req.status = 200;
    snprintf(req.content_type, sizeof(req.content_type), "text/html; charset=utf-8");

    char method[8] = {0}, path[256] = {0};
    sscanf(reqbuf, "%7s %255s", method, path);
    char *q = strchr(path, '?');
    if (q) {
        *q = 0;
        snprintf(req.query, sizeof(req.query), "%s", q + 1);
    }
    snprintf(req.uri, sizeof(req.uri), "%s", path);
    snprintf(req.method, sizeof(req.method), "%s", method);

    char *hdr_end = strstr(reqbuf, "\r\n\r\n");
    req.user_ctx = reqbuf;
    if (hdr_end) {
        req.body = hdr_end + 4;
        req.body_len = got - (size_t)(req.body - reqbuf);
        const char *cl = strstr(reqbuf, "Content-Length:");
        if (!cl) cl = strstr(reqbuf, "content-length:");
        if (cl && cl < hdr_end) {
            req.content_len = atoi(cl + 15);
            if (req.content_len < 0) req.content_len = 0;
            size_t need = (size_t)req.content_len;
            while (req.body_len < need && got < 64 * 1024 - 1) {
#ifdef _WIN32
                int n = recv(fd, reqbuf + got, (int)(64 * 1024 - 1 - got), 0);
#else
                ssize_t n = recv(fd, reqbuf + got, 64 * 1024 - 1 - got, 0);
#endif
                if (n <= 0) break;
                got += (size_t)n;
                reqbuf[got] = 0;
                req.body = hdr_end + 4;
                req.body_len = got - (size_t)(req.body - reqbuf);
            }
        } else {
            req.content_len = (int)req.body_len;
        }
    }

    int meth = (strcmp(method, "POST") == 0) ? HTTP_POST : HTTP_GET;
    httpd_uri_t *u = find_uri(s, path, meth);
    if (!u) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot found";
        write_sock(&req, nf, strlen(nf));
        free(req.resp);
        free(reqbuf);
        CLOSESOCK(fd);
        return;
    }

    (void)u->handler(&req);
    if (req.keep_sock) {
        /* Async /live/stream owns the socket. Emitting an empty 200 here
         * made the GUI <img> load HTML (0 bytes) and show a broken camera. */
        return;
    }
    if (req.chunked >= 0 && req.resp_len == 0 && req.status) {
        httpd_resp_send(&req, "", 0);
    }
    free(req.resp);
    free(reqbuf);
    CLOSESOCK(fd);
}

typedef struct {
    httpd_srv_t *s;
    SOCK fd;
} httpd_cli_arg_t;

static void httpd_set_sock_timeouts(SOCK fd) {
#ifdef _WIN32
    DWORD ms = 15000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    ms = 10000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static void httpd_client_task(void *arg) {
    httpd_cli_arg_t *a = (httpd_cli_arg_t *)arg;
    httpd_srv_t *s = a->s;
    SOCK fd = a->fd;
    free(a);
    httpd_set_sock_timeouts(fd);
    handle_client(s, fd);
    vTaskDelete(nullptr);
}

static void httpd_task(void *arg) {
    httpd_srv_t *s = (httpd_srv_t *)arg;
    while (s->run) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        SOCK c = accept(s->listen_fd, (struct sockaddr *)&a, &al);
        if (c != INVALID_SOCKET) {
#ifndef _WIN32
            (void)fcntl(c, F_SETFD, FD_CLOEXEC);
#endif
        }
        if (c == INVALID_SOCKET) {
#ifdef _WIN32
            Sleep(20);
#else
            usleep(20000);
#endif
            continue;
        }
        /* Never handle a client on the accept thread. A hung Live/MPI
         * worker used to freeze GET / and look like the GUI was dead. */
        httpd_cli_arg_t *ca = (httpd_cli_arg_t *)malloc(sizeof(*ca));
        if (!ca) {
            CLOSESOCK(c);
            continue;
        }
        ca->s = s;
        ca->fd = c;
        if (xTaskCreate(httpd_client_task, "httpd_cli", 16384, ca, 5, nullptr) != pdPASS) {
            free(ca);
            CLOSESOCK(c);
        }
    }
    vTaskDelete(nullptr);
}

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    httpd_srv_t *s = (httpd_srv_t *)calloc(1, sizeof(*s));
    s->port = cfg && cfg->server_port ? cfg->server_port : CORE_WEB_PORT;
    s->listen_fd = (SOCK)socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifndef _WIN32
    /* system(mrbin-isp)/3A used to inherit this fd and steal :1510 —
     * GET / then queued forever and the GUI looked dead. */
    (void)fcntl(s->listen_fd, F_SETFD, FD_CLOEXEC);
#endif
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)s->port);
    if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(s->listen_fd, 8) != 0) {
        free(s);
        return ESP_FAIL;
    }
    s->run = 1;
    s_srv = s;
    xTaskCreate(httpd_task, "httpd", 8192, s, 5, &s->th);
    if (out) *out = s;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t h) {
    httpd_srv_t *s = (httpd_srv_t *)h;
    if (!s) return ESP_OK;
    s->run = 0;
    CLOSESOCK(s->listen_fd);
    s_srv = nullptr;
    return ESP_OK;
}
