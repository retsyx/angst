#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MATTER_PROVISIONING_SERIAL_MAX_LENGTH 32
#define MATTER_PROVISIONING_SALT_LENGTH 32
#define MATTER_PROVISIONING_VERIFIER_LENGTH 97
#define MATTER_PROVISIONING_ROTATING_ID_LENGTH 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t discriminator;
    uint32_t iterations;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[MATTER_PROVISIONING_SERIAL_MAX_LENGTH + 1];
    uint8_t salt[MATTER_PROVISIONING_SALT_LENGTH];
    uint8_t verifier[MATTER_PROVISIONING_VERIFIER_LENGTH];
    uint8_t rotating_id[MATTER_PROVISIONING_ROTATING_ID_LENGTH];
} matter_provisioning_identity_t;

/*
 * Load the immutable device identity from the default NVS partition. Existing
 * records always win, regardless of seed or allow_seed, and are opened read-only.
 * Only an absent namespace/key may be seeded. A missing record with allow_seed
 * false returns ESP_ERR_NOT_FOUND. Seeding commits before changing the output.
 * Storage errors and invalid records never erase or replace the identity, and
 * every failure leaves the output unchanged. Call after NVS initialization,
 * during startup before registering the Matter providers.
 */
esp_err_t matter_provisioning_load(const matter_provisioning_identity_t *seed,
                                 bool allow_seed,
                                 matter_provisioning_identity_t *identity);

#ifdef __cplusplus
}
#endif
