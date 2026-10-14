#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_DISPLAY_WIDTH 536
#define BOARD_DISPLAY_HEIGHT 240

/* Call once, after lv_init(), from the task that owns LVGL. */
esp_err_t board_display_init(lv_disp_t **display);

/*
 * Set the panel brightness from 0 (off) to 255 (100%). Initialization starts
 * at 255. Call from the same task as board_display_init() and LVGL updates;
 * this waits for any outstanding display DMA transfer before changing brightness.
 */
esp_err_t board_display_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
