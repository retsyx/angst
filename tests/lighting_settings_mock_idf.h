#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef pthread_mutex_t portMUX_TYPE;
typedef pthread_mutex_t *SemaphoreHandle_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define SOC_GPIO_PIN_COUNT 49
#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK (UINT64_C(0x1ffffffffffff) & ~UINT64_C(0x3c00000))
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)

SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
void xSemaphoreGive(SemaphoreHandle_t);
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_u64(nvs_handle_t, const char *, uint64_t *);
esp_err_t nvs_set_u64(nvs_handle_t, const char *, uint64_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
