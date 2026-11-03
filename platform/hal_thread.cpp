#include "hal_thread.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#endif

void vTaskDelay(TickType_t ticks) {
#ifdef _WIN32
    Sleep((DWORD)ticks);
#else
    usleep((useconds_t)(ticks * 1000));
#endif
}

void vTaskDelete(void *) {
#ifdef _WIN32
    ExitThread(0);
#else
    pthread_exit(nullptr);
#endif
}

#ifdef _WIN32
struct task_wrap {
    void (*fn)(void *);
    void *arg;
};
static DWORD WINAPI task_th(LPVOID p) {
    task_wrap *w = (task_wrap *)p;
    w->fn(w->arg);
    free(w);
    return 0;
}
#else
struct task_wrap {
    void (*fn)(void *);
    void *arg;
};
static void *task_th(void *p) {
    task_wrap *w = (task_wrap *)p;
    w->fn(w->arg);
    free(w);
    return nullptr;
}
#endif

BaseType_t xTaskCreate(void (*fn)(void *), const char *, uint32_t, void *arg, int, TaskHandle_t *out) {
    task_wrap *w = (task_wrap *)malloc(sizeof(*w));
    if (!w) return pdFAIL;
    w->fn = fn;
    w->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(nullptr, 0, task_th, w, 0, nullptr);
    if (!h) {
        free(w);
        return pdFAIL;
    }
    if (out) *out = h;
#else
    pthread_t th;
    if (pthread_create(&th, nullptr, task_th, w) != 0) {
        free(w);
        return pdFAIL;
    }
    pthread_detach(th);
    if (out) *out = (TaskHandle_t)(uintptr_t)th;
#endif
    return pdPASS;
}

#ifdef _WIN32
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return CreateMutex(nullptr, FALSE, nullptr); }
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t, uint32_t init) {
    HANDLE h = CreateMutex(nullptr, FALSE, nullptr);
    if (h && init == 0) WaitForSingleObject(h, 0);
    return h;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { if (s) CloseHandle(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    return WaitForSingleObject(s, wait == portMAX_DELAY ? INFINITE : wait) == WAIT_OBJECT_0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return ReleaseMutex(s) ? pdTRUE : pdFALSE; }

typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    EventBits_t bits;
} ev_t;

EventGroupHandle_t xEventGroupCreate(void) {
    ev_t *e = (ev_t *)calloc(1, sizeof(*e));
    InitializeCriticalSection(&e->cs);
    InitializeConditionVariable(&e->cv);
    return e;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits) {
    ev_t *e = (ev_t *)g;
    EnterCriticalSection(&e->cs);
    e->bits |= bits;
    WakeAllConditionVariable(&e->cv);
    EventBits_t b = e->bits;
    LeaveCriticalSection(&e->cs);
    return b;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits) {
    ev_t *e = (ev_t *)g;
    EnterCriticalSection(&e->cs);
    e->bits &= ~bits;
    EventBits_t b = e->bits;
    LeaveCriticalSection(&e->cs);
    return b;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear,
                                BaseType_t, TickType_t wait) {
    ev_t *e = (ev_t *)g;
    EnterCriticalSection(&e->cs);
    DWORD t0 = GetTickCount();
    while ((e->bits & bits) == 0) {
        DWORD left = (wait == portMAX_DELAY) ? INFINITE : (GetTickCount() - t0 >= wait ? 0 : wait - (GetTickCount() - t0));
        if (left == 0 && wait != portMAX_DELAY) break;
        SleepConditionVariableCS(&e->cv, &e->cs, left);
    }
    EventBits_t b = e->bits;
    if (clear) e->bits &= ~bits;
    LeaveCriticalSection(&e->cs);
    return b;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t g) {
    ev_t *e = (ev_t *)g;
    EnterCriticalSection(&e->cs);
    EventBits_t b = e->bits;
    LeaveCriticalSection(&e->cs);
    return b;
}
void vEventGroupDelete(EventGroupHandle_t g) {
    ev_t *e = (ev_t *)g;
    if (!e) return;
    DeleteCriticalSection(&e->cs);
    free(e);
}

typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    uint32_t len, item, n, r, w;
    char *buf;
} q_t;
QueueHandle_t xQueueCreate(uint32_t len, uint32_t item) {
    q_t *q = (q_t *)calloc(1, sizeof(*q));
    q->len = len;
    q->item = item;
    q->buf = (char *)calloc(len, item);
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->cv);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t) {
    q_t *q = (q_t *)h;
    EnterCriticalSection(&q->cs);
    if (q->n >= q->len) {
        LeaveCriticalSection(&q->cs);
        return pdFALSE;
    }
    memcpy(q->buf + q->w * q->item, item, q->item);
    q->w = (q->w + 1) % q->len;
    q->n++;
    WakeConditionVariable(&q->cv);
    LeaveCriticalSection(&q->cs);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t wait) {
    q_t *q = (q_t *)h;
    EnterCriticalSection(&q->cs);
    if (q->n == 0) {
        SleepConditionVariableCS(&q->cv, &q->cs, wait == portMAX_DELAY ? INFINITE : wait);
    }
    if (q->n == 0) {
        LeaveCriticalSection(&q->cs);
        return pdFALSE;
    }
    memcpy(item, q->buf + q->r * q->item, q->item);
    q->r = (q->r + 1) % q->len;
    q->n--;
    LeaveCriticalSection(&q->cs);
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t h) {
    q_t *q = (q_t *)h;
    if (!q) return;
    DeleteCriticalSection(&q->cs);
    free(q->buf);
    free(q);
}

#else

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    pthread_mutex_init(m, nullptr);
    return m;
}
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t, uint32_t init) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    pthread_mutex_init(m, nullptr);
    if (init == 0) pthread_mutex_lock(m);
    return m;
}
void vSemaphoreDelete(SemaphoreHandle_t s) {
    if (!s) return;
    pthread_mutex_destroy((pthread_mutex_t *)s);
    free(s);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (wait == portMAX_DELAY) {
        return pthread_mutex_lock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
    }
    /* busy-wait short lock */
    for (uint32_t i = 0; i < wait; ++i) {
        if (pthread_mutex_trylock((pthread_mutex_t *)s) == 0) return pdTRUE;
        usleep(1000);
    }
    return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    return pthread_mutex_unlock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    EventBits_t bits;
} ev_t;

EventGroupHandle_t xEventGroupCreate(void) {
    ev_t *e = (ev_t *)calloc(1, sizeof(*e));
    pthread_mutex_init(&e->m, nullptr);
    pthread_cond_init(&e->c, nullptr);
    return e;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits) {
    ev_t *e = (ev_t *)g;
    pthread_mutex_lock(&e->m);
    e->bits |= bits;
    pthread_cond_broadcast(&e->c);
    EventBits_t b = e->bits;
    pthread_mutex_unlock(&e->m);
    return b;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits) {
    ev_t *e = (ev_t *)g;
    pthread_mutex_lock(&e->m);
    e->bits &= ~bits;
    EventBits_t b = e->bits;
    pthread_mutex_unlock(&e->m);
    return b;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear,
                                BaseType_t, TickType_t wait) {
    ev_t *e = (ev_t *)g;
    pthread_mutex_lock(&e->m);
    if (wait == portMAX_DELAY) {
        while ((e->bits & bits) == 0) pthread_cond_wait(&e->c, &e->m);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait / 1000;
        ts.tv_nsec += (wait % 1000) * 1000000L;
        while ((e->bits & bits) == 0) {
            if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) break;
        }
    }
    EventBits_t b = e->bits;
    if (clear) e->bits &= ~bits;
    pthread_mutex_unlock(&e->m);
    return b;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t g) {
    ev_t *e = (ev_t *)g;
    pthread_mutex_lock(&e->m);
    EventBits_t b = e->bits;
    pthread_mutex_unlock(&e->m);
    return b;
}
void vEventGroupDelete(EventGroupHandle_t g) {
    ev_t *e = (ev_t *)g;
    if (!e) return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t len, item, n, r, w;
    char *buf;
} q_t;
QueueHandle_t xQueueCreate(uint32_t len, uint32_t item) {
    q_t *q = (q_t *)calloc(1, sizeof(*q));
    q->len = len;
    q->item = item;
    q->buf = (char *)calloc(len, item);
    pthread_mutex_init(&q->m, nullptr);
    pthread_cond_init(&q->c, nullptr);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t) {
    q_t *q = (q_t *)h;
    pthread_mutex_lock(&q->m);
    if (q->n >= q->len) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;
    }
    memcpy(q->buf + q->w * q->item, item, q->item);
    q->w = (q->w + 1) % q->len;
    q->n++;
    pthread_cond_signal(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t wait) {
    q_t *q = (q_t *)h;
    pthread_mutex_lock(&q->m);
    if (q->n == 0 && wait) {
        if (wait == portMAX_DELAY) pthread_cond_wait(&q->c, &q->m);
        else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait / 1000;
            pthread_cond_timedwait(&q->c, &q->m, &ts);
        }
    }
    if (q->n == 0) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;
    }
    memcpy(item, q->buf + q->r * q->item, q->item);
    q->r = (q->r + 1) % q->len;
    q->n--;
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t h) {
    q_t *q = (q_t *)h;
    if (!q) return;
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->c);
    free(q->buf);
    free(q);
}
#endif
