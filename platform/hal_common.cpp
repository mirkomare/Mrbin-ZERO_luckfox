#include "hal_log.h"
#include "hal_timer.h"
#include "hal_random.h"
#include "hal_system.h"
#include "hal_heap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#endif

void hal_log(const char *level, const char *tag, const char *fmt, ...) {
    fprintf(stderr, "%s (%s) ", level ? level : "I", tag ? tag : "?");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *esp_err_to_name(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        default: return "ESP_ERR";
    }
}

int64_t esp_timer_get_time(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart * 1000000ULL) / (unsigned long long)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
#endif
}

typedef struct esp_timer {
    esp_timer_cb_t cb;
    void *arg;
    uint64_t period_us;
    volatile int run;
#ifdef _WIN32
    HANDLE th;
#else
    pthread_t th;
#endif
} esp_timer_t;

#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef _WIN32
static DWORD WINAPI timer_th(LPVOID p) {
#else
static void *timer_th(void *p) {
#endif
    esp_timer_t *t = (esp_timer_t *)p;
    while (t->run) {
#ifdef _WIN32
        Sleep((DWORD)((t->period_us + 999) / 1000));
#else
        usleep((useconds_t)t->period_us);
#endif
        if (t->run && t->cb) {
            t->cb(t->arg);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return nullptr;
#endif
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    if (!args || !out) return ESP_FAIL;
    esp_timer_t *t = (esp_timer_t *)calloc(1, sizeof(*t));
    if (!t) return ESP_FAIL;
    t->cb = args->callback;
    t->arg = args->arg;
    *out = t;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t period_us) {
    if (!h) return ESP_FAIL;
    h->period_us = period_us ? period_us : 1000;
    h->run = 1;
#ifdef _WIN32
    h->th = CreateThread(nullptr, 0, timer_th, h, 0, nullptr);
    return h->th ? ESP_OK : ESP_FAIL;
#else
    return pthread_create(&h->th, nullptr, timer_th, h) == 0 ? ESP_OK : ESP_FAIL;
#endif
}

esp_err_t esp_timer_stop(esp_timer_handle_t h) {
    if (!h) return ESP_OK;
    h->run = 0;
    return ESP_OK;
}

uint32_t esp_random(void) {
    uint32_t v = 0;
    esp_fill_random(&v, sizeof(v));
    return v;
}

void esp_fill_random(void *buf, size_t len) {
    if (!buf || !len) return;
#ifdef _WIN32
    HCRYPTPROV p = 0;
    if (CryptAcquireContext(&p, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(p, (DWORD)len, (BYTE *)buf);
        CryptReleaseContext(p, 0);
        return;
    }
    unsigned char *b = (unsigned char *)buf;
    for (size_t i = 0; i < len; ++i) b[i] = (unsigned char)(rand() & 0xFF);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, len);
        close(fd);
        if (n == (ssize_t)len) return;
    }
    unsigned char *b = (unsigned char *)buf;
    for (size_t i = 0; i < len; ++i) b[i] = (unsigned char)(rand() & 0xFF);
#endif
}

esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

void esp_restart(void) {
#ifndef _WIN32
    sync();
    execl("/sbin/reboot", "reboot", (char *)nullptr);
#endif
    exit(0);
}

#include <errno.h>
#ifndef _WIN32
#include <pthread.h>
#include <fcntl.h>

typedef struct {
    const char *path;
    int flags;
    volatile int fd;
    volatile int done;
    volatile int abandoned;
} hal_open_job_t;

static void *hal_open_timeout_th(void *arg) {
    hal_open_job_t *j = (hal_open_job_t *)arg;
    int fd = open(j->path, j->flags);
    if (j->abandoned) {
        if (fd >= 0) {
            close(fd);
        }
        free(j);
        return nullptr;
    }
    j->fd = fd;
    j->done = 1;
    return nullptr;
}
#endif

int hal_open_timeout(const char *path, int flags, int timeout_ms) {
    if (!path || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
#ifdef _WIN32
    (void)flags;
    (void)timeout_ms;
    errno = ENOSYS;
    return -1;
#else
    hal_open_job_t *j = (hal_open_job_t *)calloc(1, sizeof(*j));
    if (!j) {
        return -1;
    }
    j->path = path;
    j->flags = flags;
    j->fd = -1;
    pthread_t th;
    if (pthread_create(&th, nullptr, hal_open_timeout_th, j) != 0) {
        int fd = open(path, flags | O_NONBLOCK);
        free(j);
        return fd;
    }
    pthread_detach(th);
    const int step_ms = 10;
    int waited = 0;
    while (!j->done && waited < timeout_ms) {
        usleep((useconds_t)(step_ms * 1000));
        waited += step_ms;
    }
    if (j->done) {
        int fd = j->fd;
        free(j);
        return fd;
    }
    j->abandoned = 1;
    ESP_LOGW("hal", "open(%s) timeout %d ms — non blocco il caller", path, timeout_ms);
    errno = ETIMEDOUT;
    return -1;
#endif
}

void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}

void *heap_caps_realloc(void *p, size_t size, unsigned caps) {
    (void)caps;
    return realloc(p, size);
}

void heap_caps_free(void *p) { free(p); }

size_t heap_caps_get_free_size(unsigned caps) {
    (void)caps;
    /* 64 MB DDR2: no 8 MB PSRAM rings. Advertise a small staging pool. */
    return 2U * 1024U * 1024U;
}

size_t heap_caps_get_largest_free_block(unsigned caps) {
    (void)caps;
    return 512U * 1024U;
}
