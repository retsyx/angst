#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIGHTING_GPIO_DEFAULT_MASK ((UINT64_C(1) << 45) | (UINT64_C(1) << 46))
#define LIGHTING_GPIO_TEXT_CAPACITY (49 * 3 + 1)

/* Call once after nvs_flash_init(), before starting consumers. */
esp_err_t lighting_settings_init(void);

/* A short, thread-safe snapshot; never waits for flash writes. */
uint64_t lighting_settings_get(void);

/* Accept only a nonempty set of available GPIOs. Publish after NVS commit. */
esp_err_t lighting_settings_set(uint64_t gpios);

/* GPIOs usable for lighting on this board, excluding its reserved hardware. */
uint64_t lighting_gpio_available_mask(void);

/* Parse comma-separated decimal GPIO numbers, permitting surrounding spaces.
 * Empty lists, duplicates, unavailable pins and malformed numbers are rejected.
 * The output is unchanged on failure. */
esp_err_t lighting_gpio_parse(const char *text, uint64_t *gpios);

/* Format a valid set as sorted CSV. Include space for the terminating NUL.
 * The destination is unchanged on failure. */
esp_err_t lighting_gpio_format(uint64_t gpios, char *text, size_t capacity);

#ifdef __cplusplus
}
#endif
