#include "price_test_math.h"

#include <string.h>

typedef enum {
    PRICE_INVALID,
    PRICE_SUPPORTED,
    PRICE_OVERFLOW
} price_parse_result_t;

typedef struct {
    uint64_t units;
    size_t decimal_places;
    size_t integer_min_width;
    char sign;
    bool currency_before_sign;
    bool currency_after_sign;
    bool grouped;
    bool decimal_point;
    bool omit_integer_zero;
} price_format_t;

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v';
}

/* Match the odometer's optional $, sign, $, digits/commas and decimal syntax. */
static price_parse_result_t parse_price(const char *text, price_format_t *format)
{
    memset(format, 0, sizeof(*format));
    if (!text) return PRICE_INVALID;
    const char *end = text + strlen(text);
    while (text < end && whitespace(*text)) ++text;
    while (text < end && whitespace(end[-1])) --end;

    if (text < end && *text == '$') {
        format->currency_before_sign = true;
        ++text;
    }
    if (text < end && (*text == '-' || *text == '+')) format->sign = *text++;
    if (text < end && *text == '$') {
        format->currency_after_sign = true;
        ++text;
    }

    size_t integer_digits = 0;
    bool leading_integer_zero = false;
    bool found_digit = false;
    bool overflow = false;
    for (; text < end; ++text) {
        if (digit(*text)) {
            found_digit = true;
            const uint64_t value = (uint64_t)(*text - '0');
            if (!overflow) {
                if (format->units > (UINT64_MAX - value) / 10) overflow = true;
                else format->units = format->units * 10 + value;
            }
            if (format->decimal_point) ++format->decimal_places;
            else {
                if (!integer_digits) leading_integer_zero = value == 0;
                ++integer_digits;
            }
        } else if (*text == '.' && !format->decimal_point) {
            format->decimal_point = true;
        } else if (*text == ',' && !format->decimal_point && integer_digits &&
                   text + 1 < end && digit(text[1])) {
            format->grouped = true;
        } else {
            return PRICE_INVALID;
        }
    }
    if (!found_digit) return PRICE_INVALID;
    format->omit_integer_zero = integer_digits == 0;
    if (leading_integer_zero) format->integer_min_width = integer_digits;
    return overflow ? PRICE_OVERFLOW : PRICE_SUPPORTED;
}

static bool add_size(size_t *total, size_t amount)
{
    if (amount > SIZE_MAX - *total) return false;
    *total += amount;
    return true;
}

bool price_test_generate(const char *current, bool increase, uint32_t randomness,
                         char *output, size_t capacity, bool *used_fallback)
{
    if (!output || !capacity) return false;
    price_format_t format;
    const price_parse_result_t parsed = parse_price(current, &format);
    if (parsed == PRICE_OVERFLOW) return false;
    const bool fallback = parsed == PRICE_INVALID;
    if (fallback) {
        (void)parse_price("$4,000", &format);
    }

    const uint32_t basis_points = 100 + randomness % 401;
    /* Split the multiplication so even UINT64_MAX units can be rounded safely. */
    uint64_t change = format.units / 10000 * basis_points +
                     (format.units % 10000 * basis_points + 5000) / 10000;
    if (!change) change = 1;
    bool negative = format.sign == '-' && format.units != 0;
    uint64_t result;
    if (increase != negative) {
        if (change > UINT64_MAX - format.units) return false;
        result = format.units + change;
    } else if (change <= format.units) {
        result = format.units - change;
        if (!result) negative = false;
    } else {
        result = change - format.units;
        negative = !negative;
    }

    char reversed[20], digits[20];
    size_t digit_count = 0;
    do {
        reversed[digit_count++] = (char)('0' + result % 10);
        result /= 10;
    } while (result);
    for (size_t i = 0; i < digit_count; ++i) digits[i] = reversed[digit_count - i - 1];

    const size_t integer_value_digits = digit_count > format.decimal_places
                                           ? digit_count - format.decimal_places : 0;
    const size_t shown_integer_digits = integer_value_digits ? integer_value_digits
                                           : format.omit_integer_zero ? 0 : 1;
    const size_t integer_width = shown_integer_digits > format.integer_min_width
                                    ? shown_integer_digits : format.integer_min_width;
    const char sign = negative ? '-' : format.sign == '+' ? '+' : '\0';
    size_t required = 1; /* NUL terminator. */
    if (!add_size(&required, format.currency_before_sign ? 1 : 0) ||
        !add_size(&required, format.currency_after_sign ? 1 : 0) ||
        !add_size(&required, sign ? 1 : 0) ||
        !add_size(&required, integer_width) ||
        !add_size(&required, format.grouped && integer_width ? (integer_width - 1) / 3 : 0) ||
        !add_size(&required, format.decimal_point ? 1 : 0) ||
        !add_size(&required, format.decimal_places) || required > capacity) return false;

    char *cursor = output;
    if (format.currency_before_sign) *cursor++ = '$';
    if (sign) *cursor++ = sign;
    if (format.currency_after_sign) *cursor++ = '$';
    const size_t integer_padding = integer_width - shown_integer_digits;
    for (size_t i = 0; i < integer_width; ++i) {
        if (format.grouped && i && (integer_width - i) % 3 == 0) *cursor++ = ',';
        *cursor++ = i < integer_padding || !integer_value_digits
                        ? '0' : digits[i - integer_padding];
    }
    if (format.decimal_point) *cursor++ = '.';
    if (format.decimal_places) {
        const size_t fractional_padding = format.decimal_places > digit_count
                                             ? format.decimal_places - digit_count : 0;
        memset(cursor, '0', fractional_padding);
        cursor += fractional_padding;
        const size_t fractional_digits = format.decimal_places < digit_count
                                             ? format.decimal_places : digit_count;
        memcpy(cursor, digits + integer_value_digits, fractional_digits);
        cursor += fractional_digits;
    }
    *cursor = '\0';
    if (used_fallback) *used_fallback = fallback;
    return true;
}
