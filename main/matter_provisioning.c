#include "matter_provisioning.h"

#include <stddef.h>
#include <string.h>

#include "nvs.h"

#define PROVISIONING_NAMESPACE "matter_prov"
#define PROVISIONING_KEY "identity"
#define PROVISIONING_SCHEMA_VERSION 1u

/* Portable record layout; numeric fields are little-endian, with no padding. */
enum {
    RECORD_MAGIC_OFFSET = 0,
    RECORD_VERSION_OFFSET = 4,
    RECORD_SIZE_OFFSET = 6,
    RECORD_DISCRIMINATOR_OFFSET = 8,
    RECORD_ITERATIONS_OFFSET = 10,
    RECORD_VENDOR_OFFSET = 14,
    RECORD_PRODUCT_OFFSET = 16,
    RECORD_SERIAL_LENGTH_OFFSET = 18,
    RECORD_SERIAL_OFFSET = 19,
    RECORD_SALT_OFFSET = RECORD_SERIAL_OFFSET + MATTER_PROVISIONING_SERIAL_MAX_LENGTH,
    RECORD_VERIFIER_OFFSET = RECORD_SALT_OFFSET + MATTER_PROVISIONING_SALT_LENGTH,
    RECORD_ROTATING_ID_OFFSET = RECORD_VERIFIER_OFFSET + MATTER_PROVISIONING_VERIFIER_LENGTH,
    RECORD_CHECKSUM_OFFSET = RECORD_ROTATING_ID_OFFSET + MATTER_PROVISIONING_ROTATING_ID_LENGTH,
    PROVISIONING_RECORD_SIZE = RECORD_CHECKSUM_OFFSET + 4,
};

static const uint8_t RECORD_MAGIC[] = {'A', 'M', 'P', 'I'};

static void clear_data(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t record_checksum(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool valid_identity(const matter_provisioning_identity_t *identity,
                           size_t *serial_length)
{
    if (!identity || identity->discriminator > 4095 ||
        identity->iterations < 1000 || identity->iterations > 100000) {
        return false;
    }
    size_t length = 0;
    while (length <= MATTER_PROVISIONING_SERIAL_MAX_LENGTH && identity->serial_number[length]) ++length;
    if (!length || length > MATTER_PROVISIONING_SERIAL_MAX_LENGTH) return false;

    /* Match the generator's valid UTF-8 without Unicode Cc controls. */
    const unsigned char *serial = (const unsigned char *)identity->serial_number;
    for (size_t offset = 0; offset < length;) {
        const uint32_t first = serial[offset];
        size_t width;
        uint32_t codepoint;
        if (first <= 0x7f) {
            width = 1;
            codepoint = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            width = 2;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (width > length - offset) return false;
        for (size_t i = 1; i < width; ++i) {
            const uint32_t continuation = serial[offset + i];
            if (continuation < 0x80 || continuation > 0xbf) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((width == 3 && codepoint < 0x800) ||
            (width == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f)) {
            return false;
        }
        offset += width;
    }
    if (serial_length) *serial_length = length;
    return true;
}

static void encode_record(const matter_provisioning_identity_t *identity,
                          size_t serial_length,
                          uint8_t record[PROVISIONING_RECORD_SIZE])
{
    memset(record, 0, PROVISIONING_RECORD_SIZE);
    memcpy(record + RECORD_MAGIC_OFFSET, RECORD_MAGIC, sizeof(RECORD_MAGIC));
    write_u16(record + RECORD_VERSION_OFFSET, PROVISIONING_SCHEMA_VERSION);
    write_u16(record + RECORD_SIZE_OFFSET, PROVISIONING_RECORD_SIZE);
    write_u16(record + RECORD_DISCRIMINATOR_OFFSET, identity->discriminator);
    write_u32(record + RECORD_ITERATIONS_OFFSET, identity->iterations);
    write_u16(record + RECORD_VENDOR_OFFSET, identity->vendor_id);
    write_u16(record + RECORD_PRODUCT_OFFSET, identity->product_id);
    record[RECORD_SERIAL_LENGTH_OFFSET] = (uint8_t)serial_length;
    memcpy(record + RECORD_SERIAL_OFFSET, identity->serial_number, serial_length);
    memcpy(record + RECORD_SALT_OFFSET, identity->salt, sizeof(identity->salt));
    memcpy(record + RECORD_VERIFIER_OFFSET, identity->verifier, sizeof(identity->verifier));
    memcpy(record + RECORD_ROTATING_ID_OFFSET, identity->rotating_id, sizeof(identity->rotating_id));
    write_u32(record + RECORD_CHECKSUM_OFFSET, record_checksum(record, RECORD_CHECKSUM_OFFSET));
}

static bool decode_record(const uint8_t record[PROVISIONING_RECORD_SIZE],
                          matter_provisioning_identity_t *identity)
{
    if (memcmp(record + RECORD_MAGIC_OFFSET, RECORD_MAGIC, sizeof(RECORD_MAGIC)) ||
        read_u16(record + RECORD_VERSION_OFFSET) != PROVISIONING_SCHEMA_VERSION ||
        read_u16(record + RECORD_SIZE_OFFSET) != PROVISIONING_RECORD_SIZE ||
        read_u32(record + RECORD_CHECKSUM_OFFSET) != record_checksum(record, RECORD_CHECKSUM_OFFSET)) {
        return false;
    }
    const size_t serial_length = record[RECORD_SERIAL_LENGTH_OFFSET];
    if (!serial_length || serial_length > MATTER_PROVISIONING_SERIAL_MAX_LENGTH) return false;
    for (size_t i = serial_length; i < MATTER_PROVISIONING_SERIAL_MAX_LENGTH; ++i) {
        if (record[RECORD_SERIAL_OFFSET + i]) return false;
    }

    memset(identity, 0, sizeof(*identity));
    identity->discriminator = read_u16(record + RECORD_DISCRIMINATOR_OFFSET);
    identity->iterations = read_u32(record + RECORD_ITERATIONS_OFFSET);
    identity->vendor_id = read_u16(record + RECORD_VENDOR_OFFSET);
    identity->product_id = read_u16(record + RECORD_PRODUCT_OFFSET);
    memcpy(identity->serial_number, record + RECORD_SERIAL_OFFSET, serial_length);
    memcpy(identity->salt, record + RECORD_SALT_OFFSET, sizeof(identity->salt));
    memcpy(identity->verifier, record + RECORD_VERIFIER_OFFSET, sizeof(identity->verifier));
    memcpy(identity->rotating_id, record + RECORD_ROTATING_ID_OFFSET, sizeof(identity->rotating_id));
    size_t actual_serial_length;
    return valid_identity(identity, &actual_serial_length) && actual_serial_length == serial_length;
}

static esp_err_t read_identity(nvs_handle_t handle, matter_provisioning_identity_t *identity)
{
    uint8_t record[PROVISIONING_RECORD_SIZE] = {0};
    size_t size = sizeof(record);
    esp_err_t result = nvs_get_blob(handle, PROVISIONING_KEY, record, &size);
    matter_provisioning_identity_t loaded = {0};
    if (result == ESP_OK && (size != sizeof(record) || !decode_record(record, &loaded))) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result == ESP_OK) *identity = loaded;
    clear_data(&loaded, sizeof(loaded));
    clear_data(record, sizeof(record));
    return result;
}

static esp_err_t seed_identity(const matter_provisioning_identity_t *seed,
                               bool allow_seed,
                               matter_provisioning_identity_t *identity)
{
    if (!allow_seed) return ESP_ERR_NOT_FOUND;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(PROVISIONING_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    /* A record created after the first read still wins over every seed. */
    matter_provisioning_identity_t loaded = {0};
    result = read_identity(handle, &loaded);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        size_t serial_length;
        if (!valid_identity(seed, &serial_length)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            uint8_t record[PROVISIONING_RECORD_SIZE];
            encode_record(seed, serial_length, record);
            result = nvs_set_blob(handle, PROVISIONING_KEY, record, sizeof(record));
            if (result == ESP_OK) result = nvs_commit(handle);
            if (result == ESP_OK) loaded = *seed;
            clear_data(record, sizeof(record));
        }
    }
    nvs_close(handle);
    if (result == ESP_OK) *identity = loaded;
    clear_data(&loaded, sizeof(loaded));
    return result;
}

esp_err_t matter_provisioning_load(const matter_provisioning_identity_t *seed,
                                 bool allow_seed,
                                 matter_provisioning_identity_t *identity)
{
    if (!identity) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t result = nvs_open(PROVISIONING_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return seed_identity(seed, allow_seed, identity);
    if (result != ESP_OK) return result;

    matter_provisioning_identity_t loaded = {0};
    result = read_identity(handle, &loaded);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        clear_data(&loaded, sizeof(loaded));
        return seed_identity(seed, allow_seed, identity);
    }
    if (result == ESP_OK) *identity = loaded;
    clear_data(&loaded, sizeof(loaded));
    return result;
}
