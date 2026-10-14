#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These hardware operations belong to the display task. All selected outputs
 * share a single LEDC channel, including when the GPIO selection changes. */
esp_err_t led_lighting_init(uint64_t gpios);
esp_err_t led_lighting_set_gpios(uint64_t gpios);
uint64_t led_lighting_get_gpios(void);
esp_err_t led_lighting_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
