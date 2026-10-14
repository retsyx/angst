#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lighting_mock_idf.h"
#include "../main/led_lighting.c"

#define PIN(pin) (UINT64_C(1) << (pin))
static uint64_t pwm_pins;
static uint64_t low_pins;
static uint64_t output_pins;
static uint32_t duty, pending_duty;
static int timer_calls, channel_calls, gpio_calls, duty_calls;
static int fail_pin = -1;
static bool fail_config, fail_timer, fail_duty, fail_update;

uint64_t lighting_gpio_available_mask(void)
{
    return UINT64_C(0x7f800001fc02);
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin >= 0 && pin < 49);
    assert(level == 0);
    ++gpio_calls;
    low_pins |= PIN(pin);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    ++gpio_calls;
    assert(config->pin_bit_mask && !(config->pin_bit_mask & ~low_pins));
    assert(config->mode == GPIO_MODE_OUTPUT);
    assert(config->pull_up_en == GPIO_PULLUP_DISABLE);
    assert(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    if (fail_config) {
        fail_config = false;
        return ESP_FAIL;
    }
    output_pins |= config->pin_bit_mask;
    pwm_pins &= ~config->pin_bit_mask;
    return ESP_OK;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *config)
{
    ++timer_calls;
    assert(config->speed_mode == LEDC_LOW_SPEED_MODE);
    assert(config->duty_resolution == 12 && config->freq_hz == 5000);
    assert(config->timer_num == LEDC_TIMER_0 && config->clk_cfg == LEDC_AUTO_CLK);
    return fail_timer ? ESP_FAIL : ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    ++channel_calls;
    assert(config->speed_mode == LEDC_LOW_SPEED_MODE);
    assert(config->channel == LEDC_CHANNEL_0 && config->timer_sel == LEDC_TIMER_0);
    assert(config->duty == 4096);
    assert(output_pins & PIN(config->gpio_num));
    pwm_pins |= PIN(config->gpio_num);
    duty = config->duty;
    return ESP_OK;
}

esp_err_t ledc_set_pin(int pin, int speed_mode, int channel)
{
    assert(speed_mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0);
    assert(output_pins & PIN(pin));
    if (pin == fail_pin) {
        fail_pin = -1;
        return ESP_FAIL;
    }
    pwm_pins |= PIN(pin);
    return ESP_OK;
}

esp_err_t ledc_set_duty(int speed_mode, int channel, uint32_t value)
{
    ++duty_calls;
    assert(speed_mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0);
    assert(value <= 4096);
    if (fail_duty) return ESP_FAIL;
    pending_duty = value;
    return ESP_OK;
}

esp_err_t ledc_update_duty(int speed_mode, int channel)
{
    assert(speed_mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0);
    if (fail_update) return ESP_FAIL;
    duty = pending_duty;
    return ESP_OK;
}

static void reset(void)
{
    initialized = false;
    active_gpios = pwm_pins = low_pins = output_pins = 0;
    timer_calls = channel_calls = gpio_calls = duty_calls = 0;
    fail_pin = -1;
    fail_config = fail_timer = fail_duty = fail_update = false;
}

static void check_selection(uint64_t expected)
{
    assert(led_lighting_get_gpios() == expected && pwm_pins == expected);
    assert((output_pins & ~expected) == (low_pins & output_pins & ~expected));
    assert(timer_calls == 1 && channel_calls == 1);
}

int main(void)
{
    reset();
    assert(led_lighting_set_brightness(255) == ESP_ERR_INVALID_STATE);
    assert(led_lighting_set_gpios(PIN(45)) == ESP_ERR_INVALID_STATE);
    assert(led_lighting_init(0) == ESP_ERR_INVALID_ARG);
    assert(led_lighting_init(PIN(19)) == ESP_ERR_INVALID_ARG);
    assert(led_lighting_init(PIN(63)) == ESP_ERR_INVALID_ARG);
    assert(timer_calls == 0 && gpio_calls == 0);

    assert(led_lighting_init(LIGHTING_GPIO_DEFAULT_MASK) == ESP_OK);
    check_selection(LIGHTING_GPIO_DEFAULT_MASK);
    assert(duty == 4096);
    assert(led_lighting_init(PIN(45)) == ESP_ERR_INVALID_STATE);
    const int initial_gpio_calls = gpio_calls;
    for (unsigned brightness = 0; brightness <= 255; ++brightness) {
        assert(led_lighting_set_brightness((uint8_t)brightness) == ESP_OK);
        assert(duty == (brightness * 4096U + 127U) / 255U);
    }
    assert(gpio_calls == initial_gpio_calls);
    assert(led_lighting_set_brightness(128) == ESP_OK);
    const uint32_t half_duty = duty;

    assert(led_lighting_set_gpios(PIN(45)) == ESP_OK);
    check_selection(PIN(45));
    assert((low_pins & PIN(46)) && !(pwm_pins & PIN(46)));
    assert(led_lighting_set_gpios(PIN(1) | PIN(10) | PIN(16) | PIN(45)) == ESP_OK);
    check_selection(PIN(1) | PIN(10) | PIN(16) | PIN(45));
    assert(duty == half_duty);
    assert(led_lighting_set_gpios(lighting_gpio_available_mask()) == ESP_OK);
    check_selection(lighting_gpio_available_mask());
    assert(duty == half_duty);
    const int unchanged_calls = gpio_calls;
    assert(led_lighting_set_gpios(lighting_gpio_available_mask()) == ESP_OK);
    assert(led_lighting_set_gpios(0) == ESP_ERR_INVALID_ARG);
    assert(led_lighting_set_gpios(PIN(45) | PIN(5)) == ESP_ERR_INVALID_ARG);
    assert(gpio_calls == unchanged_calls);

    assert(led_lighting_set_gpios(LIGHTING_GPIO_DEFAULT_MASK) == ESP_OK);
    check_selection(LIGHTING_GPIO_DEFAULT_MASK);
    fail_pin = 11;
    assert(led_lighting_set_gpios(PIN(10) | PIN(11) | PIN(12)) == ESP_FAIL);
    check_selection(LIGHTING_GPIO_DEFAULT_MASK);
    assert(!(pwm_pins & (PIN(10) | PIN(11) | PIN(12))));
    fail_config = true;
    assert(led_lighting_set_gpios(PIN(10)) == ESP_FAIL);
    check_selection(LIGHTING_GPIO_DEFAULT_MASK);
    fail_config = true;
    assert(led_lighting_set_gpios(PIN(45)) == ESP_FAIL);
    check_selection(LIGHTING_GPIO_DEFAULT_MASK);

    fail_duty = true;
    assert(led_lighting_set_brightness(0) == ESP_FAIL && duty == half_duty);
    fail_duty = false;
    fail_update = true;
    assert(led_lighting_set_brightness(255) == ESP_FAIL && duty == half_duty);
    fail_update = false;
    assert(led_lighting_set_brightness(0) == ESP_OK && duty == 0);

    reset();
    fail_timer = true;
    assert(led_lighting_init(PIN(10)) == ESP_FAIL && !initialized);
    assert(gpio_calls == 0 && active_gpios == 0);
    reset();
    fail_pin = 16;
    assert(led_lighting_init(PIN(10) | PIN(16)) == ESP_FAIL && !initialized);
    assert(pwm_pins == 0 && active_gpios == 0);
    reset();
    assert(led_lighting_init(PIN(10)) == ESP_OK);
    check_selection(PIN(10));
    assert(!(output_pins & LIGHTING_GPIO_DEFAULT_MASK));
    puts("LED lighting: all 16 outputs share PWM, brightness, live removal, and failure rollback passed");
    return 0;
}
