#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All display mutations stay on the GUI task. This bridge holds only text. */
esp_err_t price_test_init(void);

/* Queue a random 1–5% change, retaining it for 60 seconds after this request. */
esp_err_t price_test_request(bool increase, char *result, size_t capacity);

/* Save the latest live response; false means a test temporarily takes priority. */
bool price_test_accept_live(const char *text);

/* Take a queued test or the saved live response when the test period expires. */
bool price_test_take(char *result, size_t capacity, bool *used_fallback,
                     bool *is_test);

#ifdef __cplusplus
}
#endif
