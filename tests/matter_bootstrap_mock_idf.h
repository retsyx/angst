#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_RESPONSE 0x108

typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum {
    ESP_PARTITION_SUBTYPE_APP_FACTORY = 0,
    ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
    ESP_PARTITION_SUBTYPE_APP_TEST = 0x20,
    ESP_PARTITION_SUBTYPE_ANY = 0xff,
} esp_partition_subtype_t;
typedef struct {
    void *flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
} esp_partition_t;
typedef struct esp_partition_iterator_opaque_ *esp_partition_iterator_t;
typedef enum {
    ESP_OTA_IMG_NEW = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
    ESP_OTA_IMG_UNDEFINED = 0xffffffffU,
} esp_ota_img_states_t;
typedef struct { uint8_t bytes[24]; } esp_image_header_t;

const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_partition_find_err(esp_partition_type_t, esp_partition_subtype_t,
                                 const char *, esp_partition_iterator_t *);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
