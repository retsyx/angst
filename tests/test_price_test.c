#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "price_test.h"
#include "service.h"
#include "price_mock_idf.h"

#define PERIOD_US INT64_C(60000000)

static pthread_mutex_t controller_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool fail_create;
static atomic_bool fail_take;
static atomic_bool encourage_contention;
static atomic_uint create_calls;
static atomic_uint random_calls;
static atomic_uint contended_locks;
static atomic_uint random_value;
static atomic_int_fast64_t fake_time;
static _Thread_local bool owns_controller_mutex;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    atomic_fetch_add(&create_calls, 1);
    return atomic_load(&fail_create) ? NULL : &controller_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == &controller_mutex);
    assert(timeout == portMAX_DELAY);
    assert(!owns_controller_mutex);
    if (atomic_exchange(&fail_take, false)) return pdFALSE;
    const int result = pthread_mutex_trylock(semaphore);
    if (result == EBUSY) {
        atomic_fetch_add(&contended_locks, 1);
        assert(pthread_mutex_lock(semaphore) == 0);
    } else assert(result == 0);
    owns_controller_mutex = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &controller_mutex && owns_controller_mutex);
    owns_controller_mutex = false;
    assert(pthread_mutex_unlock(semaphore) == 0);
    return pdTRUE;
}

uint32_t esp_random(void)
{
    assert(owns_controller_mutex);
    atomic_fetch_add(&random_calls, 1);
    if (atomic_load(&encourage_contention)) {
        for (unsigned i = 0; i < 4; ++i) sched_yield();
    }
    return atomic_load(&random_value);
}

int64_t esp_timer_get_time(void)
{
    assert(owns_controller_mutex);
    return atomic_load(&fake_time);
}

static void init(void)
{
    assert(price_test_init() == ESP_OK);
    assert(!owns_controller_mutex);
}

static void expect_request(bool increase, uint32_t randomness, const char *expected)
{
    char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
    atomic_store(&random_value, randomness);
    assert(price_test_request(increase, result, sizeof(result)) == ESP_OK);
    assert(strcmp(result, expected) == 0);
    assert(!owns_controller_mutex);
}

static void expect_take(const char *expected, bool expected_fallback, bool expected_test)
{
    char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
    bool fallback = !expected_fallback, is_test = !expected_test;
    assert(price_test_take(result, sizeof(result), &fallback, &is_test));
    assert(strcmp(result, expected) == 0);
    assert(fallback == expected_fallback && is_test == expected_test);
    assert(!owns_controller_mutex);
}

static void expect_no_take(size_t capacity)
{
    char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
    bool fallback = true, is_test = true;
    assert(!price_test_take(result, capacity, &fallback, &is_test));
    assert(strcmp(result, "unchanged") == 0);
    assert(fallback && is_test);
    assert(!owns_controller_mutex);
}

static void uninitialized(void)
{
    unsigned creates_before = atomic_load(&create_calls);
    char result[64] = "unchanged";
    assert(price_test_request(true, result, sizeof(result)) == ESP_ERR_INVALID_STATE);
    assert(strcmp(result, "unchanged") == 0);
    assert(price_test_request(true, NULL, sizeof(result)) == ESP_ERR_INVALID_ARG);
    assert(price_test_request(false, result, 0) == ESP_ERR_INVALID_ARG);
    assert(!price_test_accept_live("$100.00"));
    assert(!price_test_accept_live(NULL));
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    assert(atomic_load(&create_calls) == creates_before && atomic_load(&random_calls) == 0);
}

static void initialization_failure(void)
{
    atomic_store(&fail_create, true);
    assert(price_test_init() == ESP_ERR_NO_MEM);
    assert(price_test_init() == ESP_ERR_NO_MEM);
    uninitialized();
    atomic_store(&fail_create, false);
    init();
    assert(atomic_load(&create_calls) == 3);
    init();
    assert(atomic_load(&create_calls) == 3); // Idempotent after successful startup.
    expect_request(true, 0, "$4,040");
    expect_take("$4,040", true, true);
}

static void fallback(void)
{
    init();
    expect_request(true, 0, "$4,040");
    expect_take("$4,040", true, true);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    expect_request(false, 0, "$4,000");
    expect_take("$4,000", false, true);
}

static void cumulative(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    expect_request(true, 400, "$106.05");
    expect_request(false, 400, "$100.75");
    expect_request(true, 200, "$103.77");
    expect_take("$103.77", false, true); // Four clicks produce one latest update.
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    assert(!price_test_accept_live("$900.00"));
    expect_request(false, 0, "$102.73"); // Live responses do not replace the test base.
    expect_take("$102.73", false, true);
    atomic_store(&fake_time, PERIOD_US);
    expect_take("$900.00", false, false);
    assert(price_test_accept_live("-$100.00"));
    expect_request(true, 0, "-$99.00");
    expect_request(false, 400, "-$103.95");
    expect_take("-$103.95", false, true);
}

static void restoration_deadline(void)
{
    init();
    const char *latest[] = {"$200.00", "Error 500", "", "Error ESP_ERR_HTTP_CONNECT"};
    for (unsigned i = 0; i < sizeof(latest) / sizeof(latest[0]); ++i) {
        const int64_t started = (int64_t)i * PERIOD_US * 2 + 1000;
        atomic_store(&fake_time, started);
        assert(price_test_accept_live("$100.00"));
        expect_request(true, 0, "$101.00");
        assert(!price_test_accept_live("$150.00"));
        assert(!price_test_accept_live(latest[i]));
        expect_take("$101.00", false, true);
        atomic_store(&fake_time, started + PERIOD_US - 1);
        expect_no_take(SERVICE_MESSAGE_CAPACITY);
        atomic_store(&fake_time, started + PERIOD_US);
        expect_take(latest[i], false, false);
        expect_no_take(SERVICE_MESSAGE_CAPACITY);
    }
    // A fresh live response arriving exactly at expiry applies immediately.
    atomic_store(&fake_time, PERIOD_US * 10);
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    atomic_store(&fake_time, PERIOD_US * 11);
    assert(price_test_accept_live(""));
    expect_no_take(SERVICE_MESSAGE_CAPACITY); // Discard an unconsumed old test.
    expect_request(false, 0, "$3,960");
    expect_take("$3,960", true, true);
}

static void extended_deadline(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    atomic_store(&fake_time, PERIOD_US - 1000000);
    expect_request(true, 0, "$102.01");
    assert(!price_test_accept_live("$200.00"));
    expect_take("$102.01", false, true);
    atomic_store(&fake_time, PERIOD_US);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    atomic_store(&fake_time, PERIOD_US * 2 - 1000001);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    atomic_store(&fake_time, PERIOD_US * 2 - 1000000);
    expect_take("$200.00", false, false);
}

static void request_capacity(void)
{
    init();
    char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
    expect_request(true, 0, "$4,040");
    assert(!price_test_accept_live("$100.00"));
    atomic_store(&fake_time, PERIOD_US / 2);
    atomic_store(&random_value, 400);
    assert(price_test_request(true, result, strlen("$4,242")) == ESP_ERR_INVALID_ARG);
    assert(strcmp(result, "unchanged") == 0);
    expect_take("$4,040", true, true); // Failed output preserves the original fallback flag.
    atomic_store(&fake_time, PERIOD_US);
    expect_take("$100.00", false, false); // Failure did not extend the deadline.
    expect_request(true, 0, "$101.00");
    atomic_store(&random_value, 400);
    assert(price_test_request(true, result, strlen("$106.05")) == ESP_ERR_INVALID_ARG);
    assert(strcmp(result, "unchanged") == 0);
    expect_request(true, 0, "$102.01"); // Failure did not mutate the cumulative base.
    expect_no_take(strlen("$102.01"));
    expect_take("$102.01", false, true);
}

static void take_capacity(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
    bool fallback = true, is_test = true;
    assert(!price_test_take(NULL, sizeof(result), &fallback, &is_test));
    assert(!price_test_take(result, sizeof(result), NULL, &is_test));
    assert(!price_test_take(result, sizeof(result), &fallback, NULL));
    assert(strcmp(result, "unchanged") == 0 && fallback && is_test);
    expect_no_take(0);
    expect_no_take(strlen("$101.00"));
    assert(price_test_take(result, strlen("$101.00") + 1, &fallback, &is_test));
    assert(strcmp(result, "$101.00") == 0 && !fallback && is_test);

    char long_live[SERVICE_MESSAGE_CAPACITY];
    memset(long_live, 'X', sizeof(long_live) - 1);
    long_live[sizeof(long_live) - 1] = '\0';
    assert(!price_test_accept_live(long_live));
    atomic_store(&fake_time, PERIOD_US);
    expect_no_take(strlen(long_live));
    expect_take(long_live, false, false);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    expect_request(true, 0, "$4,040"); // Restored non-price text selects the fallback.
    assert(!price_test_accept_live(""));
    atomic_store(&fake_time, PERIOD_US * 2);
    assert(price_test_take(result, 1, &fallback, &is_test));
    assert(result[0] == '\0' && !fallback && !is_test);
}

static void no_live_expiry(void)
{
    init();
    expect_request(true, 0, "$4,040");
    expect_take("$4,040", true, true);
    atomic_store(&fake_time, PERIOD_US - 1);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    atomic_store(&fake_time, PERIOD_US);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    atomic_store(&fake_time, PERIOD_US * 3);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    assert(price_test_accept_live("Error 500"));
    expect_request(true, 0, "$4,040");
    atomic_store(&fake_time, PERIOD_US * 4);
    expect_take("Error 500", false, false);
}

static void invalid_live(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    char too_long[SERVICE_MESSAGE_CAPACITY + 1];
    memset(too_long, '9', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(!price_test_accept_live(NULL));
    assert(!price_test_accept_live(too_long));
    atomic_store(&fake_time, PERIOD_US);
    expect_take("$100.00", false, false);
    assert(price_test_accept_live("18446744073709551615"));
    char result[64] = "unchanged";
    assert(price_test_request(true, result, sizeof(result)) == ESP_ERR_INVALID_ARG);
    assert(strcmp(result, "unchanged") == 0);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    expect_take("$101.00", false, true);
}

static void lock_failure(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    atomic_store(&fake_time, PERIOD_US / 2);
    char result[64] = "unchanged";
    unsigned entropy_before = atomic_load(&random_calls);
    atomic_store(&fail_take, true);
    assert(price_test_request(false, result, sizeof(result)) == ESP_ERR_TIMEOUT);
    assert(strcmp(result, "unchanged") == 0);
    assert(atomic_load(&random_calls) == entropy_before);
    atomic_store(&fail_take, true);
    assert(!price_test_accept_live("$300.00"));
    atomic_store(&fail_take, true);
    expect_no_take(SERVICE_MESSAGE_CAPACITY);
    expect_take("$101.00", false, true);
    atomic_store(&fake_time, PERIOD_US);
    expect_take("$100.00", false, false);
}

#define PRODUCERS 6
#define REQUESTS 40
#define CONSUMERS 2
#define CONSUMER_TRIES 400
#define PRICE_CAPACITY 64
static char expected_prices[PRODUCERS * REQUESTS + 1][PRICE_CAPACITY];
static char producer_prices[PRODUCERS][REQUESTS][PRICE_CAPACITY];
static atomic_uint successful_takes;
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static bool start_workers;

static void wait_for_workers(void)
{
    assert(pthread_mutex_lock(&start_mutex) == 0);
    while (!start_workers) assert(pthread_cond_wait(&start_condition, &start_mutex) == 0);
    assert(pthread_mutex_unlock(&start_mutex) == 0);
}

static void format_cents(char *result, uint64_t cents)
{
    assert(snprintf(result, PRICE_CAPACITY, "$%" PRIu64 ".%02" PRIu64,
                    cents / 100, cents % 100) > 0);
}

static bool expected_price(const char *text)
{
    for (unsigned i = 0; i <= PRODUCERS * REQUESTS; ++i) {
        if (strcmp(text, expected_prices[i]) == 0) return true;
    }
    return false;
}

static void *producer(void *argument)
{
    const unsigned index = (unsigned)(uintptr_t)argument;
    wait_for_workers();
    for (unsigned i = 0; i < REQUESTS; ++i) {
        assert(price_test_request(true, producer_prices[index][i], PRICE_CAPACITY) == ESP_OK);
        sched_yield();
    }
    return NULL;
}

static void *consumer(void *argument)
{
    (void)argument;
    wait_for_workers();
    for (unsigned i = 0; i < CONSUMER_TRIES; ++i) {
        char result[SERVICE_MESSAGE_CAPACITY] = "unchanged";
        bool fallback = true, is_test = false;
        if (price_test_take(result, sizeof(result), &fallback, &is_test)) {
            assert(!fallback && is_test);
            assert(expected_price(result)); // No partial or torn text is observable.
            atomic_fetch_add(&successful_takes, 1);
        } else assert(strcmp(result, "unchanged") == 0 && fallback && !is_test);
        sched_yield();
    }
    return NULL;
}

static void *live_responses(void *argument)
{
    (void)argument;
    wait_for_workers();
    for (unsigned i = 0; i < CONSUMER_TRIES; ++i) {
        char live[64];
        assert(snprintf(live, sizeof(live), "Error %u", i) > 0);
        assert(!price_test_accept_live(live));
        sched_yield();
    }
    return NULL;
}

static int compare_prices(const void *left, const void *right)
{
    return strcmp(left, right);
}

static void concurrent(void)
{
    init();
    assert(price_test_accept_live("$100.00"));
    expect_request(true, 0, "$101.00");
    uint64_t cents = 10100;
    format_cents(expected_prices[0], cents);
    for (unsigned i = 1; i <= PRODUCERS * REQUESTS; ++i) {
        cents = (cents * 101 + 50) / 100;
        format_cents(expected_prices[i], cents);
    }
    const uint64_t final_cents = cents;
    atomic_store(&encourage_contention, true);
    pthread_t threads[PRODUCERS + CONSUMERS + 1];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(pthread_create(&threads[i], NULL, producer, (void *)(uintptr_t)i) == 0);
    }
    for (unsigned i = 0; i < CONSUMERS; ++i) {
        assert(pthread_create(&threads[PRODUCERS + i], NULL, consumer, NULL) == 0);
    }
    assert(pthread_create(&threads[PRODUCERS + CONSUMERS], NULL, live_responses, NULL) == 0);
    assert(pthread_mutex_lock(&start_mutex) == 0);
    start_workers = true;
    assert(pthread_cond_broadcast(&start_condition) == 0);
    assert(pthread_mutex_unlock(&start_mutex) == 0);
    for (unsigned i = 0; i < PRODUCERS + CONSUMERS + 1; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    atomic_store(&encourage_contention, false);
    assert(atomic_load(&successful_takes) > 0);
    char actual[PRODUCERS * REQUESTS][PRICE_CAPACITY];
    memcpy(actual, producer_prices, sizeof(actual));
    qsort(actual, PRODUCERS * REQUESTS, PRICE_CAPACITY, compare_prices);
    qsort(expected_prices + 1, PRODUCERS * REQUESTS, PRICE_CAPACITY, compare_prices);
    assert(memcmp(actual, expected_prices + 1, sizeof(actual)) == 0); // Every increment committed once.
    char final_decrease[PRICE_CAPACITY];
    format_cents(final_decrease, (final_cents * 99 + 50) / 100);
    expect_request(false, 0, final_decrease);
    expect_take(final_decrease, false, true);
    assert(!price_test_accept_live("Error 599"));
    atomic_store(&fake_time, PERIOD_US);
    expect_take("Error 599", false, false);
    printf("Concurrent run: %u queued takes, %u contended mutex acquisitions\n",
           atomic_load(&successful_takes), atomic_load(&contended_locks));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"uninitialized", uninitialized}, {"initialization_failure", initialization_failure},
        {"fallback", fallback}, {"cumulative", cumulative},
        {"restoration_deadline", restoration_deadline}, {"extended_deadline", extended_deadline},
        {"request_capacity", request_capacity}, {"take_capacity", take_capacity},
        {"no_live_expiry", no_live_expiry}, {"invalid_live", invalid_live},
        {"lock_failure", lock_failure}, {"concurrent", concurrent},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        assert(!owns_controller_mutex);
        printf("PASS price-test controller: %s\n", cases[i].name);
        return 0;
    }
    assert(!"Unknown scenario");
    return 1;
}
