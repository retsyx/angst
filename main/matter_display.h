#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS, the default event loop, and service listeners must exist first.
 * Returns ESP_OK only after the Matter server reports readiness. */
esp_err_t matter_display_start(void);

/* Refuse OTA before any app-slot write unless the original pairing identity
 * is safely persisted and still matches the running Matter providers. */
esp_err_t matter_display_prepare_ota(void);

/* Thread-safe snapshot. Apply it from the task that owns the display driver. */
uint8_t matter_display_brightness(void);

#ifdef __cplusplus
}
#endif
