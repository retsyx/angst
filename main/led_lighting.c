#include "led_lighting.h"
#include "lighting_settings.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"

#define LIGHTS_FULL_DUTY (1U << 12)

static bool initialized;
static uint64_t active_gpios;

static bool valid_gpios(uint64_t gpios)
{
    return gpios && !(gpios & ~lighting_gpio_available_mask());
}

static esp_err_t drive_low(uint64_t gpios)
{
    if (!gpios) return ESP_OK;
    for (unsigned pin = 0; pin < 49; ++pin) {
        if (!(gpios & (UINT64_C(1) << pin))) continue;
        esp_err_t error = gpio_set_level((gpio_num_t)pin, 0);
        if (error != ESP_OK) return error;
    }
    const gpio_config_t config = {
        .pin_bit_mask = gpios,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    // Output configuration detaches PWM from the GPIO matrix. Setting the low
    // output latch first ensures removed LEDs turn off instead of staying lit.
    return gpio_config(&config);
}

static esp_err_t connect_pwm(uint64_t gpios)
{
    for (unsigned pin = 0; pin < 49; ++pin) {
        if (!(gpios & (UINT64_C(1) << pin))) continue;
        esp_err_t error = ledc_set_pin((int)pin, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

esp_err_t led_lighting_init(uint64_t gpios)
{
    if (!valid_gpios(gpios)) return ESP_ERR_INVALID_ARG;
    if (initialized) return ESP_ERR_INVALID_STATE;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    error = drive_low(gpios);
    if (error != ESP_OK) return error;

    const unsigned first_pin = (unsigned)__builtin_ctzll(gpios);
    const ledc_channel_config_t channel = {
        .gpio_num = (int)first_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = LIGHTS_FULL_DUTY,
    };
    error = ledc_channel_config(&channel);
    if (error == ESP_OK) error = connect_pwm(gpios & ~(UINT64_C(1) << first_pin));
    if (error != ESP_OK) {
        (void)drive_low(gpios);
        return error;
    }
    active_gpios = gpios;
    initialized = true;
    return ESP_OK;
}

esp_err_t led_lighting_set_gpios(uint64_t gpios)
{
    if (!valid_gpios(gpios)) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (gpios == active_gpios) return ESP_OK;

    const uint64_t added = gpios & ~active_gpios;
    const uint64_t removed = active_gpios & ~gpios;
    esp_err_t error = drive_low(added);
    if (error == ESP_OK) error = connect_pwm(added);
    if (error != ESP_OK) {
        (void)drive_low(added);
        return error;
    }
    error = drive_low(removed);
    if (error != ESP_OK) {
        (void)drive_low(added);
        (void)connect_pwm(removed);
        return error;
    }
    active_gpios = gpios;
    return ESP_OK;
}

uint64_t led_lighting_get_gpios(void)
{
    return active_gpios;
}

esp_err_t led_lighting_set_brightness(uint8_t brightness)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    // Full 0..4096 duty range: off is steady low, 100% is steady high.
    const uint32_t duty = (brightness * LIGHTS_FULL_DUTY + 127U) / 255U;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error != ESP_OK) return error;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
