#include "display_settings.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "display_settings";
static const char *NAMESPACE = "display_cfg";
static const char *ANIMATION_KEY = "animation";
static const char *STICK_FIGURE_KEY = "stick_figure";

static SemaphoreHandle_t writer_lock;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static display_settings_state_t current = {
    .animation_enabled = true,
    .stick_figure_enabled = true,
};

static void publish_settings(display_settings_state_t settings)
{
    portENTER_CRITICAL(&state_lock);
    current = settings;
    portEXIT_CRITICAL(&state_lock);
}

static esp_err_t load_enabled(nvs_handle_t handle, const char *key, bool *enabled)
{
    uint8_t stored = 1;
    esp_err_t err = nvs_get_u8(handle, key, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        *enabled = true;
        if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
            ESP_LOGW(TAG, "Invalid %s setting type; using enabled", key);
        }
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    if (stored > 1) {
        ESP_LOGW(TAG, "Invalid %s setting %u; using enabled", key, (unsigned)stored);
        stored = 1;
    }
    *enabled = stored != 0;
    return ESP_OK;
}

esp_err_t display_settings_init(void)
{
    /* Initialization is performed by app_main before HTTP/GUI consumers. */
    if (writer_lock) return ESP_OK;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        return err;
    }

    display_settings_state_t loaded;
    err = load_enabled(handle, ANIMATION_KEY, &loaded.animation_enabled);
    if (err == ESP_OK) {
        err = load_enabled(handle, STICK_FIGURE_KEY, &loaded.stick_figure_enabled);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        return err;
    }

    publish_settings(loaded);
    writer_lock = lock;
    return ESP_OK;
}

display_settings_state_t display_settings_get(void)
{
    portENTER_CRITICAL(&state_lock);
    display_settings_state_t settings = current;
    portEXIT_CRITICAL(&state_lock);
    return settings;
}

bool display_settings_animation_enabled(void)
{
    return display_settings_get().animation_enabled;
}

bool display_settings_stick_figure_enabled(void)
{
    return display_settings_get().stick_figure_enabled;
}

static esp_err_t set_enabled(bool enabled, bool stick_figure)
{
    if (!writer_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(writer_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    display_settings_state_t next = display_settings_get();
    bool *value = stick_figure ? &next.stick_figure_enabled : &next.animation_enabled;
    esp_err_t err = ESP_OK;
    if (*value != enabled) {
        *value = enabled;
        nvs_handle_t handle;
        err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            /* Rewrite the complete cached state: a failed earlier write or
             * commit must not leak into a later change to the other setting. */
            err = nvs_set_u8(handle, ANIMATION_KEY, next.animation_enabled ? 1 : 0);
            if (err == ESP_OK) {
                err = nvs_set_u8(handle, STICK_FIGURE_KEY, next.stick_figure_enabled ? 1 : 0);
            }
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
            if (err == ESP_OK) publish_settings(next);
        }
    }

    xSemaphoreGive(writer_lock);
    return err;
}

esp_err_t display_settings_set_animation_enabled(bool enabled)
{
    return set_enabled(enabled, false);
}

esp_err_t display_settings_set_stick_figure_enabled(bool enabled)
{
    return set_enabled(enabled, true);
}
