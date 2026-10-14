#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the local display settings and firmware upload page on port 80. */
esp_err_t ota_server_start(void);

#ifdef __cplusplus
}
#endif
