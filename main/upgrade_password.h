#pragma once

#include <stddef.h>

#include "esp_err.h"

#define UPGRADE_PASSWORD_MAX_LENGTH 128
#define UPGRADE_PASSWORD_GENERATED_LENGTH 10

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ensure a persisted password exists without retaining a plaintext copy.
 * Call at boot after NVS and Wi-Fi/Bluetooth are initialized for hardware RNG
 * entropy, even if the web server is disabled.
 */
esp_err_t upgrade_password_init(void);

/*
 * Load the NVS password. If its namespace or key is absent, generate a random
 * 10-character password and commit it before returning it. Storage errors or
 * corrupt existing passwords fail without creating a replacement.
 * The output buffer is changed only on success; capacity includes the terminator.
 * Call after NVS and Wi-Fi/Bluetooth are initialized for hardware RNG entropy.
 */
esp_err_t upgrade_password_load(char *password, size_t capacity);

/*
 * Save and commit 1–128 printable ASCII bytes without leading or trailing spaces.
 * NVS keeps this password across reboots and OTA upgrades.
 * The caller should update its active password only after this returns ESP_OK.
 */
esp_err_t upgrade_password_save(const char *password);

#ifdef __cplusplus
}
#endif
