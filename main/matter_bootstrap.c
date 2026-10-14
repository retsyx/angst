#include "matter_bootstrap.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

esp_err_t matter_bootstrap_check(bool *fresh_install)
{
    if (!fresh_install) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state;
    esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_OK) {
        // The bootloader can mark the first USB-installed image VALID before
        // starting it. VALID therefore still requires the alternate-slot scan.
        if (state != ESP_OTA_IMG_UNDEFINED && state != ESP_OTA_IMG_VALID) {
            *fresh_install = false;
            return ESP_OK;
        }
    } else if (result != ESP_ERR_NOT_FOUND) {
        return result;
    }

    esp_partition_iterator_t iterator = NULL;
    result = esp_partition_find_err(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY,
                                    NULL, &iterator);
    if (result != ESP_OK) {
        return result;
    }
    if (!iterator) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool fresh = true;
    bool found_running = false;
    while (iterator) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (!partition) {
            esp_partition_iterator_release(iterator);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (partition->flash_chip == running->flash_chip && partition->address == running->address) {
            found_running = true;
        } else {
            uint8_t header[sizeof(esp_image_header_t)];
            result = esp_partition_read(partition, 0, header, sizeof(header));
            if (result != ESP_OK) {
                esp_partition_iterator_release(iterator);
                return result;
            }
            for (size_t i = 0; i < sizeof(header); ++i) {
                // Invalid or partially erased images are evidence of prior use, too.
                if (header[i] != UINT8_MAX) {
                    fresh = false;
                    break;
                }
            }
        }
        // Advancing past the final partition releases the iterator.
        iterator = esp_partition_next(iterator);
    }
    if (!found_running) {
        return ESP_ERR_NOT_FOUND;
    }
    *fresh_install = fresh;
    return ESP_OK;
}
