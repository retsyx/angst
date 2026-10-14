#include "lighting_settings.h"

#include <ctype.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "soc/soc_caps.h"

static const char *TAG = "lighting_settings";
static const char *NAMESPACE = "lighting_cfg";
static const char *GPIO_KEY = "gpios";

static SemaphoreHandle_t writer_lock;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t current = LIGHTING_GPIO_DEFAULT_MASK;

uint64_t lighting_gpio_available_mask(void)
{
    /* Header pins shared by the basic and touch board variants: GPIO1,
     * GPIO10..16 and GPIO39..46. The remaining pins serve the display,
     * touch interface, buttons, battery sensing, USB, flash or octal PSRAM. */
    return SOC_GPIO_VALID_OUTPUT_GPIO_MASK & UINT64_C(0x7f800001fc02);
}

static bool valid_mask(uint64_t gpios)
{
    return gpios && !(gpios & ~lighting_gpio_available_mask());
}

static void publish_settings(uint64_t gpios)
{
    portENTER_CRITICAL(&state_lock);
    current = gpios;
    portEXIT_CRITICAL(&state_lock);
}

uint64_t lighting_settings_get(void)
{
    portENTER_CRITICAL(&state_lock);
    const uint64_t gpios = current;
    portEXIT_CRITICAL(&state_lock);
    return gpios;
}

esp_err_t lighting_settings_init(void)
{
    /* app_main initializes settings before HTTP and GPIO consumers. */
    if (writer_lock) return ESP_OK;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        return err;
    }

    uint64_t loaded = LIGHTING_GPIO_DEFAULT_MASK;
    err = nvs_get_u64(handle, GPIO_KEY, &loaded);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
            ESP_LOGW(TAG, "Invalid GPIO setting type; using defaults");
        }
        loaded = LIGHTING_GPIO_DEFAULT_MASK;
        err = ESP_OK;
    } else if (err == ESP_OK && !valid_mask(loaded)) {
        ESP_LOGW(TAG, "Invalid GPIO setting; using defaults");
        loaded = LIGHTING_GPIO_DEFAULT_MASK;
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        return err;
    }

    publish_settings(loaded);
    writer_lock = lock;
    return ESP_OK;
}

esp_err_t lighting_settings_set(uint64_t gpios)
{
    if (!valid_mask(gpios)) return ESP_ERR_INVALID_ARG;
    if (!writer_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(writer_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    if (gpios != lighting_settings_get()) {
        nvs_handle_t handle;
        err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u64(handle, GPIO_KEY, gpios);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
            if (err == ESP_OK) publish_settings(gpios);
        }
    }
    xSemaphoreGive(writer_lock);
    return err;
}

esp_err_t lighting_gpio_parse(const char *text, uint64_t *gpios)
{
    if (!text || !gpios) return ESP_ERR_INVALID_ARG;
    uint64_t parsed = 0;
    for (;;) {
        while (isspace((unsigned char)*text)) ++text;
        if (*text < '0' || *text > '9') return ESP_ERR_INVALID_ARG;
        unsigned pin = 0;
        do {
            /* Reject out-of-range numbers before multiplication can overflow. */
            pin = pin * 10U + (unsigned)(*text++ - '0');
            if (pin >= SOC_GPIO_PIN_COUNT) return ESP_ERR_INVALID_ARG;
        } while (*text >= '0' && *text <= '9');
        const uint64_t bit = UINT64_C(1) << pin;
        if (!(bit & lighting_gpio_available_mask()) || (parsed & bit)) {
            return ESP_ERR_INVALID_ARG;
        }
        parsed |= bit;
        while (isspace((unsigned char)*text)) ++text;
        if (!*text) break;
        if (*text++ != ',') return ESP_ERR_INVALID_ARG;
    }
    *gpios = parsed;
    return ESP_OK;
}

esp_err_t lighting_gpio_format(uint64_t gpios, char *text, size_t capacity)
{
    if (!text || !valid_mask(gpios)) return ESP_ERR_INVALID_ARG;
    size_t required = 0;
    unsigned count = 0;
    for (unsigned pin = 0; pin < SOC_GPIO_PIN_COUNT; ++pin) {
        if (gpios & (UINT64_C(1) << pin)) {
            required += (count++ ? 1U : 0U) + (pin >= 10 ? 2U : 1U);
        }
    }
    if (capacity <= required) return ESP_ERR_INVALID_SIZE;
    char *out = text;
    count = 0;
    for (unsigned pin = 0; pin < SOC_GPIO_PIN_COUNT; ++pin) {
        if (!(gpios & (UINT64_C(1) << pin))) continue;
        if (count++) *out++ = ',';
        if (pin >= 10) *out++ = (char)('0' + pin / 10);
        *out++ = (char)('0' + pin % 10);
    }
    *out = '\0';
    return ESP_OK;
}
