#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "password_mock_idf.h"
#include "../main/upgrade_password.c"

static bool namespace_present, key_present;
static char stored[256], pending[256];
static size_t stored_size;
static int open_error, write_open_error, get_error, set_error, commit_error;
static unsigned open_calls, get_calls, set_calls, commit_calls, close_calls, live_handles;
static unsigned random_calls;
static size_t random_bytes_requested;
static uint8_t entropy[256];

static void reset(void)
{
    namespace_present = key_present = false;
    memset(stored, 0, sizeof(stored));
    memset(pending, 0, sizeof(pending));
    stored_size = 0;
    open_error = write_open_error = get_error = set_error = commit_error = 0;
    open_calls = get_calls = set_calls = commit_calls = close_calls = live_handles = 0;
    random_calls = 0;
    random_bytes_requested = 0;
    for (size_t i = 0; i < sizeof(entropy); ++i) entropy[i] = (uint8_t)i;
}

static void store_bytes(const char *value, size_t size)
{
    assert(size <= sizeof(stored));
    namespace_present = key_present = true;
    memcpy(stored, value, size);
    stored_size = size;
}

static void store_string(const char *value)
{
    store_bytes(value, strlen(value) + 1);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "display_ota"));
    ++open_calls;
    if (open_error) return open_error;
    if (mode == NVS_READONLY && !namespace_present) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) {
        if (write_open_error) return write_open_error;
        namespace_present = true;
    }
    *handle = 7;
    ++live_handles;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 7 && live_handles && !strcmp(key, "password"));
    ++get_calls;
    if (get_error) return get_error;
    if (!key_present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) {
        *size = stored_size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(value, stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 7 && live_handles && !strcmp(key, "password"));
    ++set_calls;
    if (set_error) return set_error;
    assert(strlen(value) < sizeof(pending));
    strcpy(pending, value);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 7 && live_handles);
    ++commit_calls;
    if (commit_error) return commit_error;
    store_string(pending);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 7 && live_handles);
    ++close_calls;
    --live_handles;
}

void esp_fill_random(void *buffer, size_t length)
{
    assert(random_bytes_requested + length <= sizeof(entropy));
    memcpy(buffer, entropy + random_bytes_requested, length);
    random_bytes_requested += length;
    ++random_calls;
}

static void expect_load_error(esp_err_t error)
{
    char output[UPGRADE_PASSWORD_MAX_LENGTH + 1] = "unchanged";
    assert(upgrade_password_load(output, sizeof(output)) == error);
    assert(!strcmp(output, "unchanged"));
    assert(!live_handles);
}

static void expect_generated_password(char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    assert(upgrade_password_load(output, capacity) == ESP_OK);
    assert(strlen(output) == UPGRADE_PASSWORD_GENERATED_LENGTH);
    assert(UPGRADE_PASSWORD_GENERATED_LENGTH == 10);
    for (size_t i = 0; output[i]; ++i) assert(strchr(alphabet, output[i]));
    assert(random_calls == 1 && random_bytes_requested == UPGRADE_PASSWORD_GENERATED_LENGTH);
    assert(set_calls == 1 && commit_calls == 1 && !live_handles);
    assert(key_present && !strcmp(stored, output));
}

static void expect_no_generation(void)
{
    assert(!random_calls && !random_bytes_requested && !set_calls && !commit_calls);
}

int main(void)
{
    char output[UPGRADE_PASSWORD_MAX_LENGTH + 1] = "unchanged";

    reset();
    assert(upgrade_password_load(NULL, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_load(output, 0) == ESP_ERR_INVALID_ARG);
    assert(!open_calls && !strcmp(output, "unchanged"));
    expect_no_generation();

    /* Boot initialization creates the password once, and propagates failures. */
    reset();
    assert(upgrade_password_init() == ESP_OK);
    assert(key_present && strlen(stored) == UPGRADE_PASSWORD_GENERATED_LENGTH);
    assert(random_calls == 1 && set_calls == 1 && commit_calls == 1 && !live_handles);
    assert(upgrade_password_init() == ESP_OK);
    assert(random_calls == 1 && set_calls == 1 && commit_calls == 1 && !live_handles);
    reset(); open_error = ESP_ERR_NVS_NOT_INITIALIZED;
    assert(upgrade_password_init() == ESP_ERR_NVS_NOT_INITIALIZED);
    expect_no_generation();
    reset(); commit_error = ESP_FAIL;
    assert(upgrade_password_init() == ESP_FAIL);
    assert(random_calls == 1 && !key_present && !live_handles);

    /* Both missing storage cases create and commit a password before returning it. */
    reset();
    expect_generated_password(output, sizeof(output));
    assert(!strcmp(output, "ABCDEFGHJK"));
    assert(open_calls == 2 && get_calls == 0 && close_calls == 1);
    char first_password[UPGRADE_PASSWORD_GENERATED_LENGTH + 1];
    strcpy(first_password, output);
    memset(entropy, 255, sizeof(entropy));
    assert(upgrade_password_load(output, sizeof(output)) == ESP_OK);
    assert(!strcmp(output, first_password));
    assert(random_calls == 1 && set_calls == 1 && commit_calls == 1 && !live_handles);

    reset(); namespace_present = true;
    expect_generated_password(output, UPGRADE_PASSWORD_GENERATED_LENGTH + 1);
    assert(open_calls == 2 && get_calls == 1 && close_calls == 2);

    /* Exercise the full byte range, including values above the alphabet length. */
    const uint8_t mixed_entropy[] = {0, 1, 31, 32, 63, 64, 127, 128, 254, 255};
    reset();
    memcpy(entropy, mixed_entropy, sizeof(mixed_entropy));
    expect_generated_password(output, sizeof(output));
    assert(!strcmp(output, "AB9A9A9A89"));
    assert(strcmp(output, first_password));
    reset();
    memset(entropy, 255, sizeof(entropy));
    expect_generated_password(output, sizeof(output));
    assert(!strcmp(output, "9999999999"));

    /* An undersized output must not create a password that cannot be returned. */
    for (unsigned existing_namespace = 0; existing_namespace < 2; ++existing_namespace) {
        reset(); namespace_present = existing_namespace;
        strcpy(output, "unchanged");
        assert(upgrade_password_load(output, UPGRADE_PASSWORD_GENERATED_LENGTH) == ESP_ERR_INVALID_SIZE);
        assert(!strcmp(output, "unchanged") && !key_present && !live_handles);
        expect_no_generation();
        assert(open_calls == 1 && get_calls == existing_namespace);
    }

    /* Persisted custom passwords remain authoritative, with no entropy consumed. */
    reset(); store_string("stored-password");
    assert(upgrade_password_load(output, sizeof(output)) == ESP_OK);
    assert(!strcmp(output, "stored-password") && close_calls == 1);
    expect_no_generation();
    strcpy(output, "unchanged");
    assert(upgrade_password_load(output, 4) == ESP_ERR_INVALID_SIZE);
    assert(!strcmp(output, "unchanged") && !live_handles);
    expect_no_generation();

    reset(); open_error = ESP_ERR_NVS_NOT_INITIALIZED;
    expect_load_error(ESP_ERR_NVS_NOT_INITIALIZED);
    assert(get_calls == 0 && close_calls == 0);
    expect_no_generation();
    reset(); namespace_present = true; get_error = ESP_ERR_NVS_TYPE_MISMATCH;
    expect_load_error(ESP_ERR_NVS_TYPE_MISMATCH);
    assert(close_calls == 1);
    expect_no_generation();
    reset(); namespace_present = true; get_error = ESP_FAIL;
    expect_load_error(ESP_FAIL);
    assert(close_calls == 1);
    expect_no_generation();

    /* Generation failures never expose a password that was not persisted. */
    for (unsigned existing_namespace = 0; existing_namespace < 2; ++existing_namespace) {
        reset(); namespace_present = existing_namespace; write_open_error = ESP_FAIL;
        expect_load_error(ESP_FAIL);
        assert(random_calls == 1 && !set_calls && !commit_calls && !key_present);
        assert(close_calls == existing_namespace);
        reset(); namespace_present = existing_namespace; set_error = ESP_FAIL;
        expect_load_error(ESP_FAIL);
        assert(random_calls == 1 && set_calls == 1 && !commit_calls && !key_present);
        assert(close_calls == existing_namespace + 1);
        reset(); namespace_present = existing_namespace; commit_error = ESP_FAIL;
        expect_load_error(ESP_FAIL);
        assert(random_calls == 1 && set_calls == 1 && commit_calls == 1 && !key_present);
        assert(close_calls == existing_namespace + 1);
    }

    reset(); store_string(""); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_string("with\nnewline"); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_string("with\177delete"); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_string("with\303\251unicode"); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_string(" leading"); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_string("trailing "); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_bytes("A\0B\0", 4); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_bytes("AB", 2); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    reset(); store_bytes("", 0); expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();
    char too_long[UPGRADE_PASSWORD_MAX_LENGTH + 2];
    memset(too_long, 'A', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    reset(); store_string(too_long); expect_load_error(ESP_ERR_NVS_INVALID_LENGTH); expect_no_generation();
    reset(); store_bytes(too_long, UPGRADE_PASSWORD_MAX_LENGTH + 1);
    expect_load_error(ESP_ERR_INVALID_RESPONSE); expect_no_generation();

    reset();
    assert(upgrade_password_save(NULL) == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save("") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save("bad\tpassword") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save("bad\177password") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save("bad\303\251password") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save(" leading") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save("trailing ") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save(" ") == ESP_ERR_INVALID_ARG);
    assert(upgrade_password_save(too_long) == ESP_ERR_INVALID_ARG);
    assert(!open_calls && !set_calls && !commit_calls);

    reset(); open_error = ESP_FAIL;
    assert(upgrade_password_save("valid") == ESP_FAIL);
    assert(!set_calls && !commit_calls && !close_calls);
    reset(); set_error = ESP_FAIL;
    assert(upgrade_password_save("valid") == ESP_FAIL);
    assert(set_calls == 1 && !commit_calls && close_calls == 1 && !live_handles);
    reset(); commit_error = ESP_FAIL;
    assert(upgrade_password_save("valid") == ESP_FAIL);
    assert(set_calls == 1 && commit_calls == 1 && close_calls == 1 && !live_handles);

    reset();
    assert(upgrade_password_save("new-password") == ESP_OK);
    assert(set_calls == 1 && commit_calls == 1 && close_calls == 1 && !live_handles);
    assert(upgrade_password_load(output, sizeof(output)) == ESP_OK);
    assert(!strcmp(output, "new-password") && !random_calls);

    char maximum[UPGRADE_PASSWORD_MAX_LENGTH + 1];
    memset(maximum, '~', UPGRADE_PASSWORD_MAX_LENGTH);
    maximum[UPGRADE_PASSWORD_MAX_LENGTH] = '\0';
    reset();
    assert(upgrade_password_save(maximum) == ESP_OK);
    assert(upgrade_password_load(output, sizeof(output)) == ESP_OK);
    assert(!strcmp(output, maximum) && !random_calls);

    char printable[97];
    printable[0] = '!';
    for (size_t i = 0; i < 95; ++i) printable[i + 1] = (char)(32 + i);
    printable[96] = '\0';
    reset();
    assert(upgrade_password_save(printable) == ESP_OK);
    assert(upgrade_password_load(output, sizeof(output)) == ESP_OK);
    assert(!strcmp(output, printable) && !random_calls);

    puts("Upgrade password host checks passed: random generation, committed persistence, saved precedence, bounds, corruption, and NVS failures.");
}
