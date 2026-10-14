#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Change a price by 100 + randomness % 401 basis points (1..5 percent).
 * Round half up at its existing decimal precision, with a minimum change of
 * one displayed unit. The direction is numeric, including for negative prices.
 *
 * Preserve currency/sign placement, explicit plus, grouping, decimal precision,
 * and leading integer zeros. Commas are normalized to groups of three digits.
 * Non-prices use "$4,000" as the baseline before applying the requested change.
 *
 * Significant decimal units and the resulting magnitude must fit uint64_t;
 * long fractional precision is supported without floating-point conversion.
 * On failure, output and used_fallback are unchanged. used_fallback may be NULL.
 * output may alias current if its buffer has the supplied capacity.
 */
bool price_test_generate(const char *current, bool increase, uint32_t randomness,
                         char *output, size_t capacity, bool *used_fallback);

#ifdef __cplusplus
}
#endif
