#include "price_test.h"

#include <stdint.h>
#include <string.h>

#include "price_test_math.h"
#include "service.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#define TEST_PRICE_CAPACITY 64
#define TEST_PERIOD_US (INT64_C(60) * 1000000)

static SemaphoreHandle_t s_lock;
static char s_current[SERVICE_MESSAGE_CAPACITY];
static char s_latest_live[SERVICE_MESSAGE_CAPACITY];
static char s_pending_test[TEST_PRICE_CAPACITY];
static bool s_have_live;
static bool s_active;
static bool s_pending;
static bool s_used_fallback;
static int64_t s_deadline;

esp_err_t price_test_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t price_test_request(bool increase, char *result, size_t capacity)
{
    if (!result || !capacity) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    char generated[TEST_PRICE_CAPACITY];
    bool fallback = false;
    if (!price_test_generate(s_current, increase, esp_random(), generated,
                             sizeof(generated), &fallback) ||
        strlen(generated) >= capacity) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(result, generated);
    strcpy(s_current, generated);
    strcpy(s_pending_test, generated);
    s_used_fallback = fallback;
    s_pending = true;
    s_active = true;
    s_deadline = esp_timer_get_time() + TEST_PERIOD_US;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool price_test_accept_live(const char *text)
{
    if (!s_lock || !text || strlen(text) >= sizeof(s_latest_live)) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    strcpy(s_latest_live, text);
    s_have_live = true;
    const bool accept = !s_active || esp_timer_get_time() >= s_deadline;
    if (accept) {
        s_active = false;
        s_pending = false;
        strcpy(s_current, text);
    }
    xSemaphoreGive(s_lock);
    return accept;
}

bool price_test_take(char *result, size_t capacity, bool *used_fallback,
                     bool *is_test)
{
    if (!s_lock || !result || !capacity || !used_fallback || !is_test) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    const bool expired = s_active && esp_timer_get_time() >= s_deadline;
    const char *next = NULL;
    bool test = false;
    if (expired && s_have_live) next = s_latest_live;
    else if (s_pending && !expired) {
        next = s_pending_test;
        test = true;
    }
    if (!next || strlen(next) >= capacity) {
        /* With no live response yet, leave the test price until one arrives. */
        if (expired && !s_have_live) {
            s_active = false;
            s_pending = false;
        }
        xSemaphoreGive(s_lock);
        return false;
    }
    strcpy(result, next);
    *used_fallback = test && s_used_fallback;
    *is_test = test;
    s_pending = false;
    if (!test) {
        s_active = false;
        strcpy(s_current, next);
    }
    xSemaphoreGive(s_lock);
    return true;
}
