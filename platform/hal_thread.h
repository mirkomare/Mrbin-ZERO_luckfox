#pragma once

#include "hal_log.h"
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <sched.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define portMAX_DELAY  0xFFFFFFFFu
#define BIT0     (1u << 0)

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
typedef void *QueueHandle_t;
typedef uint32_t EventBits_t;

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#ifdef _WIN32
#define taskYIELD() Sleep(0)
#else
#define taskYIELD() sched_yield()
#endif

void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *self);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                       int prio, TaskHandle_t *out);
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                                 uint32_t stack, void *arg, int prio,
                                                 TaskHandle_t *out, int) {
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t max_count, uint32_t init);
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);

EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t g);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t g);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait);

QueueHandle_t xQueueCreate(uint32_t len, uint32_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
void vQueueDelete(QueueHandle_t q);

#ifdef __cplusplus
}
#endif
