#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "matter_provisioning_mock_idf.h"
#include "../main/matter_provisioning.c"

static bool namespace_present, key_present;
static uint8_t stored[512], pending[512];
static size_t stored_size, pending_size;
static esp_err_t open_error, write_open_error, get_error, write_get_error, set_error, commit_error;
static unsigned readonly_opens, writable_opens, get_calls, set_calls, commit_calls, close_calls;
static unsigned live_handles;
static const matter_provisioning_identity_t *watched_output;
static matter_provisioning_identity_t expected_output;
static bool inject_at_write_open;
static uint8_t raced_record[512];
static size_t raced_size;

static void check_unpublished_output(void)
{
    if (watched_output) assert(!memcmp(watched_output, &expected_output, sizeof(expected_output)));
}

static void reset_calls(void)
{
    open_error = write_open_error = get_error = write_get_error = set_error = commit_error = ESP_OK;
    readonly_opens = writable_opens = get_calls = set_calls = commit_calls = close_calls = 0;
    live_handles = 0;
    watched_output = NULL;
    inject_at_write_open = false;
}

static void reset(void)
{
    reset_calls();
    namespace_present = key_present = false;
    memset(stored, 0, sizeof(stored));
    memset(pending, 0, sizeof(pending));
    stored_size = pending_size = 0;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    check_unpublished_output();
    assert(!strcmp(name, "matter_prov") && !live_handles);
    if (mode == NVS_READONLY) ++readonly_opens;
    else { assert(mode == NVS_READWRITE); ++writable_opens; }
    if (open_error) return open_error;
    if (mode == NVS_READONLY && !namespace_present) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) {
        if (write_open_error) return write_open_error;
        namespace_present = true;
        if (inject_at_write_open) {
            memcpy(stored, raced_record, raced_size);
            stored_size = raced_size;
            key_present = true;
            inject_at_write_open = false;
        }
    }
    *handle = mode == NVS_READONLY ? 7 : 8;
    ++live_handles;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    check_unpublished_output();
    assert((handle == 7 || handle == 8) && live_handles == 1 && !strcmp(key, "identity"));
    ++get_calls;
    if (handle == 8 && write_get_error) return write_get_error;
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

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    check_unpublished_output();
    assert(handle == 8 && live_handles == 1 && !strcmp(key, "identity"));
    assert(!key_present); /* Existing identities must never be rewritten. */
    assert(size <= sizeof(pending));
    ++set_calls;
    if (set_error) return set_error;
    memcpy(pending, value, size);
    pending_size = size;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    check_unpublished_output();
    assert(handle == 8 && live_handles == 1 && pending_size);
    ++commit_calls;
    if (commit_error) return commit_error;
    memcpy(stored, pending, pending_size);
    stored_size = pending_size;
    key_present = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    check_unpublished_output();
    assert((handle == 7 || handle == 8) && live_handles == 1);
    ++close_calls;
    --live_handles;
}

static matter_provisioning_identity_t sample_identity(unsigned variant)
{
    matter_provisioning_identity_t identity = {0};
    identity.discriminator = (uint16_t)(1234 + variant);
    identity.iterations = 10000 + variant;
    identity.vendor_id = (uint16_t)(0xfff1 + variant);
    identity.product_id = (uint16_t)(0x8000 + variant);
    snprintf(identity.serial_number, sizeof(identity.serial_number), "ANGST-IDENTITY-%u", variant);
    for (size_t i = 0; i < sizeof(identity.salt); ++i) identity.salt[i] = (uint8_t)(i + variant);
    for (size_t i = 0; i < sizeof(identity.verifier); ++i) identity.verifier[i] = (uint8_t)(i * 3 + variant);
    for (size_t i = 0; i < sizeof(identity.rotating_id); ++i) identity.rotating_id[i] = (uint8_t)(255 - i - variant);
    return identity;
}

static void assert_identity(const matter_provisioning_identity_t *actual,
                            const matter_provisioning_identity_t *expected)
{
    assert(actual->discriminator == expected->discriminator);
    assert(actual->iterations == expected->iterations);
    assert(actual->vendor_id == expected->vendor_id && actual->product_id == expected->product_id);
    assert(!strcmp(actual->serial_number, expected->serial_number));
    assert(!memcmp(actual->salt, expected->salt, sizeof(actual->salt)));
    assert(!memcmp(actual->verifier, expected->verifier, sizeof(actual->verifier)));
    assert(!memcmp(actual->rotating_id, expected->rotating_id, sizeof(actual->rotating_id)));
}

static void watch(matter_provisioning_identity_t *output)
{
    watched_output = output;
    memcpy(&expected_output, output, sizeof(expected_output));
}

static void expect_error(const matter_provisioning_identity_t *seed, bool allow_seed, esp_err_t error)
{
    matter_provisioning_identity_t output;
    memset(&output, 0xa5, sizeof(output));
    watch(&output);
    assert(matter_provisioning_load(seed, allow_seed, &output) == error);
    check_unpublished_output();
    watched_output = NULL;
    assert(!live_handles);
}

static void provision(const matter_provisioning_identity_t *seed)
{
    matter_provisioning_identity_t output;
    memset(&output, 0xa5, sizeof(output));
    watch(&output);
    assert(matter_provisioning_load(seed, true, &output) == ESP_OK);
    watched_output = NULL;
    assert_identity(&output, seed);
    assert(key_present && stored_size == 200);
    assert(readonly_opens == 1 && writable_opens == 1 && set_calls == 1 && commit_calls == 1);
    assert(!live_handles);
}

static void expect_readonly_identity(const matter_provisioning_identity_t *seed, bool allow_seed,
                                     const matter_provisioning_identity_t *expected)
{
    reset_calls();
    uint8_t before[sizeof(stored)];
    memcpy(before, stored, sizeof(stored));
    matter_provisioning_identity_t output = {0};
    watch(&output);
    assert(matter_provisioning_load(seed, allow_seed, &output) == ESP_OK);
    watched_output = NULL;
    assert_identity(&output, expected);
    assert(readonly_opens == 1 && get_calls == 1 && close_calls == 1);
    assert(!writable_opens && !set_calls && !commit_calls && !live_handles);
    assert(!memcmp(before, stored, sizeof(stored)));
}

static void repair_checksum(void)
{
    write_u32(stored + RECORD_CHECKSUM_OFFSET, record_checksum(stored, RECORD_CHECKSUM_OFFSET));
}

static void test_serial_encodings(const matter_provisioning_identity_t *seed_a,
                                 const matter_provisioning_identity_t *seed_b)
{
    const char *valid_serials[] = {
        "ANGST-\xc3\xa9", "\xc2\xa0", "\xc2\xad", "\xe0\xa0\x80", "\xed\x9f\xbf",
        "\xee\x80\x80", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf",
        "X\xe2\x82\xac\xf0\x9f\x98\x80",
    };
    for (size_t i = 0; i < sizeof(valid_serials) / sizeof(*valid_serials); ++i) {
        matter_provisioning_identity_t identity = *seed_a;
        memset(identity.serial_number, 0, sizeof(identity.serial_number));
        strcpy(identity.serial_number, valid_serials[i]);
        reset(); provision(&identity);
        expect_readonly_identity(seed_b, true, &identity);
    }
    matter_provisioning_identity_t identity = *seed_a;
    memset(identity.serial_number, 'A', 30);
    memcpy(identity.serial_number + 30, "\xc3\xa9", 3);
    reset(); provision(&identity); /* The serial limit counts UTF-8 bytes. */
    expect_readonly_identity(seed_b, true, &identity);

    const char *invalid_serials[] = {
        "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xe0\x9f\xbf",
        "\xf0\x80\x80\x80", "\xf0\x8f\xbf\xbf", "\xed\xa0\x80", "\xed\xbf\xbf",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xf8\x88\x80\x80\x80",
        "\xfc\x84\x80\x80\x80\x80", "\xfe", "\xff", "\x80", "\xbf",
        "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xc2" "A", "\xe2" "A" "\xac",
        "\xf0\x9f" "A" "\x80", "\x1f", "\x7f", "\xc2\x80", "\xc2\x85", "\xc2\x9f",
    };
    for (size_t i = 0; i < sizeof(invalid_serials) / sizeof(*invalid_serials); ++i) {
        identity = *seed_a;
        memset(identity.serial_number, 0, sizeof(identity.serial_number));
        strcpy(identity.serial_number, invalid_serials[i]);
        reset(); expect_error(&identity, true, ESP_ERR_INVALID_ARG);
        assert(!set_calls && !commit_calls && !key_present);
    }

    /* Invalid UTF-8 cannot enter through a record with a repaired checksum. */
    reset(); provision(seed_a);
    uint8_t valid_record[PROVISIONING_RECORD_SIZE];
    memcpy(valid_record, stored, sizeof(valid_record));
    for (size_t i = 0; i < sizeof(invalid_serials) / sizeof(*invalid_serials); ++i) {
        reset_calls(); memcpy(stored, valid_record, sizeof(valid_record));
        const size_t length = strlen(invalid_serials[i]);
        stored[RECORD_SERIAL_LENGTH_OFFSET] = (uint8_t)length;
        memset(stored + RECORD_SERIAL_OFFSET, 0, MATTER_PROVISIONING_SERIAL_MAX_LENGTH);
        memcpy(stored + RECORD_SERIAL_OFFSET, invalid_serials[i], length);
        repair_checksum();
        expect_error(seed_b, true, ESP_ERR_INVALID_RESPONSE);
        assert(!writable_opens && !set_calls && !commit_calls);
    }
}

int main(void)
{
    const matter_provisioning_identity_t seed_a = sample_identity(0);
    const matter_provisioning_identity_t seed_b = sample_identity(1);
    reset();
    assert(matter_provisioning_load(&seed_a, true, NULL) == ESP_ERR_INVALID_ARG);
    assert(!readonly_opens && !writable_opens);

    /* Missing records cannot be created by an OTA/load-only caller. */
    for (unsigned existing_namespace = 0; existing_namespace < 2; ++existing_namespace) {
        reset(); namespace_present = existing_namespace;
        expect_error(&seed_a, false, ESP_ERR_NOT_FOUND);
        assert(readonly_opens == 1 && get_calls == existing_namespace);
        assert(!writable_opens && !set_calls && !commit_calls && !key_present);
        reset(); namespace_present = existing_namespace;
        expect_error(NULL, true, ESP_ERR_INVALID_ARG);
        assert(writable_opens == 1 && !set_calls && !commit_calls && !key_present);
    }

    reset(); provision(&seed_a);
    assert(!memcmp(stored, "AMPI\x01\x00\xc8\x00", 8));
    assert(stored[8] == 0xd2 && stored[9] == 0x04); /* 1234, little-endian */
    assert(stored[10] == 0x10 && stored[11] == 0x27 && !stored[12] && !stored[13]);
    expect_readonly_identity(&seed_b, true, &seed_a);
    expect_readonly_identity(&seed_b, false, &seed_a);
    expect_readonly_identity(NULL, true, &seed_a);
    expect_readonly_identity(NULL, false, &seed_a);
    matter_provisioning_identity_t invalid = seed_b;
    invalid.iterations = 0;
    expect_readonly_identity(&invalid, true, &seed_a);

    reset(); namespace_present = true; provision(&seed_a);
    assert(get_calls == 2 && close_calls == 2);

    /* A record appearing before writable-open wins, even over an invalid seed. */
    raced_size = stored_size;
    memcpy(raced_record, stored, raced_size);
    for (unsigned invalid_seed = 0; invalid_seed < 2; ++invalid_seed) {
        reset(); inject_at_write_open = true;
        matter_provisioning_identity_t output = {0};
        watch(&output);
        assert(matter_provisioning_load(invalid_seed ? NULL : &seed_b, true, &output) == ESP_OK);
        watched_output = NULL;
        assert_identity(&output, &seed_a);
        assert(readonly_opens == 1 && writable_opens == 1 && get_calls == 1 && close_calls == 1);
        assert(!set_calls && !commit_calls && !live_handles);
        assert(stored_size == raced_size && !memcmp(stored, raced_record, raced_size));
    }
    reset(); inject_at_write_open = true; raced_record[0] ^= 1;
    expect_error(&seed_b, true, ESP_ERR_INVALID_RESPONSE);
    assert(!set_calls && !commit_calls && key_present);
    raced_record[0] ^= 1;

    /* Bounds and serial validation apply before writing a seed. */
    for (unsigned invalid_case = 0; invalid_case < 8; ++invalid_case) {
        invalid = seed_a;
        switch (invalid_case) {
        case 0: invalid.discriminator = 4096; break;
        case 1: invalid.iterations = 999; break;
        case 2: invalid.iterations = 100001; break;
        case 3: invalid.serial_number[0] = 0; break;
        case 4: memset(invalid.serial_number, 'A', sizeof(invalid.serial_number)); break;
        case 5: invalid.serial_number[2] = '\n'; break;
        case 6: invalid.serial_number[2] = '\x7f'; break;
        case 7: invalid.serial_number[2] = (char)0x80; break;
        }
        reset(); expect_error(&invalid, true, ESP_ERR_INVALID_ARG);
        assert(writable_opens == 1 && !set_calls && !commit_calls && !key_present);
    }
    for (unsigned boundary = 0; boundary < 2; ++boundary) {
        matter_provisioning_identity_t identity = seed_a;
        identity.discriminator = boundary ? 4095 : 0;
        identity.iterations = boundary ? 100000 : 1000;
        memset(identity.serial_number, '~', MATTER_PROVISIONING_SERIAL_MAX_LENGTH);
        identity.serial_number[boundary ? MATTER_PROVISIONING_SERIAL_MAX_LENGTH : 1] = 0;
        reset(); provision(&identity);
        expect_readonly_identity(&seed_b, true, &identity);
    }
    test_serial_encodings(&seed_a, &seed_b);

    /* Storage failures never seed from a replacement image or publish output. */
    reset(); open_error = ESP_ERR_NVS_NOT_INITIALIZED;
    expect_error(&seed_b, true, ESP_ERR_NVS_NOT_INITIALIZED);
    assert(!writable_opens && !set_calls && !commit_calls);
    const esp_err_t read_errors[] = {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_LENGTH};
    for (size_t i = 0; i < sizeof(read_errors) / sizeof(*read_errors); ++i) {
        reset(); provision(&seed_a); reset_calls(); get_error = read_errors[i];
        expect_error(&seed_b, true, read_errors[i]);
        assert(!writable_opens && !set_calls && !commit_calls && close_calls == 1);
    }
    for (unsigned existing_namespace = 0; existing_namespace < 2; ++existing_namespace) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            reset(); namespace_present = existing_namespace;
            if (failure == 0) write_open_error = ESP_FAIL;
            if (failure == 1) set_error = ESP_FAIL;
            if (failure == 2) commit_error = ESP_FAIL;
            expect_error(&seed_a, true, ESP_FAIL);
            assert(writable_opens == 1 && !key_present);
            assert(set_calls == (failure != 0) && commit_calls == (failure == 2));
        }
    }
    reset(); write_get_error = ESP_FAIL;
    expect_error(&seed_a, true, ESP_FAIL);
    assert(writable_opens == 1 && !set_calls && !commit_calls && !key_present);

    /* Every byte of the portable record is checked, including all credentials. */
    reset(); provision(&seed_a);
    uint8_t valid_record[PROVISIONING_RECORD_SIZE];
    memcpy(valid_record, stored, sizeof(valid_record));
    for (size_t i = 0; i < sizeof(valid_record); ++i) {
        reset_calls(); memcpy(stored, valid_record, sizeof(valid_record)); stored[i] ^= 1;
        expect_error(&seed_b, true, ESP_ERR_INVALID_RESPONSE);
        assert(!writable_opens && !set_calls && !commit_calls);
    }
    /* A valid checksum cannot make an unsupported or malformed schema valid. */
    for (unsigned corrupt_case = 0; corrupt_case < 9; ++corrupt_case) {
        reset_calls(); memcpy(stored, valid_record, sizeof(valid_record));
        switch (corrupt_case) {
        case 0: stored[RECORD_VERSION_OFFSET] = 2; break;
        case 1: stored[RECORD_SIZE_OFFSET] = 199; break;
        case 2: write_u16(stored + RECORD_DISCRIMINATOR_OFFSET, 4096); break;
        case 3: write_u32(stored + RECORD_ITERATIONS_OFFSET, 999); break;
        case 4: stored[RECORD_SERIAL_LENGTH_OFFSET] = 0; break;
        case 5: stored[RECORD_SERIAL_LENGTH_OFFSET] = 33; break;
        case 6: stored[RECORD_SERIAL_OFFSET] = '\n'; break;
        case 7: stored[RECORD_SERIAL_OFFSET + 1] = 0; break;
        case 8: stored[RECORD_SERIAL_OFFSET + 31] = 'X'; break;
        }
        repair_checksum();
        expect_error(&seed_b, true, ESP_ERR_INVALID_RESPONSE);
        assert(!writable_opens && !set_calls && !commit_calls);
    }
    const size_t invalid_sizes[] = {0, 1, PROVISIONING_RECORD_SIZE - 1, PROVISIONING_RECORD_SIZE + 1};
    for (size_t i = 0; i < sizeof(invalid_sizes) / sizeof(*invalid_sizes); ++i) {
        reset_calls(); memcpy(stored, valid_record, sizeof(valid_record)); stored_size = invalid_sizes[i];
        expect_error(&seed_b, true, stored_size > PROVISIONING_RECORD_SIZE ? ESP_ERR_NVS_INVALID_LENGTH : ESP_ERR_INVALID_RESPONSE);
        assert(!writable_opens && !set_calls && !commit_calls);
    }

    puts("Matter provisioning checks passed: immutable read-only identity, portable record validation, denied OTA seeding, commit-before-use, bounds, corruption, and storage failures.");
    return 0;
}
