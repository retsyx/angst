#include "service.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define DISPLAY_IDLE_BIT BIT1
#define URL_ENABLED_BIT BIT2
#define URL_CHANGED_BIT BIT3
#define HTTP_TIMEOUT_MS 15000
#define TIME_SYNC_TIMEOUT_MS 10000
#define NETWORK_HOSTNAME "angst"
#define NETWORK_INSTANCE "Angst"
/* esp_netif_create_default_wifi_sta(), also used by ESP-Matter. */
#define NETWORK_STA_IF_KEY "WIFI_STA_DEF"

static const char *TAG = "display_service";
static EventGroupHandle_t service_events;
static QueueHandle_t message_queue;
static bool mdns_started;
static SemaphoreHandle_t url_lock;
static nvs_handle_t settings_handle;
/* Both the string and its generation are protected by url_lock. */
static char service_url[SERVICE_URL_MAX_LENGTH + 1];
static uint32_t url_generation;

static bool valid_url(const char *url)
{
    if (!url) return false;
    size_t length = strnlen(url, SERVICE_URL_MAX_LENGTH + 1);
    if (length > SERVICE_URL_MAX_LENGTH) return false;
    if (!length) return true;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= ' ' || c >= 127) return false;
    }
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, length, 0, &parsed) != 0 ||
        !parsed.field_data[UF_HOST].len) return false;
    size_t scheme_length = parsed.field_data[UF_SCHEMA].len;
    const char *scheme = url + parsed.field_data[UF_SCHEMA].off;
    return (scheme_length == 4 && strncasecmp(scheme, "http", 4) == 0) ||
           (scheme_length == 5 && strncasecmp(scheme, "https", 5) == 0);
}

static esp_err_t init_url(void)
{
    url_lock = xSemaphoreCreateMutex();
    if (!url_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open("display_cfg", NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(url_lock);
        url_lock = NULL;
        return err;
    }
    service_url[0] = '\0';
    size_t capacity = sizeof(service_url);
    err = nvs_get_str(settings_handle, "price_url", service_url, &capacity);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH ||
               (err == ESP_OK &&
                (!capacity || capacity > sizeof(service_url) ||
                 strnlen(service_url, capacity) != capacity - 1 ||
                 !valid_url(service_url)))) {
        ESP_LOGW(TAG, "Invalid stored price URL; price requests disabled");
        service_url[0] = '\0';
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(settings_handle);
        vSemaphoreDelete(url_lock);
        url_lock = NULL;
    }
    return err;
}

esp_err_t service_get_url(char *buffer, size_t capacity)
{
    if (!buffer || !capacity) return ESP_ERR_INVALID_ARG;
    if (!url_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(url_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    size_t length = strlen(service_url) + 1;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (length <= capacity) {
        memcpy(buffer, service_url, length);
        err = ESP_OK;
    }
    xSemaphoreGive(url_lock);
    return err;
}

esp_err_t service_set_url(const char *url)
{
    if (!valid_url(url)) return ESP_ERR_INVALID_ARG;
    if (!url_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(url_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (strcmp(service_url, url) != 0) {
        err = nvs_set_str(settings_handle, "price_url", url);
        if (err == ESP_OK) err = nvs_commit(settings_handle);
        if (err == ESP_OK) {
            strcpy(service_url, url);
            ++url_generation;
            if (service_url[0]) xEventGroupSetBits(service_events, URL_ENABLED_BIT);
            else xEventGroupClearBits(service_events, URL_ENABLED_BIT);
            xEventGroupSetBits(service_events, URL_CHANGED_BIT);
        }
    }
    xSemaphoreGive(url_lock);
    return err;
}

typedef struct {
    service_message_t message;
    size_t length;
    bool too_long;
    bool invalid_text;
} response_t;

static esp_err_t start_mdns(void)
{
    if (mdns_started) return ESP_OK;
    /* mDNS is shared with Matter; its initialization is idempotent. */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;

    char hostname[MDNS_NAME_BUF_LEN];
    if (mdns_hostname_get(hostname) != ESP_OK) {
        uint8_t mac[6];
        err = esp_wifi_get_mac(WIFI_IF_STA, mac);
        if (err != ESP_OK) return err;
        snprintf(hostname, sizeof(hostname), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        err = mdns_hostname_set(hostname);
    }
    if (err == ESP_OK) err = mdns_instance_name_set(NETWORK_INSTANCE);
    /* Matter sets the responder hostname to its MAC-derived hostname. A
     * delegated alias preserves angst.local and the HTTP service target. */
    if (err == ESP_OK && !mdns_hostname_exists(NETWORK_HOSTNAME)) {
        err = mdns_delegate_hostname_add(NETWORK_HOSTNAME, NULL);
    }
#if CONFIG_DISPLAY_OTA_ENABLE
    if (err == ESP_OK && !mdns_service_exists_with_instance(NETWORK_INSTANCE,
                                                           "_http", "_tcp", NETWORK_HOSTNAME)) {
        err = mdns_service_add_for_host(NETWORK_INSTANCE, "_http", "_tcp",
                                       NETWORK_HOSTNAME, 80, NULL, 0);
    }
#endif
    /* Never tear down the responder owned jointly with Matter on failure. */
    mdns_started = err == ESP_OK;
    return err;
}

static void update_mdns_addresses(esp_netif_t *station)
{
    if (!mdns_started) return;
#if CONFIG_LWIP_IPV6
    mdns_ip_addr_t addresses[1 + CONFIG_LWIP_IPV6_NUM_ADDRESSES] = {0};
#else
    mdns_ip_addr_t addresses[1] = {0};
#endif
    size_t count = 0;
    if (station) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(station, &info) == ESP_OK && info.ip.addr) {
            addresses[count].addr.type = ESP_IPADDR_TYPE_V4;
            addresses[count++].addr.u_addr.ip4 = info.ip;
        }
#if CONFIG_LWIP_IPV6
        esp_ip6_addr_t ipv6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int ipv6_count = esp_netif_get_all_preferred_ip6(station, ipv6);
        for (int i = 0; i < ipv6_count; ++i) {
            addresses[count].addr.type = ESP_IPADDR_TYPE_V6;
            addresses[count++].addr.u_addr.ip6 = ipv6[i];
        }
#endif
    }
    for (size_t i = 1; i < count; ++i) addresses[i - 1].next = &addresses[i];
    /* The mDNS API copies the address list before returning. */
    esp_err_t err = mdns_delegate_hostname_set_address(NETWORK_HOSTNAME,
                                                       count ? addresses : NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "mDNS address update: %s", esp_err_to_name(err));
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_netif_t *station = esp_netif_get_handle_from_ifkey(NETWORK_STA_IF_KEY);
        /* This listener runs before Matter's listener, before its first DHCP
         * connection. Matter creates the station interface and owns Wi-Fi. */
        esp_err_t err = station ? esp_netif_set_hostname(station, NETWORK_HOSTNAME)
                                : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) ESP_LOGW(TAG, "Station hostname: %s", esp_err_to_name(err));
        err = start_mdns();
        if (err != ESP_OK) ESP_LOGW(TAG, "mDNS initialization: %s", esp_err_to_name(err));
    } else if ((base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED ||
                                       id == WIFI_EVENT_STA_STOP)) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        xEventGroupClearBits(service_events, WIFI_CONNECTED_BIT);
        update_mdns_addresses(NULL);
        ESP_LOGI(TAG, "Wi-Fi unavailable; waiting for Matter connectivity");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        esp_err_t err = start_mdns();
        if (err != ESP_OK) ESP_LOGW(TAG, "mDNS initialization: %s", esp_err_to_name(err));
        update_mdns_addresses(event->esp_netif);
        xEventGroupSetBits(service_events, WIFI_CONNECTED_BIT);
#if CONFIG_DISPLAY_OTA_ENABLE
        ESP_LOGI(TAG, "Angst connected; web interface: http://angst.local/ (http://" IPSTR "/)",
                 IP2STR(&event->ip_info.ip));
#else
        ESP_LOGI(TAG, "Angst connected as angst.local; IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        const ip_event_got_ip6_t *event = data;
        if (event->esp_netif == esp_netif_get_handle_from_ifkey(NETWORK_STA_IF_KEY) &&
            (xEventGroupGetBits(service_events) & WIFI_CONNECTED_BIT)) {
            update_mdns_addresses(event->esp_netif);
        }
    }
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
        esp_http_client_get_status_code(event->client) != 200) {
        return ESP_OK;
    }

    /* ON_DATA delivers both chunked and Content-Length response bodies. */
    size_t length = (size_t)event->data_len;
    if (memchr(event->data, '\0', length)) response->invalid_text = true;
    if (length > sizeof(response->message.text) - 1 - response->length) {
        response->too_long = true;
        return ESP_FAIL;
    }
    memcpy(response->message.text + response->length, event->data, length);
    response->length += length;
    response->message.text[response->length] = '\0';
    return ESP_OK;
}

static void fetch(const char *url, response_t *response)
{
    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(response->message.text, sizeof(response->message.text),
                 "Error %s", esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (response->too_long) {
        snprintf(response->message.text, sizeof(response->message.text),
                 "Error response too long");
    } else if (response->invalid_text) {
        snprintf(response->message.text, sizeof(response->message.text),
                 "Error invalid response");
    } else if (err != ESP_OK) {
        snprintf(response->message.text, sizeof(response->message.text),
                 "Error %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Request failed: %s", esp_err_to_name(err));
    } else if (status != 200) {
        snprintf(response->message.text, sizeof(response->message.text),
                 "Error %d", status);
    }
    ESP_LOGI(TAG, "HTTP status: %d", status);
    esp_http_client_cleanup(client);
}

static void service_task(void *arg)
{
    (void)arg;
    bool sntp_started = false;
    response_t response;
    char url[SERVICE_URL_MAX_LENGTH + 1];
    for (;;) {
        xEventGroupWaitBits(service_events,
                            WIFI_CONNECTED_BIT | DISPLAY_IDLE_BIT | URL_ENABLED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        xSemaphoreTake(url_lock, portMAX_DELAY);
        memcpy(url, service_url, sizeof(url));
        uint32_t generation = url_generation;
        xEventGroupClearBits(service_events, URL_CHANGED_BIT);
        xSemaphoreGive(url_lock);
        if (!url[0]) continue;
        if (!sntp_started) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_err_t err = esp_netif_sntp_init(&config);
            sntp_started = err == ESP_OK;
            if (err != ESP_OK) ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
        }
        /* An actual clock is needed for HTTPS certificate date validation. */
        if (sntp_started && time(NULL) < 1704067200) {
            esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS));
            if (err != ESP_OK) ESP_LOGW(TAG, "Time synchronization: %s", esp_err_to_name(err));
        }

        /* Settings can change while time synchronization is waiting. */
        xSemaphoreTake(url_lock, portMAX_DELAY);
        bool current = generation == url_generation;
        xSemaphoreGive(url_lock);
        if (!current) continue;

        fetch(url, &response);
        /* A response from a replaced or disabled URL must not reach the GUI.
         * Publishing under the settings lock serializes it with URL updates. */
        xSemaphoreTake(url_lock, portMAX_DELAY);
        if (generation == url_generation) {
            xEventGroupClearBits(service_events, DISPLAY_IDLE_BIT);
            xQueueOverwrite(message_queue, &response.message);
        }
        xSemaphoreGive(url_lock);
        /* Match the sketch: interval starts after the previous request ends. */
        xEventGroupWaitBits(service_events, URL_CHANGED_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(CONFIG_DISPLAY_POLL_INTERVAL_SECONDS * 1000));
    }
}

esp_err_t service_start(QueueHandle_t messages)
{
    if (!messages) return ESP_ERR_INVALID_ARG;
    if (service_events) return ESP_ERR_INVALID_STATE;
    esp_err_t err = init_url();
    if (err != ESP_OK) return err;
    message_queue = messages;
    service_events = xEventGroupCreate();
    if (!service_events) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    xEventGroupSetBits(service_events, DISPLAY_IDLE_BIT);
    if (service_url[0]) xEventGroupSetBits(service_events, URL_ENABLED_BIT);

    /* Register before Matter initializes Wi-Fi so its first connection cannot
     * race the DHCP hostname setup or the worker's connectivity listener. */
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) goto unregister_wifi;
    /* Initialize the shared singleton synchronously before Matter can start
     * DNS-SD from its task. Preset netifs are discovered lazily by mDNS. */
    err = mdns_init();
    if (err != ESP_OK) goto unregister_ip;

    if (xTaskCreate(service_task, "display_service", 8192, NULL, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto unregister_ip;
    }
    return ESP_OK;

unregister_ip:
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event);
unregister_wifi:
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
fail:
    if (service_events) vEventGroupDelete(service_events);
    service_events = NULL;
    message_queue = NULL;
    nvs_close(settings_handle);
    vSemaphoreDelete(url_lock);
    url_lock = NULL;
    return err;
}

void service_set_display_idle(bool idle)
{
    if (!service_events) return;
    if (idle) xEventGroupSetBits(service_events, DISPLAY_IDLE_BIT);
    else xEventGroupClearBits(service_events, DISPLAY_IDLE_BIT);
}
