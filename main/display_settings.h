#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after nvs_flash_init(), before starting consumers. */
esp_err_t display_settings_init(void);

typedef struct {
    bool animation_enabled;
    bool stick_figure_enabled;
} display_settings_state_t;

/* A short, thread-safe snapshot; never waits for flash writes. */
display_settings_state_t display_settings_get(void);
bool display_settings_animation_enabled(void);
bool display_settings_stick_figure_enabled(void);

/* Publish the new value only after it has been committed to NVS. */
esp_err_t display_settings_set_animation_enabled(bool enabled);
esp_err_t display_settings_set_stick_figure_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
