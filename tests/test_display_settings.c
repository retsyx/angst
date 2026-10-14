#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "settings_mock_idf.h"
#include "../main/display_settings.c"

/* Pending writes deliberately survive handle closure, exercising isolation
 * from dirty keys left behind by failed setters and failed commits. */
static uint8_t stored[2], pending[2];
static bool present[2], dirty[2], invalid_type[2];
static int open_error, get_error[2], set_error_key, commit_error;
static bool fail_mutex_create, fail_mutex_take;
static unsigned open_calls, set_calls, commit_calls, live_handles, live_mutexes;
static unsigned next_handle;
static const char *storage_path;
static pthread_mutex_t flash_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flash_changed = PTHREAD_COND_INITIALIZER;
static bool block_commit, commit_entered, release_commit;
static bool reader_finished;

static int key_index(const char *key)
{
    if (!strcmp(key, "animation")) return 0;
    assert(!strcmp(key, "stick_figure"));
    return 1;
}

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
    assert(!strcmp(name, "display_cfg") && mode == NVS_READWRITE);
    ++open_calls;
    if (open_error) return open_error;
    *handle = ++next_handle;
    ++live_handles;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle && live_handles);
    int index = key_index(key);
    if (get_error[index]) return get_error[index];
    if (!present[index]) return ESP_ERR_NVS_NOT_FOUND;
    if (invalid_type[index]) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = stored[index];
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle && live_handles && value <= 1);
    int index = key_index(key);
    ++set_calls;
    if (set_error_key == index) return ESP_FAIL;
    pending[index] = value;
    dirty[index] = true;
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
    for (unsigned index = 0; index < 2; ++index) {
        if (dirty[index]) {
            stored[index] = pending[index];
            present[index] = true;
            dirty[index] = invalid_type[index] = false;
        }
    }
    if (storage_path) {
        FILE *file = fopen(storage_path, "wb");
        assert(file && fwrite(stored, sizeof(stored), 1, file) == 1);
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
    publish_settings((display_settings_state_t){ true, true });
    memset(stored, 0, sizeof(stored));
    memset(pending, 0, sizeof(pending));
    memset(present, 0, sizeof(present));
    memset(dirty, 0, sizeof(dirty));
    memset(invalid_type, 0, sizeof(invalid_type));
    memset(get_error, 0, sizeof(get_error));
    open_error = commit_error = 0;
    set_error_key = -1;
    fail_mutex_create = fail_mutex_take = false;
    open_calls = set_calls = commit_calls = next_handle = 0;
    block_commit = commit_entered = release_commit = reader_finished = false;
    storage_path = NULL;
}

static void expect_state(bool animation, bool figure)
{
    display_settings_state_t snapshot = display_settings_get();
    assert(snapshot.animation_enabled == animation);
    assert(snapshot.stick_figure_enabled == figure);
    assert(display_settings_animation_enabled() == animation);
    assert(display_settings_stick_figure_enabled() == figure);
}

static void expect_persisted(bool animation, bool figure)
{
    expect_state(animation, figure);
    assert(present[0] && present[1]);
    assert(stored[0] == animation && stored[1] == figure);
    assert(!live_handles);
}

static void initialization_checks(void)
{
    reset();
    expect_state(true, true);
    assert(display_settings_set_animation_enabled(false) == ESP_ERR_INVALID_STATE);
    assert(display_settings_set_stick_figure_enabled(false) == ESP_ERR_INVALID_STATE);
    assert(display_settings_init() == ESP_OK);
    expect_state(true, true);
    assert(!set_calls && !commit_calls && !live_handles && live_mutexes == 1);
    assert(display_settings_init() == ESP_OK && open_calls == 1);

    /* An old installation stores only the original master key. */
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        reset(); present[0] = true; stored[0] = enabled;
        assert(display_settings_init() == ESP_OK);
        expect_state(enabled, true);
        assert(!present[1] && !set_calls && !commit_calls);
    }
    for (unsigned missing = 0; missing < 2; ++missing) {
        reset(); present[1 - missing] = true;
        assert(display_settings_init() == ESP_OK);
        expect_state(missing == 0, missing == 1);
    }
    for (unsigned corrupt = 0; corrupt < 2; ++corrupt) {
        for (unsigned index = 0; index < 2; ++index) {
            reset(); present[0] = present[1] = true;
            stored[index] = 7;
            invalid_type[index] = corrupt != 0;
            assert(display_settings_init() == ESP_OK);
            expect_state(index == 0, index == 1);
            assert(!live_handles);
        }
    }
    for (unsigned index = 0; index < 2; ++index) {
        reset(); present[0] = present[1] = true;
        get_error[index] = ESP_FAIL;
        assert(display_settings_init() == ESP_FAIL);
        expect_state(true, true);
        assert(!writer_lock && !live_mutexes && !live_handles);
        get_error[index] = ESP_OK;
        assert(display_settings_init() == ESP_OK);
    }
    reset(); open_error = ESP_FAIL;
    assert(display_settings_init() == ESP_FAIL);
    assert(!writer_lock && !live_mutexes && !live_handles);
    open_error = ESP_OK;
    assert(display_settings_init() == ESP_OK);
    reset(); fail_mutex_create = true;
    assert(display_settings_init() == ESP_ERR_NO_MEM);
    assert(!writer_lock && !live_mutexes && !open_calls);
    fail_mutex_create = false;
    assert(display_settings_init() == ESP_OK);
}

static void independence_checks(void)
{
    for (unsigned animation = 0; animation < 2; ++animation) {
        for (unsigned figure = 0; figure < 2; ++figure) {
            reset();
            present[0] = present[1] = true;
            stored[0] = animation; stored[1] = figure;
            assert(display_settings_init() == ESP_OK);
            expect_state(animation, figure);
            assert(display_settings_set_animation_enabled(animation) == ESP_OK);
            assert(display_settings_set_stick_figure_enabled(figure) == ESP_OK);
            assert(!set_calls && !commit_calls);
            assert(display_settings_set_animation_enabled(!animation) == ESP_OK);
            expect_persisted(!animation, figure);
            assert(display_settings_set_stick_figure_enabled(!figure) == ESP_OK);
            expect_persisted(!animation, !figure);
            assert(display_settings_set_animation_enabled(animation) == ESP_OK);
            expect_persisted(animation, !figure);
        }
    }
}

static void failure_checks(void)
{
    for (unsigned figure = 0; figure < 2; ++figure) {
        for (unsigned failure = 0; failure < 4; ++failure) {
            reset(); assert(display_settings_init() == ESP_OK);
            if (failure == 0) open_error = ESP_FAIL;
            if (failure == 1) set_error_key = 0;
            if (failure == 2) set_error_key = 1;
            if (failure == 3) commit_error = ESP_FAIL;
            esp_err_t result = figure
                ? display_settings_set_stick_figure_enabled(false)
                : display_settings_set_animation_enabled(false);
            assert(result == ESP_FAIL);
            expect_state(true, true);
            assert(!live_handles);
            open_error = commit_error = 0; set_error_key = -1;
            /* Commit the other setting before retrying: a dirty failed write
             * must not change the persisted value of the first setting. */
            result = figure
                ? display_settings_set_animation_enabled(false)
                : display_settings_set_stick_figure_enabled(false);
            assert(result == ESP_OK);
            expect_persisted(figure == 0, figure != 0);
            result = figure
                ? display_settings_set_stick_figure_enabled(false)
                : display_settings_set_animation_enabled(false);
            assert(result == ESP_OK);
            expect_persisted(false, false);
        }
    }
    reset(); assert(display_settings_init() == ESP_OK);
    fail_mutex_take = true;
    assert(display_settings_set_animation_enabled(false) == ESP_ERR_TIMEOUT);
    expect_state(true, true);
    assert(!set_calls && !commit_calls);
    fail_mutex_take = false;
    assert(display_settings_set_animation_enabled(false) == ESP_OK);
    expect_persisted(false, true);
}

static void *write_master(void *unused)
{
    (void)unused;
    assert(display_settings_set_animation_enabled(false) == ESP_OK);
    return NULL;
}

static void *write_figure(void *unused)
{
    (void)unused;
    assert(display_settings_set_stick_figure_enabled(false) == ESP_OK);
    return NULL;
}

static void *read_during_commit(void *unused)
{
    (void)unused;
    for (unsigned index = 0; index < 10000; ++index) expect_state(true, true);
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
        display_settings_state_t snapshot = display_settings_get();
        /* Writers commit in master-then-figure order; no torn snapshot may
         * show a figure change without the preceding master change. */
        assert(!snapshot.animation_enabled || snapshot.stick_figure_enabled);
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
    reset(); assert(display_settings_init() == ESP_OK);
    block_commit = true;
    pthread_t master, figure, reader, snapshots;
    assert(!pthread_create(&master, NULL, write_master, NULL));
    wait_for(&commit_entered);
    assert(!pthread_create(&figure, NULL, write_figure, NULL));
    assert(!pthread_create(&reader, NULL, read_during_commit, NULL));
    /* The reader must finish while flash and both writers remain blocked. */
    wait_for(&reader_finished);
    assert(!pthread_join(reader, NULL));
    atomic_store(&readers_running, true);
    assert(!pthread_create(&snapshots, NULL, read_snapshots, NULL));
    assert(!pthread_mutex_lock(&flash_lock));
    release_commit = true;
    assert(!pthread_cond_broadcast(&flash_changed));
    assert(!pthread_mutex_unlock(&flash_lock));
    assert(!pthread_join(master, NULL));
    assert(!pthread_join(figure, NULL));
    atomic_store(&readers_running, false);
    assert(!pthread_join(snapshots, NULL));
    expect_persisted(false, false);
    assert(commit_calls == 2 && set_calls == 4);
}

static void persistence_process(int argc, char **argv)
{
    assert(argc == 5);
    bool animation = atoi(argv[3]) != 0;
    bool figure = atoi(argv[4]) != 0;
    reset();
    if (!strcmp(argv[1], "save")) {
        storage_path = argv[2];
        assert(display_settings_init() == ESP_OK);
        /* Change both first so even the all-enabled combination commits. */
        assert(display_settings_set_animation_enabled(false) == ESP_OK);
        assert(display_settings_set_stick_figure_enabled(false) == ESP_OK);
        assert(display_settings_set_animation_enabled(animation) == ESP_OK);
        assert(display_settings_set_stick_figure_enabled(figure) == ESP_OK);
        expect_persisted(animation, figure);
    } else {
        assert(!strcmp(argv[1], "load"));
        FILE *file = fopen(argv[2], "rb");
        assert(file && fread(stored, sizeof(stored), 1, file) == 1);
        assert(!fclose(file));
        present[0] = present[1] = true;
        assert(display_settings_init() == ESP_OK);
        expect_persisted(animation, figure);
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
    initialization_checks();
    independence_checks();
    failure_checks();
    concurrent_checks();
    reset();
    puts("Display settings checks passed: migration, independent preferences, corrupt defaults, failure recovery, serialized writes, and reads during commit.");
    return 0;
}
