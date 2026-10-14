#include "upgrade_password.h"

#include <stdbool.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#define PASSWORD_NAMESPACE "display_ota"
#define PASSWORD_KEY "password"

static bool valid_password(const char *password, size_t *length)
{
    /* HTTP headers trim surrounding spaces; accept internal spaces only. */
    if (!password || password[0] == ' ') {
        return false;
    }
    for (size_t i = 0; i <= UPGRADE_PASSWORD_MAX_LENGTH; ++i) {
        const unsigned char character = (unsigned char)password[i];
        if (character == '\0') {
            if (length) {
                *length = i;
            }
            return i != 0 && password[i - 1] != ' ';
        }
        if (character < 32 || character > 126) {
            return false;
        }
    }
    return false;
}

static void clear_password(char *password, size_t capacity)
{
    /* Volatile writes ensure cleanup is retained even after the last buffer use. */
    volatile char *cursor = password;
    while (capacity--) {
        *cursor++ = '\0';
    }
}

static esp_err_t generate_password(char *password, size_t capacity)
{
    /* 32 symbols give uniform sampling and omit ambiguous I/O/0/1. */
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    if (capacity <= UPGRADE_PASSWORD_GENERATED_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    char generated[UPGRADE_PASSWORD_GENERATED_LENGTH + 1] = {0};
    esp_fill_random(generated, UPGRADE_PASSWORD_GENERATED_LENGTH);
    for (size_t i = 0; i < UPGRADE_PASSWORD_GENERATED_LENGTH; ++i) {
        generated[i] = alphabet[(unsigned char)generated[i] % (sizeof(alphabet) - 1)];
    }

    const esp_err_t result = upgrade_password_save(generated);
    if (result == ESP_OK) {
        memcpy(password, generated, sizeof(generated));
    }
    clear_password(generated, sizeof(generated));
    return result;
}

esp_err_t upgrade_password_load(char *password, size_t capacity)
{
    if (!password || !capacity) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(PASSWORD_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return generate_password(password, capacity);
    }
    if (result != ESP_OK) {
        return result;
    }

    /* Read into a bounded temporary buffer so errors never alter the caller's value. */
    char stored_password[UPGRADE_PASSWORD_MAX_LENGTH + 1] = {0};
    size_t stored_size = sizeof(stored_password);
    result = nvs_get_str(handle, PASSWORD_KEY, stored_password, &stored_size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        clear_password(stored_password, sizeof(stored_password));
        return generate_password(password, capacity);
    }
    if (result != ESP_OK) {
        /* Storage errors must not replace an existing password. */
        clear_password(stored_password, sizeof(stored_password));
        return result;
    }

    size_t length;
    if (stored_size < 2 || stored_size > sizeof(stored_password) ||
        !valid_password(stored_password, &length) || length + 1 != stored_size) {
        clear_password(stored_password, sizeof(stored_password));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (capacity <= length) {
        clear_password(stored_password, sizeof(stored_password));
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(password, stored_password, length + 1);
    clear_password(stored_password, sizeof(stored_password));
    return ESP_OK;
}

esp_err_t upgrade_password_init(void)
{
    char password[UPGRADE_PASSWORD_MAX_LENGTH + 1] = {0};
    const esp_err_t result = upgrade_password_load(password, sizeof(password));
    clear_password(password, sizeof(password));
    return result;
}

esp_err_t upgrade_password_save(const char *password)
{
    if (!valid_password(password, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(PASSWORD_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, PASSWORD_KEY, password);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}
