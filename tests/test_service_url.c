#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_mock_idf.h"
#include "service.h"

static const char *scenario;
static const char *old_url = "https://prices.example/old?token=one";
static const char *new_url = "http://prices.example/new?token=two";
static bool key_present, mutex_held, allocation_failure;
static char stored[SERVICE_URL_MAX_LENGTH + 64], pending[SERVICE_URL_MAX_LENGTH + 1];
static size_t stored_size;
static int open_error, get_error, set_error, commit_error;
static unsigned set_calls, commit_calls, live_handles, mutex_deletes;
static mock_event_group_t events;
static unsigned change_events, http_calls, published, waits, sntp_calls;
static char fetched_urls[3][SERVICE_URL_MAX_LENGTH + 1];
static service_message_t last_message;
static void (*worker)(void *);
static esp_event_handler_t network_listener;
static jmp_buf worker_end;
static bool running_worker;
static esp_netif_t station;
static char mutex_token, queue_token;

const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";

static bool is_scenario(const char *name) { return !strcmp(scenario, name); }

static void store_bytes(const void *value, size_t size)
{
    assert(size <= sizeof(stored));
    memcpy(stored, value, size);
    stored_size = size;
    key_present = true;
}

static void store_string(const char *value) { store_bytes(value, strlen(value) + 1); }

static void assert_url(const char *expected)
{
    char value[SERVICE_URL_MAX_LENGTH + 1] = "untouched";
    assert(service_get_url(value, sizeof(value)) == ESP_OK);
    assert(!strcmp(value, expected));
}

const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock failure"; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocation_failure ? NULL : &mutex_token; }
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    assert(handle == &mutex_token && timeout == portMAX_DELAY && !mutex_held);
    mutex_held = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle == &mutex_token && mutex_held);
    mutex_held = false;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    assert(handle == &mutex_token && !mutex_held);
    ++mutex_deletes;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "display_cfg") && mode == NVS_READWRITE);
    if (open_error) return open_error;
    *handle = 7;
    ++live_handles;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 7 && live_handles && !strcmp(key, "price_url"));
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
    assert(handle == 7 && live_handles && mutex_held && !strcmp(key, "price_url"));
    ++set_calls;
    if (set_error) return set_error;
    assert(strlen(value) <= SERVICE_URL_MAX_LENGTH);
    strcpy(pending, value);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 7 && live_handles && mutex_held);
    ++commit_calls;
    if (commit_error) return commit_error;
    store_string(pending);
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 7 && live_handles); --live_handles; }

EventGroupHandle_t xEventGroupCreate(void) { events.bits = 0; return &events; }
void vEventGroupDelete(EventGroupHandle_t group) { assert(group == &events); events.bits = 0; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    assert(group == &events);
    if (bits & BIT3) {
        /* Changed sources become visible to the worker only after persistence. */
        assert(mutex_held && commit_calls && !commit_error);
        ++change_events;
    }
    return group->bits |= bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    assert(group == &events);
    EventBits_t previous = group->bits;
    group->bits &= ~bits;
    return previous;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { assert(group == &events); return group->bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              int clear, int all, TickType_t timeout)
{
    assert(group == &events && running_worker && !mutex_held && ++waits < 10);
    if (all) {
        assert(timeout == portMAX_DELAY && !clear);
        assert((bits & (BIT0 | BIT1)) == (BIT0 | BIT1));
        /* A disabled source blocks the worker without HTTP or clock work. */
        if ((group->bits & bits) != bits) longjmp(worker_end, 1);
    } else {
        assert(bits == BIT3 && clear && timeout == 30000);
        if (is_scenario("poll-wakeup") && http_calls == 1) {
            assert(service_set_url(new_url) == ESP_OK);
            assert(group->bits & bits);
        } else if (is_scenario("worker") || http_calls == 2) {
            longjmp(worker_end, 1);
        }
        assert(group->bits & bits);
    }
    EventBits_t result = group->bits;
    if (clear) group->bits &= ~bits;
    return result;
}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    assert(!arg && !worker);
    worker = entry;
    if (handle) *handle = &queue_token;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(!"Polling must remain interruptible by URL updates"); }
int xQueueOverwrite(QueueHandle_t queue, const void *message)
{
    assert(queue == &queue_token);
    last_message = *(const service_message_t *)message;
    ++published;
    /* Simulate the GUI consuming the response before the next poll. */
    service_set_display_idle(true);
    return pdTRUE;
}

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
                                      esp_event_handler_t callback, void *arg)
{
    assert((base == WIFI_EVENT || base == IP_EVENT) && id == ESP_EVENT_ANY_ID && !arg);
    network_listener = callback;
    return ESP_OK;
}
esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id, esp_event_handler_t callback)
{
    (void)base; (void)id; assert(callback == network_listener); return ESP_OK;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { assert(!strcmp(key, "WIFI_STA_DEF")); return &station; }
esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *host) { assert(netif == &station && !strcmp(host, "angst")); return ESP_OK; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info) { assert(netif == &station); info->ip.addr = 1; return ESP_OK; }
esp_err_t esp_wifi_get_mac(int iface, uint8_t *mac) { assert(iface == WIFI_IF_STA); memset(mac, 0x11, 6); return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { assert(config->server); ++sntp_calls; return ESP_OK; }
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout) { assert(timeout == 10000); return ESP_OK; }
esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_hostname_get(char *host) { strcpy(host, "device"); return ESP_OK; }
esp_err_t mdns_hostname_set(const char *host) { assert(host); return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *name) { assert(name); return ESP_OK; }
bool mdns_hostname_exists(const char *host) { assert(host); return true; }
esp_err_t mdns_delegate_hostname_add(const char *host, const mdns_ip_addr_t *addr) { assert(host); (void)addr; return ESP_OK; }
bool mdns_service_exists_with_instance(const char *name, const char *service, const char *protocol, const char *host)
{ assert(name && service && protocol && host); return true; }
esp_err_t mdns_service_add_for_host(const char *name, const char *service, const char *protocol,
                                   const char *host, int port, const void *txt, size_t count)
{ assert(name && service && protocol && host && port == 80 && !txt && !count); return ESP_OK; }
esp_err_t mdns_delegate_hostname_set_address(const char *host, const mdns_ip_addr_t *addr) { assert(host); (void)addr; return ESP_OK; }

struct mock_http_client { esp_http_client_config_t config; };
static struct mock_http_client client;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(!mutex_held && config->url && config->url[0] && http_calls < 3);
    assert(strcmp(config->url, CONFIG_DISPLAY_SERVICE_URL));
    assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
    strcpy(fetched_urls[http_calls], config->url);
    ++http_calls;
    client.config = *config;
    return &client;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t handle)
{
    assert(handle == &client && !mutex_held);
    if (http_calls == 1 && is_scenario("changed-fetch")) assert(service_set_url(new_url) == ESP_OK);
    if (http_calls == 1 && is_scenario("disable-fetch")) assert(service_set_url("") == ESP_OK);
    char *body = fetched_urls[http_calls - 1];
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA, .client = handle,
        .user_data = client.config.user_data, .data = body, .data_len = (int)strlen(body),
    };
    return client.config.event_handler(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t handle) { assert(handle == &client); return 200; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle) { assert(handle == &client); return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }

static void run_worker(void)
{
    assert(worker && network_listener);
    ip_event_got_ip_t event = {.esp_netif = &station, .ip_info.ip.addr = 1};
    network_listener(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
    running_worker = true;
    if (!setjmp(worker_end)) worker(NULL);
    running_worker = false;
    assert(!mutex_held);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    if (is_scenario("restore") || is_scenario("save-failure") || is_scenario("commit-failure") ||
        is_scenario("worker") || is_scenario("changed-fetch") || is_scenario("disable-fetch") ||
        is_scenario("poll-wakeup")) store_string(old_url);
    if (is_scenario("invalid-stored")) store_string("ftp://prices.example/file");
    if (is_scenario("oversized-stored")) {
        memset(stored, 'a', sizeof(stored)); stored[sizeof(stored) - 1] = '\0';
        stored_size = sizeof(stored); key_present = true;
    }
    if (is_scenario("corrupt-stored")) {
        const char corrupt[] = "https://prices.example/good\0extra";
        store_bytes(corrupt, sizeof(corrupt));
    }
    if (is_scenario("load-failure")) get_error = ESP_ERR_NVS_TYPE_MISMATCH;
    if (is_scenario("open-failure")) open_error = ESP_FAIL;
    if (is_scenario("allocation-failure")) allocation_failure = true;

    char output[SERVICE_URL_MAX_LENGTH + 1] = "untouched";
    assert(service_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(service_get_url(output, sizeof(output)) == ESP_ERR_INVALID_STATE);
    assert(service_set_url(old_url) == ESP_ERR_INVALID_STATE);
    assert(service_get_url(NULL, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(service_get_url(output, 0) == ESP_ERR_INVALID_ARG);
    esp_err_t result = service_start(&queue_token);
    if (is_scenario("load-failure") || is_scenario("open-failure") || is_scenario("allocation-failure")) {
        assert(result == (get_error ? get_error : open_error ? open_error : ESP_ERR_NO_MEM));
        assert(!worker && !live_handles && !mutex_held);
        assert(mutex_deletes == (allocation_failure ? 0U : 1U));
        assert(service_get_url(output, sizeof(output)) == ESP_ERR_INVALID_STATE);
        return 0;
    }
    assert(result == ESP_OK && worker && !mutex_held);
    assert(service_start(&queue_token) == ESP_ERR_INVALID_STATE);

    if (is_scenario("restore")) {
        assert_url(old_url);
        strcpy(output, "untouched");
        assert(service_get_url(output, 2) == ESP_ERR_INVALID_SIZE && !strcmp(output, "untouched"));
        assert(service_set_url(old_url) == ESP_OK && !set_calls && !commit_calls && !change_events);
    } else if (is_scenario("validation")) {
        const char *invalid[] = {NULL, "http://", "https:///path", "ftp://prices.example/file", "/relative",
            " https://prices.example/", "https://prices.example/ ", "https://prices.example/\n", "https://prices.example/\177",
            "https://prices.example/\303\251", "http://prices.example:99999/"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
            assert(service_set_url(invalid[i]) == ESP_ERR_INVALID_ARG);
        char maximum[SERVICE_URL_MAX_LENGTH + 2];
        strcpy(maximum, "https://prices.example/");
        memset(maximum + strlen(maximum), 'x', sizeof(maximum) - strlen(maximum) - 1);
        maximum[sizeof(maximum) - 1] = '\0';
        assert(service_set_url(maximum) == ESP_ERR_INVALID_ARG);
        assert(!set_calls && !commit_calls);
        maximum[SERVICE_URL_MAX_LENGTH] = '\0';
        assert(service_set_url(maximum) == ESP_OK); assert_url(maximum);
        const char *valid[] = {"HTTP://prices.example/p?key=one", "https://user:pass@prices.example/p", "https://[::1]:8443/prices", ""};
        for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
            assert(service_set_url(valid[i]) == ESP_OK); assert_url(valid[i]);
        }
    } else if (is_scenario("save-failure") || is_scenario("commit-failure")) {
        if (is_scenario("save-failure")) set_error = ESP_FAIL;
        else commit_error = ESP_FAIL;
        assert(service_set_url(new_url) == ESP_FAIL);
        assert_url(old_url); assert(!strcmp(stored, old_url));
        assert(set_calls == 1 && commit_calls == (set_error ? 0U : 1U) && !change_events);
        set_error = commit_error = 0;
        assert(service_set_url(new_url) == ESP_OK); assert_url(new_url);
    } else if (is_scenario("persist")) {
        assert_url("");
        assert(service_set_url(new_url) == ESP_OK && !strcmp(stored, new_url));
        assert_url(new_url);
        assert(change_events == 1 && (events.bits & BIT2));
        assert(service_set_url("") == ESP_OK && !strcmp(stored, ""));
        assert_url(""); assert(!(events.bits & BIT2) && change_events == 2);
    } else if (is_scenario("worker") || is_scenario("changed-fetch") || is_scenario("disable-fetch") || is_scenario("poll-wakeup")) {
        run_worker();
        assert(!strcmp(fetched_urls[0], old_url) && sntp_calls == 1);
        if (is_scenario("disable-fetch")) {
            assert(http_calls == 1 && !published); assert_url("");
        } else if (is_scenario("worker")) {
            assert(http_calls == 1 && published == 1 && !strcmp(last_message.text, old_url));
        } else {
            assert(http_calls == 2 && !strcmp(fetched_urls[1], new_url));
            assert(published == (is_scenario("changed-fetch") ? 1U : 2U));
            assert(!strcmp(last_message.text, new_url));
        }
    } else {
        assert_url("");
        run_worker();
        assert(!http_calls && !sntp_calls && !published && !(events.bits & BIT2));
    }
    printf("Service URL scenario passed: %s\n", scenario);
    return 0;
}
