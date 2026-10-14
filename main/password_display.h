#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the initially hidden physical-screen overlay on the LVGL task, before
 * starting the web server. Pass the display's LVGL top layer as the parent so
 * changing price objects remain underneath it. Repeated initialization returns
 * ESP_ERR_INVALID_STATE.
 */
esp_err_t password_display_init(lv_obj_t *screen);

/*
 * Request a physical-screen display of 1–128 printable ASCII password bytes,
 * without leading or trailing spaces. Safe from the HTTP task: it copies into
 * private storage and never calls LVGL, logs the password, or returns its value.
 * ESP_ERR_INVALID_STATE means initialization is incomplete or a request is
 * already pending/showing; ESP_ERR_INVALID_ARG means the password is invalid.
 */
esp_err_t password_display_request(const char *password);

/*
 * Advance the overlay on the LVGL task. Returns true while a request is pending
 * or visible, so the caller can pause price rendering and brighten the panel.
 * A fitting password remains for 15 seconds. An overflowing password makes two
 * smooth trips to the far end and back at 90 pixels/second, pausing for 2 seconds
 * at the start and each endpoint. Text is rasterized once so scrolling only copies
 * the visible pixels.
 * On completion, the label is detached and both private text and pixels are erased.
 * Password changes during display apply to the next request, not this snapshot.
 */
bool password_display_update(void);

#ifdef __cplusplus
}
#endif
