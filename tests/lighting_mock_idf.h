#pragma once

#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

typedef int gpio_num_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);

#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_12_BIT 12
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_AUTO_CLK 0
typedef struct {
    int speed_mode, duty_resolution, timer_num;
    uint32_t freq_hz;
    int clk_cfg;
} ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, timer_sel;
    uint32_t duty;
} ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
esp_err_t ledc_set_pin(int pin, int speed_mode, int channel);
esp_err_t ledc_set_duty(int speed_mode, int channel, uint32_t duty);
esp_err_t ledc_update_duty(int speed_mode, int channel);
