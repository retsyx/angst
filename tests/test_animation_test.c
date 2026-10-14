#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "animation_test.h"
#include "price_mock_idf.h"

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool fail_create;
static atomic_bool fail_take;
static atomic_bool workers_done;
static atomic_uint create_calls;
static atomic_uint consumed;
static _Thread_local bool owns_mutex;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    atomic_fetch_add(&create_calls, 1);
    return atomic_load(&fail_create) ? NULL : &bridge_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == &bridge_mutex && timeout == portMAX_DELAY && !owns_mutex);
    if (atomic_exchange(&fail_take, false)) return pdFALSE;
    assert(pthread_mutex_lock(semaphore) == 0);
    owns_mutex = true;
    sched_yield();
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &bridge_mutex && owns_mutex);
    owns_mutex = false;
    assert(pthread_mutex_unlock(semaphore) == 0);
    return pdTRUE;
}

static void expect_no_take(void)
{
    animation_id_t id = ANIMATION_PHONE;
    assert(!animation_test_take(&id) && id == ANIMATION_PHONE && !owns_mutex);
}

static void expect_take(animation_id_t expected)
{
    animation_id_t id = ANIMATION_NONE;
    assert(animation_test_take(&id) && id == expected && !owns_mutex);
    expect_no_take();
}

static void startup(void)
{
    animation_status_t status = animation_test_status();
    assert(status.animation_id == ANIMATION_NONE && !status.pending);
    animation_test_publish(ANIMATION_PARTY);
    assert(animation_test_request(ANIMATION_NEWSPAPER) == ESP_ERR_INVALID_STATE);
    assert(animation_test_request(ANIMATION_NONE) == ESP_ERR_INVALID_ARG);
    expect_no_take();
    atomic_store(&fail_create, true);
    assert(animation_test_init() == ESP_ERR_NO_MEM);
    expect_no_take();
    atomic_store(&fail_create, false);
    assert(animation_test_init() == ESP_OK);
    assert(animation_test_init() == ESP_OK && atomic_load(&create_calls) == 2);
    for (unsigned i = 1; i <= ANIMATION_COUNT; ++i) {
        assert(animation_test_request((animation_id_t)i) == ESP_OK);
        expect_take((animation_id_t)i);
    }
    assert(!animation_test_take(NULL));
}

static void status_lifecycle(void)
{
    assert(animation_test_init() == ESP_OK);
    animation_status_t status = animation_test_status();
    assert(status.animation_id == ANIMATION_NONE && !status.pending);
    assert(animation_test_request(ANIMATION_PARTY) == ESP_OK);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_NONE && status.pending);
    expect_take(ANIMATION_PARTY);
    animation_test_publish(ANIMATION_PARTY);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_PARTY && !status.pending);
    assert(animation_test_request(ANIMATION_CAT) == ESP_OK);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_PARTY && status.pending);
    animation_test_publish(ANIMATION_NONE);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_NONE && status.pending);
    expect_take(ANIMATION_CAT);
    animation_test_publish(ANIMATION_CAT);
    atomic_store(&fail_take, true);
    animation_test_publish(ANIMATION_NONE);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_CAT && !status.pending);
    animation_test_publish((animation_id_t)(ANIMATION_COUNT + 1));
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_CAT);
    animation_test_publish(ANIMATION_NONE);
    status = animation_test_status();
    assert(status.animation_id == ANIMATION_NONE && !status.pending);
}

static void latest_and_failures(void)
{
    assert(animation_test_init() == ESP_OK);
    assert(animation_test_request(ANIMATION_CLEAN) == ESP_OK);
    assert(animation_test_request(ANIMATION_DOG) == ESP_OK);
    assert(animation_test_request(ANIMATION_UPSET) == ESP_OK);
    assert(animation_test_request((animation_id_t)-1) == ESP_ERR_INVALID_ARG);
    assert(animation_test_request((animation_id_t)(ANIMATION_COUNT + 1)) == ESP_ERR_INVALID_ARG);
    assert(animation_test_request(ANIMATION_NONE) == ESP_ERR_INVALID_ARG);
    assert(!animation_test_take(NULL));
    atomic_store(&fail_take, true);
    assert(animation_test_request(ANIMATION_CELEBRATE) == ESP_ERR_TIMEOUT);
    atomic_store(&fail_take, true);
    expect_no_take();
    expect_take(ANIMATION_UPSET);
}

#define PRODUCERS 4
#define REQUESTS 2000
#define CONSUMERS 2
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static bool start_workers;

static void wait_for_start(void)
{
    assert(pthread_mutex_lock(&start_mutex) == 0);
    while (!start_workers) assert(pthread_cond_wait(&start_condition, &start_mutex) == 0);
    assert(pthread_mutex_unlock(&start_mutex) == 0);
}

static void *produce(void *context)
{
    const unsigned offset = (unsigned)(uintptr_t)context;
    wait_for_start();
    for (unsigned i = 0; i < REQUESTS; ++i) {
        const animation_id_t id = (animation_id_t)((i + offset) % ANIMATION_COUNT + 1);
        assert(animation_test_request(id) == ESP_OK);
        sched_yield();
    }
    return NULL;
}

static void *consume(void *context)
{
    (void)context;
    wait_for_start();
    while (!atomic_load(&workers_done)) {
        animation_id_t id = ANIMATION_NONE;
        if (animation_test_take(&id)) {
            assert(animation_catalog_lookup(id));
            animation_test_publish(id);
            atomic_fetch_add(&consumed, 1);
        } else assert(id == ANIMATION_NONE);
        animation_status_t status = animation_test_status();
        assert(status.animation_id == ANIMATION_NONE || animation_catalog_lookup(status.animation_id));
        sched_yield();
    }
    return NULL;
}

static void concurrent(void)
{
    assert(animation_test_init() == ESP_OK);
    pthread_t producers[PRODUCERS], consumers[CONSUMERS];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(pthread_create(&producers[i], NULL, produce, (void *)(uintptr_t)i) == 0);
    }
    for (unsigned i = 0; i < CONSUMERS; ++i) {
        assert(pthread_create(&consumers[i], NULL, consume, NULL) == 0);
    }
    assert(pthread_mutex_lock(&start_mutex) == 0);
    start_workers = true;
    assert(pthread_cond_broadcast(&start_condition) == 0);
    assert(pthread_mutex_unlock(&start_mutex) == 0);
    for (unsigned i = 0; i < PRODUCERS; ++i) assert(pthread_join(producers[i], NULL) == 0);
    atomic_store(&workers_done, true);
    for (unsigned i = 0; i < CONSUMERS; ++i) assert(pthread_join(consumers[i], NULL) == 0);
    assert(atomic_load(&consumed) > 0 && atomic_load(&consumed) <= PRODUCERS * REQUESTS);
    assert(animation_test_request(ANIMATION_NEWSPAPER) == ESP_OK);
    expect_take(ANIMATION_NEWSPAPER);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "latest_and_failures")) latest_and_failures();
    else if (!strcmp(argv[1], "concurrent")) concurrent();
    else if (!strcmp(argv[1], "status_lifecycle")) status_lifecycle();
    else assert(!"Unknown scenario");
    assert(!owns_mutex);
    printf("PASS animation request bridge: %s\n", argv[1]);
    return 0;
}
