#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
const char *esp_err_to_name(esp_err_t);

#define CONFIG_DISPLAY_OTA_ENABLE 1
#define CONFIG_LWIP_IPV6 0
#define CONFIG_DISPLAY_POLL_INTERVAL_SECONDS 30
/* Prove that a stale build configuration never becomes the runtime default. */
#define CONFIG_DISPLAY_SERVICE_URL "https://legacy.invalid/never-fetch"

typedef unsigned TickType_t;
typedef unsigned EventBits_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct { EventBits_t bits; } mock_event_group_t;
typedef mock_event_group_t *EventGroupHandle_t;
#define BIT0 1U
#define BIT1 2U
#define BIT2 4U
#define BIT3 8U
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(value) (value)
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, TickType_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xQueueOverwrite(QueueHandle_t, const void *);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(TickType_t);

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);

typedef const char *esp_event_base_t;
extern const char *WIFI_EVENT, *IP_EVENT;
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_STA_STOP 3
#define IP_EVENT_STA_LOST_IP 4
#define IP_EVENT_STA_GOT_IP 5
#define IP_EVENT_GOT_IP6 6
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
esp_err_t esp_event_handler_unregister(esp_event_base_t, int32_t, esp_event_handler_t);

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { unsigned marker; } esp_netif_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { esp_netif_t *esp_netif; } ip_event_got_ip6_t;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
#define WIFI_IF_STA 0
esp_err_t esp_wifi_get_mac(int, uint8_t *);
typedef struct { const char *server; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(value) ((esp_sntp_config_t){ .server = (value) })
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_netif_sntp_sync_wait(TickType_t);

#define MDNS_NAME_BUF_LEN 64
#define ESP_IPADDR_TYPE_V4 0
typedef struct mdns_ip_addr {
    struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } addr;
    struct mdns_ip_addr *next;
} mdns_ip_addr_t;
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_get(char *);
esp_err_t mdns_hostname_set(const char *);
esp_err_t mdns_instance_name_set(const char *);
bool mdns_hostname_exists(const char *);
esp_err_t mdns_delegate_hostname_add(const char *, const mdns_ip_addr_t *);
bool mdns_service_exists_with_instance(const char *, const char *, const char *, const char *);
esp_err_t mdns_service_add_for_host(const char *, const char *, const char *, const char *, int, const void *, size_t);
esp_err_t mdns_delegate_hostname_set_address(const char *, const mdns_ip_addr_t *);

typedef struct mock_http_client *esp_http_client_handle_t;
typedef struct {
    int event_id;
    void *user_data;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
} esp_http_client_event_t;
#define HTTP_EVENT_ON_DATA 1
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms, buffer_size;
    bool disable_auto_redirect;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_crt_bundle_attach(void *);
