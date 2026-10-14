#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
