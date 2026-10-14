#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matter_bootstrap_mock_idf.h"
#include "../main/matter_bootstrap.c"

#define PARTITION_COUNT 4

struct esp_partition_iterator_opaque_ { size_t index; };
static esp_partition_t partitions[PARTITION_COUNT];
static uint8_t headers[PARTITION_COUNT][sizeof(esp_image_header_t)];
static const esp_partition_t *running;
static esp_ota_img_states_t ota_state;
static esp_err_t state_error, find_error, read_error;
static size_t partition_count, invalid_get_index, read_error_index;
static bool empty_success;
static unsigned running_calls, state_calls, find_calls, read_calls, release_calls, live_iterators;

static void reset(void)
{
    assert(!live_iterators);
    memset(partitions, 0, sizeof(partitions));
    memset(headers, 0xff, sizeof(headers));
    for (size_t i = 0; i < PARTITION_COUNT; ++i) {
        partitions[i].type = ESP_PARTITION_TYPE_APP;
        partitions[i].address = (uint32_t)(0x20000 + i * 0x600000);
        partitions[i].size = 0x600000;
    }
    partitions[0].subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY;
    partitions[1].subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    partitions[2].subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    partitions[3].subtype = ESP_PARTITION_SUBTYPE_APP_TEST;
    running = &partitions[1];
    headers[1][0] = 0xe9;
    ota_state = ESP_OTA_IMG_UNDEFINED;
    state_error = find_error = read_error = ESP_OK;
    partition_count = PARTITION_COUNT;
    invalid_get_index = read_error_index = PARTITION_COUNT;
    empty_success = false;
    running_calls = state_calls = find_calls = read_calls = release_calls = 0;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    ++running_calls;
    return running;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == running && state);
    ++state_calls;
    if (state_error) return state_error;
    *state = ota_state;
    return ESP_OK;
}

esp_err_t esp_partition_find_err(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                 const char *label, esp_partition_iterator_t *iterator)
{
    assert(type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_ANY && !label && iterator);
    ++find_calls;
    *iterator = NULL;
    if (find_error) return find_error;
    if (!partition_count) return ESP_ERR_NOT_FOUND;
    if (empty_success) return ESP_OK;
    *iterator = calloc(1, sizeof(**iterator));
    assert(*iterator);
    ++live_iterators;
    return ESP_OK;
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    assert(iterator && live_iterators == 1 && iterator->index < partition_count);
    if (iterator->index == invalid_get_index) return NULL;
    return &partitions[iterator->index];
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    assert(iterator && live_iterators == 1);
    if (++iterator->index == partition_count) {
        esp_partition_iterator_release(iterator);
        return NULL;
    }
    return iterator;
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    if (!iterator) return;
    assert(live_iterators == 1);
    ++release_calls;
    --live_iterators;
    free(iterator);
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *buffer, size_t size)
{
    size_t index = (size_t)(partition - partitions);
    assert(index < partition_count && offset == 0 && size == sizeof(esp_image_header_t));
    assert(partition->address != running->address || partition->flash_chip != running->flash_chip);
    ++read_calls;
    if (index == read_error_index) return read_error;
    memcpy(buffer, headers[index], size);
    return ESP_OK;
}

static void expect_error(esp_err_t expected)
{
    for (unsigned initial = 0; initial < 2; ++initial) {
        bool fresh = initial;
        assert(matter_bootstrap_check(&fresh) == expected);
        assert(fresh == (bool)initial && !live_iterators);
    }
}

static void expect_success(bool expected)
{
    bool fresh = !expected;
    assert(matter_bootstrap_check(&fresh) == ESP_OK);
    assert(fresh == expected && !live_iterators);
}

int main(void)
{
    reset();
    assert(matter_bootstrap_check(NULL) == ESP_ERR_INVALID_ARG);
    assert(!running_calls && !state_calls && !find_calls && !read_calls);
    reset(); running = NULL;
    expect_error(ESP_ERR_NOT_FOUND);
    assert(!state_calls && !find_calls && !read_calls);

    reset();
    expect_success(true);
    assert(read_calls == PARTITION_COUNT - 1 && release_calls == 1);
    reset(); state_error = ESP_ERR_NOT_FOUND;
    expect_success(true);
    assert(read_calls == PARTITION_COUNT - 1 && release_calls == 1);
    reset(); ota_state = ESP_OTA_IMG_VALID;
    expect_success(true);
    assert(read_calls == PARTITION_COUNT - 1 && release_calls == 1);
    reset(); partition_count = 1; running = &partitions[0];
    expect_success(true);
    assert(!read_calls && release_calls == 1);

    const esp_ota_img_states_t used_states[] = {
        ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY,
        ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, (esp_ota_img_states_t)123,
    };
    for (size_t i = 0; i < sizeof(used_states) / sizeof(used_states[0]); ++i) {
        reset(); ota_state = used_states[i];
        expect_success(false);
        assert(!find_calls && !read_calls);
    }
    const esp_err_t unexpected_errors[] = {ESP_FAIL, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG};
    for (size_t i = 0; i < sizeof(unexpected_errors) / sizeof(unexpected_errors[0]); ++i) {
        reset(); state_error = unexpected_errors[i];
        expect_error(unexpected_errors[i]);
        assert(!find_calls && !read_calls);
    }

    /* Any programmed byte counts, including an invalid or partially erased image. */
    for (size_t partition = 0; partition < PARTITION_COUNT; ++partition) {
        if (partition == 1) continue;
        for (size_t byte = 0; byte < sizeof(esp_image_header_t); ++byte) {
            reset(); headers[partition][byte] = 0;
            expect_success(false);
            assert(read_calls == PARTITION_COUNT - 1 && release_calls == 1);
        }
    }
    reset(); state_error = ESP_ERR_NOT_FOUND; headers[2][0] = 0xe9;
    expect_success(false);
    for (size_t partition = 0; partition < PARTITION_COUNT; ++partition) {
        if (partition == 1) continue;
        reset(); ota_state = ESP_OTA_IMG_VALID; headers[partition][0] = 0xe9;
        expect_success(false);
        assert(read_calls == PARTITION_COUNT - 1 && release_calls == 1);
    }
    reset(); ota_state = ESP_OTA_IMG_VALID; read_error_index = 2; read_error = ESP_FAIL;
    expect_error(ESP_FAIL);

    reset(); find_error = ESP_ERR_NO_MEM;
    expect_error(ESP_ERR_NO_MEM);
    assert(!read_calls && !release_calls);
    reset(); find_error = ESP_FAIL;
    expect_error(ESP_FAIL);
    reset(); partition_count = 0;
    expect_error(ESP_ERR_NOT_FOUND);
    reset(); empty_success = true;
    expect_error(ESP_ERR_INVALID_RESPONSE);
    reset(); invalid_get_index = 2;
    expect_error(ESP_ERR_INVALID_RESPONSE);
    assert(release_calls == 2);

    for (size_t partition = 0; partition < PARTITION_COUNT; ++partition) {
        if (partition == 1) continue;
        reset(); read_error_index = partition; read_error = ESP_FAIL;
        expect_error(ESP_FAIL);
        assert(release_calls == 2);
    }
    reset(); headers[0][0] = 0; read_error_index = 3; read_error = ESP_ERR_INVALID_SIZE;
    expect_error(ESP_ERR_INVALID_SIZE);

    /* Enumeration must include the running partition, even if APIs return a copy. */
    esp_partition_t running_copy;
    reset(); running_copy = partitions[1]; running = &running_copy;
    expect_success(true);
    reset(); running_copy = partitions[1]; running_copy.address = 0x70000000; running = &running_copy;
    expect_error(ESP_ERR_NOT_FOUND);
    reset(); partitions[0].address = partitions[1].address; partitions[0].flash_chip = &running_copy;
    headers[0][0] = 0;
    expect_success(false);

    puts("Matter bootstrap host checks passed: fresh USB context, OTA lifecycle, alternate image evidence, iterator cleanup, and read errors.");
}
