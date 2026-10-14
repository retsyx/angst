#include "price_test_math.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void expect(const char *input, bool increase, uint32_t randomness,
                   const char *expected, bool expected_fallback)
{
    char output[1024];
    memset(output, 0xa5, sizeof(output));
    bool fallback = !expected_fallback;
    assert(price_test_generate(input, increase, randomness, output,
                               sizeof(output), &fallback));
    assert(strcmp(output, expected) == 0);
    assert(fallback == expected_fallback);
    assert((unsigned char)output[strlen(expected) + 1] == 0xa5);
    ++checks;
}

static void expect_failure(const char *input, bool increase, uint32_t randomness,
                           size_t capacity, bool initial_fallback)
{
    char output[128], before[128];
    memset(output, 0xa5, sizeof(output));
    memcpy(output, "unchanged", sizeof("unchanged"));
    memcpy(before, output, sizeof(before));
    bool fallback = initial_fallback;
    assert(!price_test_generate(input, increase, randomness, output, capacity, &fallback));
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(fallback == initial_fallback);
    ++checks;
}

int main(void)
{
    /* Both percentage endpoints, nearest-unit rounding, and formatting. */
    expect("$4,123.75", true, 0, "$4,164.99", false);
    expect("$4,123.75", false, 0, "$4,082.51", false);
    expect("$4,123.75", true, 400, "$4,329.94", false);
    expect("$4,123.75", false, 400, "$3,917.56", false);
    expect("50.00", true, 1, "50.51", false); /* 0.505 rounds to 0.51. */
    expect("50.00", false, 1, "49.49", false);
    expect("49.00", true, 1, "49.49", false); /* 0.4949 rounds to 0.49. */
    expect("49.00", false, 1, "48.51", false);
    expect("100.00", true, 0, "101.00", false);
    expect("$9,999.99", true, 0, "$10,099.99", false);
    expect("$9,999.99", false, 0, "$9,899.99", false);
    expect("1.00", false, 0, "0.99", false);
    expect("10.000", true, 0, "10.100", false);
    expect("10.000", false, 0, "9.900", false);
    expect("1.", true, 0, "2.", false);

    /* Increasing a negative price moves it toward zero. */
    expect("-$10.00", true, 0, "-$9.90", false);
    expect("-$10.00", false, 0, "-$10.10", false);
    expect("$-10.00", true, 0, "$-9.90", false);
    expect("$-10.00", false, 0, "$-10.10", false);
    expect("+$10.00", true, 0, "+$10.10", false);
    expect("+$10.00", false, 0, "+$9.90", false);
    expect("$+10.00", false, 0, "$+9.90", false);
    expect("  \t$4,000.00\r\n", true, 0, "$4,040.00", false);

    /* Tiny and zero prices still change by one displayed unit. */
    expect("0", true, 0, "1", false);
    expect("0", false, 400, "-1", false);
    expect("$0.00", true, 0, "$0.01", false);
    expect("$0.00", false, 0, "$-0.01", false);
    expect("-$0.00", false, 0, "-$0.01", false);
    expect("-0.00", true, 0, "0.01", false);
    expect("-0.00", false, 0, "-0.01", false);
    expect("0.01", true, 400, "0.02", false);
    expect("0.01", false, 0, "0.00", false);
    expect("-0.01", true, 0, "0.00", false);
    expect("-0.01", false, 0, "-0.02", false);
    expect("+0.01", false, 0, "+0.00", false);
    expect("$.01", false, 0, "$.00", false);
    expect("+.09", true, 0, "+.10", false);
    expect(".99", true, 0, "1.00", false);
    expect(".0", false, 0, "-.1", false);
    expect("0.000", false, 0, "-0.001", false);
    expect("000.99", true, 0, "001.00", false);
    expect("000,001.00", true, 0, "000,001.01", false);
    expect("12,34.50", true, 0, "1,246.85", false);
    expect("$-$.25", true, 0, "$-$.24", false);

    /* Values above floating-point integer precision remain exact. */
    expect("9007199254740993", true, 0, "9097271247288403", false);
    expect("9007199254740993", false, 0, "8917127262193583", false);
    expect("-9007199254740993", true, 0, "-8917127262193583", false);
    expect("18,446,744,073,709,551,615", false, 0,
           "18,262,276,632,972,456,099", false);
    expect("-18446744073709551615", true, 0, "-18262276632972456099", false);
    expect("0.123456789012345678", true, 0, "0.124691356902469135", false);

    for (const char **input = (const char *[]){"Angst", "Error 500", "", " \t\r\n",
            "$", "-", ".", "1..2", "1,,000", "1,", "123e3", "12 34", "++12",
            "999999999999999999999999USD", NULL}; *input; ++input) {
        expect(*input, true, 0, "$4,040", true);
        expect(*input, false, 400, "$3,800", true);
    }
    expect(NULL, true, 0, "$4,040", true);

    /* Every selectable percentage has the right numeric direction and range. */
    for (uint32_t randomness = 0; randomness <= 400; ++randomness) {
        char positive_up[64], positive_down[64], negative_up[64], negative_down[64];
        assert(price_test_generate("10000", true, randomness, positive_up, sizeof(positive_up), NULL));
        assert(price_test_generate("10000", false, randomness, positive_down, sizeof(positive_down), NULL));
        assert(price_test_generate("-10000", true, randomness, negative_up, sizeof(negative_up), NULL));
        assert(price_test_generate("-10000", false, randomness, negative_down, sizeof(negative_down), NULL));
        const long up = strtol(positive_up, NULL, 10);
        const long down = strtol(positive_down, NULL, 10);
        assert(up >= 10100 && up <= 10500);
        assert(down >= 9500 && down <= 9900);
        assert(up + down == 20000);
        assert(strtol(negative_up, NULL, 10) == -down);
        assert(strtol(negative_down, NULL, 10) == -up);
        char repeated[64];
        assert(price_test_generate("10000", true, randomness + 401, repeated, sizeof(repeated), NULL));
        assert(strcmp(positive_up, repeated) == 0);
        checks += 5;
    }
    char maximum_random[64];
    assert(price_test_generate("10000", true, UINT32_MAX, maximum_random, sizeof(maximum_random), NULL));
    assert(strtol(maximum_random, NULL, 10) >= 10100 && strtol(maximum_random, NULL, 10) <= 10500);
    ++checks;

    /* Fractional precision is not constrained by the size of a power of ten. */
    char tiny[1024], tiny_expected[1024], tiny_output[1024];
    memset(tiny, '0', 242);
    tiny[0] = '.';
    tiny[241] = '1';
    tiny[242] = '\0';
    memcpy(tiny_expected, tiny, 243);
    tiny_expected[241] = '2';
    expect(tiny, true, 0, tiny_expected, false);
    tiny_expected[241] = '0';
    expect(tiny, false, 400, tiny_expected, false);
    assert(price_test_generate(tiny, true, 0, tiny_output, 243, NULL));
    assert(strlen(tiny_output) == 242);
    char untouched[1024];
    memset(untouched, 0xa5, sizeof(untouched));
    memcpy(tiny_output, untouched, sizeof(untouched));
    assert(!price_test_generate(tiny, true, 0, tiny_output, 242, NULL));
    assert(memcmp(tiny_output, untouched, sizeof(untouched)) == 0);
    checks += 2;

    /* Parse/result overflow and insufficient capacity never partly replace text. */
    expect_failure("18446744073709551616", true, 0, 128, true);
    expect_failure("-18446744073709551616", false, 400, 128, false);
    expect_failure("1.00000000000000000000", true, 0, 128, false);
    expect_failure("18446744073709551615", true, 0, 128, true);
    expect_failure("-18446744073709551615", false, 400, 128, false);
    expect_failure("$4,123.75", true, 0, 9, false);
    expect_failure("Angst", true, 0, 6, false);
    expect_failure("1", true, 0, 0, true);
    bool fallback = true;
    assert(!price_test_generate("1", true, 0, NULL, 8, &fallback) && fallback);
    ++checks;
    char exact[10];
    assert(price_test_generate("$4,123.75", true, 0, exact, sizeof(exact), NULL));
    assert(strcmp(exact, "$4,164.99") == 0);
    ++checks;
    char alias[64] = "$4,123.75";
    assert(price_test_generate(alias, true, 0, alias, sizeof(alias), NULL));
    assert(strcmp(alias, "$4,164.99") == 0);
    ++checks;

    printf("PASS %u fixed-point price generation, signed direction, formatting, range, precision and failure checks\n", checks);
    return 0;
}
