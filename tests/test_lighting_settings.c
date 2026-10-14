#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lighting_settings_mock_idf.h"
#include "../main/lighting_settings.c"

static uint64_t stored, pending;
static bool present, dirty, invalid_type;
static int open_error, get_error, set_error, commit_error;
static bool fail_mutex_create, fail_mutex_take;
static unsigned open_calls, set_calls, commit_calls, live_handles, live_mutexes;
static unsigned next_handle;
static const char *storage_path;
static pthread_mutex_t flash_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flash_changed = PTHREAD_COND_INITIALIZER;
static bool block_commit, commit_entered, release_commit, reader_finished;
static const uint64_t FIRST = UINT64_C(1) << 1;
static const uint64_t SECOND = (UINT64_C(1) << 10) | (UINT64_C(1) << 40) |
    LIGHTING_GPIO_DEFAULT_MASK;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (fail_mutex_create) return NULL;
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    assert(mutex && !pthread_mutex_init(mutex, NULL));
    ++live_mutexes;
    return mutex;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(mutex && live_mutexes && !pthread_mutex_destroy(mutex));
    free(mutex);
    --live_mutexes;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t delay)
{
    assert(mutex && delay == portMAX_DELAY);
    if (fail_mutex_take) return 0;
    assert(!pthread_mutex_lock(mutex));
    return pdTRUE;
}

void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(!pthread_mutex_unlock(mutex));
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "lighting_cfg") && mode == NVS_READWRITE);
    ++open_calls;
    if (open_error) return open_error;
    *handle = ++next_handle;
    ++live_handles;
    return ESP_OK;
}

esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value)
{
    assert(handle && live_handles && !strcmp(key, "gpios"));
    if (get_error) return get_error;
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (invalid_type) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = stored;
    return ESP_OK;
}

esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value)
{
    assert(handle && live_handles && !strcmp(key, "gpios"));
    ++set_calls;
    if (set_error) return set_error;
    pending = value;
    dirty = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle && live_handles);
    ++commit_calls;
    assert(!pthread_mutex_lock(&flash_lock));
    if (block_commit) {
        commit_entered = true;
        assert(!pthread_cond_broadcast(&flash_changed));
        while (!release_commit) {
            assert(!pthread_cond_wait(&flash_changed, &flash_lock));
        }
    }
    assert(!pthread_mutex_unlock(&flash_lock));
    if (commit_error) return commit_error;
    if (dirty) {
        stored = pending;
        present = true;
        dirty = invalid_type = false;
    }
    if (storage_path) {
        FILE *file = fopen(storage_path, "wb");
        assert(file && fwrite(&stored, sizeof(stored), 1, file) == 1);
        assert(!fclose(file));
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle && live_handles);
    --live_handles;
}

static void reset(void)
{
    if (writer_lock) {
        vSemaphoreDelete(writer_lock);
        writer_lock = NULL;
    }
    assert(!live_handles && !live_mutexes);
    publish_settings(LIGHTING_GPIO_DEFAULT_MASK);
    stored = pending = 0;
    present = dirty = invalid_type = false;
    open_error = get_error = set_error = commit_error = 0;
    fail_mutex_create = fail_mutex_take = false;
    open_calls = set_calls = commit_calls = next_handle = 0;
    block_commit = commit_entered = release_commit = reader_finished = false;
    storage_path = NULL;
}

static void expect_persisted(uint64_t gpios)
{
    assert(lighting_settings_get() == gpios);
    assert(present && stored == gpios && !live_handles);
}

static void parser_checks(void)
{
    const uint64_t all = lighting_gpio_available_mask();
    assert(all == UINT64_C(0x7f800001fc02));
    assert((all & LIGHTING_GPIO_DEFAULT_MASK) == LIGHTING_GPIO_DEFAULT_MASK);
    assert(lighting_gpio_parse(NULL, &stored) == ESP_ERR_INVALID_ARG);
    assert(lighting_gpio_parse("45,46", NULL) == ESP_ERR_INVALID_ARG);
    for (unsigned pin = 0; pin < 64; ++pin) {
        char text[10];
        snprintf(text, sizeof(text), "%u", pin);
        uint64_t output = UINT64_MAX;
        esp_err_t result = lighting_gpio_parse(text, &output);
        if (all & (UINT64_C(1) << pin)) {
            assert(result == ESP_OK && output == (UINT64_C(1) << pin));
        } else {
            assert(result == ESP_ERR_INVALID_ARG && output == UINT64_MAX);
        }
    }
    const char *invalid[] = {
        "", " ", ",", "45,", ",45", "45,,46", "45,46,45", "45 46",
        "45x", "45.0", "+45", "-1", "0x2d", "1e1", "45;46", "45, 48",
        "49", "64", "999999999999999999999999999999", "45,046,46", "\x80",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        uint64_t output = UINT64_MAX;
        assert(lighting_gpio_parse(invalid[index], &output) == ESP_ERR_INVALID_ARG);
        assert(output == UINT64_MAX);
    }
    uint64_t output = 0;
    assert(lighting_gpio_parse(" \t46 , 045,\n1, 10 \r", &output) == ESP_OK);
    assert(output == (LIGHTING_GPIO_DEFAULT_MASK | FIRST | (UINT64_C(1) << 10)));
    char csv[LIGHTING_GPIO_TEXT_CAPACITY];
    assert(lighting_gpio_format(output, csv, sizeof(csv)) == ESP_OK);
    assert(!strcmp(csv, "1,10,45,46"));
    const uint64_t masks[] = { FIRST, LIGHTING_GPIO_DEFAULT_MASK, SECOND, all };
    for (size_t index = 0; index < sizeof(masks) / sizeof(masks[0]); ++index) {
        assert(lighting_gpio_format(masks[index], csv, sizeof(csv)) == ESP_OK);
        assert(lighting_gpio_parse(csv, &output) == ESP_OK && output == masks[index]);
        size_t needed = strlen(csv) + 1;
        char exact[LIGHTING_GPIO_TEXT_CAPACITY];
        memset(exact, 'x', sizeof(exact));
        assert(lighting_gpio_format(masks[index], exact, needed - 1) == ESP_ERR_INVALID_SIZE);
        for (size_t offset = 0; offset < sizeof(exact); ++offset) assert(exact[offset] == 'x');
        assert(lighting_gpio_format(masks[index], exact, needed) == ESP_OK);
        assert(!strcmp(exact, csv) && exact[needed] == 'x');
    }
    strcpy(csv, "untouched");
    assert(lighting_gpio_format(0, csv, sizeof(csv)) == ESP_ERR_INVALID_ARG);
    assert(lighting_gpio_format(UINT64_MAX, csv, sizeof(csv)) == ESP_ERR_INVALID_ARG);
    assert(lighting_gpio_format(FIRST, NULL, sizeof(csv)) == ESP_ERR_INVALID_ARG);
    assert(!strcmp(csv, "untouched"));
}

static void initialization_checks(void)
{
    reset();
    assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
    assert(lighting_settings_set(FIRST) == ESP_ERR_INVALID_STATE);
    assert(lighting_settings_init() == ESP_OK);
    assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
    assert(!set_calls && !commit_calls && !live_handles && live_mutexes == 1);
    assert(lighting_settings_init() == ESP_OK && open_calls == 1);
    const uint64_t valid[] = { FIRST, SECOND, lighting_gpio_available_mask() };
    for (size_t index = 0; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        reset(); present = true; stored = valid[index];
        assert(lighting_settings_init() == ESP_OK);
        expect_persisted(valid[index]);
        assert(!set_calls && !commit_calls);
    }
    const uint64_t malformed[] = { 0, UINT64_C(1) << 5, UINT64_C(1) << 63,
        LIGHTING_GPIO_DEFAULT_MASK | (UINT64_C(1) << 19), UINT64_MAX };
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); ++index) {
        reset(); present = true; stored = malformed[index];
        assert(lighting_settings_init() == ESP_OK);
        assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
        assert(!set_calls && !commit_calls && !live_handles);
    }
    reset(); present = invalid_type = true;
    assert(lighting_settings_init() == ESP_OK);
    assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset();
        if (failure == 0) fail_mutex_create = true;
        if (failure == 1) open_error = ESP_FAIL;
        if (failure == 2) get_error = ESP_ERR_TIMEOUT;
        const esp_err_t expected = failure == 0 ? ESP_ERR_NO_MEM :
            failure == 1 ? ESP_FAIL : ESP_ERR_TIMEOUT;
        assert(lighting_settings_init() == expected);
        assert(!writer_lock && !live_mutexes && !live_handles);
        assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
        fail_mutex_create = false; open_error = get_error = ESP_OK;
        assert(lighting_settings_init() == ESP_OK);
    }
}

static void failure_checks(void)
{
    reset(); assert(lighting_settings_init() == ESP_OK);
    assert(lighting_settings_set(LIGHTING_GPIO_DEFAULT_MASK) == ESP_OK);
    assert(!set_calls && !commit_calls);
    assert(lighting_settings_set(0) == ESP_ERR_INVALID_ARG);
    assert(lighting_settings_set(UINT64_MAX) == ESP_ERR_INVALID_ARG);
    assert(!set_calls && !commit_calls);
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset(); assert(lighting_settings_init() == ESP_OK);
        if (failure == 0) open_error = ESP_FAIL;
        if (failure == 1) set_error = ESP_FAIL;
        if (failure == 2) commit_error = ESP_FAIL;
        if (failure == 3) fail_mutex_take = true;
        assert(lighting_settings_set(FIRST) == (failure == 3 ? ESP_ERR_TIMEOUT : ESP_FAIL));
        assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK && !live_handles);
        open_error = set_error = commit_error = 0; fail_mutex_take = false;
        /* A successful replacement must overwrite a dirty failed NVS write. */
        assert(lighting_settings_set(SECOND) == ESP_OK);
        expect_persisted(SECOND);
        assert(lighting_settings_set(FIRST) == ESP_OK);
        expect_persisted(FIRST);
    }
}

static void *write_first(void *unused)
{
    (void)unused;
    assert(lighting_settings_set(FIRST) == ESP_OK);
    return NULL;
}

static void *write_second(void *unused)
{
    (void)unused;
    assert(lighting_settings_set(SECOND) == ESP_OK);
    return NULL;
}

static void *read_during_commit(void *unused)
{
    (void)unused;
    for (unsigned index = 0; index < 10000; ++index) {
        assert(lighting_settings_get() == LIGHTING_GPIO_DEFAULT_MASK);
    }
    assert(!pthread_mutex_lock(&flash_lock));
    reader_finished = true;
    assert(!pthread_cond_broadcast(&flash_changed));
    assert(!pthread_mutex_unlock(&flash_lock));
    return NULL;
}

static atomic_bool readers_running;

static void *read_snapshots(void *unused)
{
    (void)unused;
    while (atomic_load(&readers_running)) {
        uint64_t snapshot = lighting_settings_get();
        /* Distinct values span both halves of the 64-bit snapshot. */
        assert(snapshot == LIGHTING_GPIO_DEFAULT_MASK || snapshot == FIRST || snapshot == SECOND);
    }
    return NULL;
}

static void wait_for(bool *flag)
{
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 2;
    assert(!pthread_mutex_lock(&flash_lock));
    while (!*flag) {
        assert(!pthread_cond_timedwait(&flash_changed, &flash_lock, &deadline));
    }
    assert(!pthread_mutex_unlock(&flash_lock));
}

static void concurrent_checks(void)
{
    reset(); assert(lighting_settings_init() == ESP_OK);
    block_commit = true;
    pthread_t first, second, reader, snapshots;
    assert(!pthread_create(&first, NULL, write_first, NULL));
    wait_for(&commit_entered);
    assert(!pthread_create(&second, NULL, write_second, NULL));
    assert(!pthread_create(&reader, NULL, read_during_commit, NULL));
    wait_for(&reader_finished);
    assert(!pthread_join(reader, NULL));
    atomic_store(&readers_running, true);
    assert(!pthread_create(&snapshots, NULL, read_snapshots, NULL));
    assert(!pthread_mutex_lock(&flash_lock));
    release_commit = true;
    assert(!pthread_cond_broadcast(&flash_changed));
    assert(!pthread_mutex_unlock(&flash_lock));
    assert(!pthread_join(first, NULL));
    assert(!pthread_join(second, NULL));
    atomic_store(&readers_running, false);
    assert(!pthread_join(snapshots, NULL));
    expect_persisted(SECOND);
    assert(commit_calls == 2 && set_calls == 2);
}

static void persistence_process(int argc, char **argv)
{
    assert(argc == 4);
    reset();
    uint64_t gpios;
    assert(lighting_gpio_parse(argv[3], &gpios) == ESP_OK);
    if (!strcmp(argv[1], "save")) {
        storage_path = argv[2];
        assert(lighting_settings_init() == ESP_OK);
        assert(lighting_settings_set(gpios == FIRST ? SECOND : FIRST) == ESP_OK);
        assert(lighting_settings_set(gpios) == ESP_OK);
        expect_persisted(gpios);
    } else {
        assert(!strcmp(argv[1], "load"));
        FILE *file = fopen(argv[2], "rb");
        assert(file && fread(&stored, sizeof(stored), 1, file) == 1);
        assert(!fclose(file));
        present = true;
        assert(lighting_settings_init() == ESP_OK);
        expect_persisted(gpios);
        assert(!set_calls && !commit_calls);
    }
    reset();
}

int main(int argc, char **argv)
{
    if (argc != 1) {
        persistence_process(argc, argv);
        return 0;
    }
    parser_checks();
    initialization_checks();
    failure_checks();
    concurrent_checks();
    reset();
    puts("Lighting settings passed: safe GPIO validation, defaults, failure recovery, serialized writes, nonblocking snapshots and persistence.");
    return 0;
}
