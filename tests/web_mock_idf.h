#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef int esp_err_t;
typedef struct lv_obj_t lv_obj_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_OTA_VALIDATE_FAILED 100
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_RESP_USE_STRLEN -1
#define HTTP_GET 1
#define HTTP_POST 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef void *httpd_handle_t;
typedef struct { size_t content_len; } httpd_req_t;
typedef struct { size_t stack_size; unsigned recv_wait_timeout, send_wait_timeout; bool lru_purge_enable; unsigned server_port; unsigned max_uri_handlers; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){ .server_port=80 })
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef struct { size_t size; } esp_partition_t;
typedef uint32_t esp_ota_handle_t;
typedef struct { uint8_t header[24]; } esp_image_header_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, int);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
const char *esp_err_to_name(esp_err_t);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
uint32_t ulTaskNotifyTake(int, uint32_t);
void vTaskDelay(unsigned);
void esp_restart(void);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void xTaskNotifyGive(TaskHandle_t);
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_stop(httpd_handle_t);
