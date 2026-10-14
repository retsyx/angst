#pragma once

#include <stdbool.h>

#include "animation_catalog.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single pending request crosses from the HTTP task to the GUI task. */
esp_err_t animation_test_init(void);
esp_err_t animation_test_request(animation_id_t id);
bool animation_test_take(animation_id_t *id);

/* HTTP reads the GUI's published state; it never touches LVGL objects. */
typedef struct {
    animation_id_t animation_id;
    bool pending;
} animation_status_t;
void animation_test_publish(animation_id_t current);
animation_status_t animation_test_status(void);

#ifdef __cplusplus
}
#endif
