#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Check whether the flash layout is consistent with a first USB installation.
 * NEW/PENDING_VERIFY/INVALID/ABORTED states or any programmed alternate app
 * header rule out a fresh installation. VALID alone is inconclusive because
 * the bootloader can set it on the first USB boot; alternate headers are still
 * inspected in that state. This read-only check does not inspect Matter NVS;
 * callers must also reject existing identity/provisioning before seeding data.
 * The output is changed only on success.
 */
esp_err_t matter_bootstrap_check(bool *fresh_install);

#ifdef __cplusplus
}
#endif
