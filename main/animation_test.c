#include "animation_test.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static animation_id_t s_pending;
static animation_id_t s_current;

esp_err_t animation_test_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t animation_test_request(animation_id_t id)
{
    if (!animation_catalog_lookup(id)) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    /* Latest request wins without blocking either task on display rendering. */
    s_pending = id;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool animation_test_take(animation_id_t *id)
{
    if (!id || !s_lock) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    const bool pending = s_pending != ANIMATION_NONE;
    if (pending) {
        *id = s_pending;
        s_pending = ANIMATION_NONE;
    }
    xSemaphoreGive(s_lock);
    return pending;
}

void animation_test_publish(animation_id_t current)
{
    if (current != ANIMATION_NONE && !animation_catalog_lookup(current)) return;
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_current = current;
    xSemaphoreGive(s_lock);
}

animation_status_t animation_test_status(void)
{
    animation_status_t status = {ANIMATION_NONE, false};
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return status;
    status.animation_id = s_current;
    status.pending = s_pending != ANIMATION_NONE;
    xSemaphoreGive(s_lock);
    return status;
}
