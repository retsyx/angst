#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web_mock_idf.h"
#ifndef OTA_SERVER_SOURCE
#define OTA_SERVER_SOURCE "../main/ota_server.c"
#endif
#include OTA_SERVER_SOURCE

static const char TEST_PASSWORD[] = "stored-test-secret";

static char header[256];
static char settings_header_value[32];
static char price_test_header_value[32];
static char animation_test_header_value[32];
static char display_password_header[32];
static bool display_password_header_read_error;
static char content_type_header[128];
static const char *header_read_error_name;
static char response[32768];
static char status_text[128];
static char response_type[64];
static int begin_calls, write_calls, abort_calls, end_calls, boot_calls, notify_calls, delete_calls;
static int identity_prepare_calls;
static esp_err_t identity_prepare_error;
static int begin_error, write_error, end_error, boot_error, send_error, create_result;
static int recv_values[16], recv_count, recv_index, recv_calls;
static unsigned char receive_body[2048];
static size_t receive_body_length, receive_body_offset;
static size_t bytes_written;
static esp_partition_t partition = {4096};
static bool have_partition;
static httpd_config_t server_config;
static int registered;
static bool animation_enabled;
static bool stick_figure_enabled;
static int settings_set_calls, settings_set_error;
static int figure_set_calls, figure_set_error;
static unsigned char receive_byte;
static char persisted_password[129];
static int password_load_calls, password_save_calls;
static esp_err_t password_load_error, password_save_error;
static const char *expected_password_during_save;
static int price_test_calls;
static bool price_test_increase;
static esp_err_t price_test_error;
static char generated_price[64];
static int animation_test_calls;
static animation_id_t requested_animation;
static esp_err_t animation_test_error;
static animation_status_t published_animation;

static char persisted_url[SERVICE_URL_MAX_LENGTH + 1];
static char requested_url[SERVICE_URL_MAX_LENGTH + 1];
static int url_get_calls, url_set_calls;
static esp_err_t url_get_error, url_set_error;
static char shown_password[UPGRADE_PASSWORD_MAX_LENGTH + 1];
static int display_password_calls;
static esp_err_t display_password_error;
static uint64_t persisted_lighting_mask;
static uint64_t parsed_lighting_mask;
static const char *expected_lighting_text;
static int lighting_parse_calls, lighting_set_calls;
static esp_err_t lighting_parse_error, lighting_set_error, lighting_format_error;

static void reset(void)
{
    strcpy(header, TEST_PASSWORD);
    strcpy(settings_header_value, "1");
    strcpy(price_test_header_value, "1");
    strcpy(animation_test_header_value, "1");
    strcpy(display_password_header, "1");
    display_password_header_read_error = false;
    strcpy(content_type_header, "text/plain");
    header_read_error_name = NULL;
    response[0] = status_text[0] = 0;
    response_type[0] = 0;
    begin_calls = write_calls = abort_calls = end_calls = boot_calls = notify_calls = delete_calls = 0;
    identity_prepare_calls = 0;
    identity_prepare_error = ESP_OK;
    begin_error = write_error = end_error = boot_error = send_error = 0;
    create_result = pdPASS;
    recv_count = recv_index = 0;
    recv_calls = 0;
    receive_body_length = receive_body_offset = 0;
    bytes_written = 0;
    have_partition = true;
    animation_enabled = true;
    stick_figure_enabled = true;
    settings_set_calls = settings_set_error = 0;
    figure_set_calls = figure_set_error = 0;
    receive_byte = 0xe9;
    s_restart_pending = false;
    s_server = NULL;
    registered = 0;
    memset(s_password, 0, sizeof(s_password));
    strcpy(s_password, TEST_PASSWORD);
    s_password_length = strlen(s_password);
    strcpy(persisted_password, TEST_PASSWORD);
    password_load_calls = password_save_calls = 0;
    password_load_error = password_save_error = ESP_OK;
    expected_password_during_save = NULL;
    price_test_calls = 0;
    price_test_increase = false;
    price_test_error = ESP_OK;
    strcpy(generated_price, "$4,123.45");
    animation_test_calls = 0;
    requested_animation = ANIMATION_NONE;
    animation_test_error = ESP_OK;
    published_animation = (animation_status_t){.animation_id=ANIMATION_NONE};
    persisted_url[0] = requested_url[0] = 0;
    url_get_calls = url_set_calls = 0;
    url_get_error = url_set_error = ESP_OK;
    shown_password[0] = 0;
    display_password_calls = 0;
    display_password_error = ESP_OK;
    persisted_lighting_mask = LIGHTING_GPIO_DEFAULT_MASK;
    parsed_lighting_mask = LIGHTING_GPIO_DEFAULT_MASK;
    expected_lighting_text = NULL;
    lighting_parse_calls = lighting_set_calls = 0;
    lighting_parse_error = lighting_set_error = lighting_format_error = ESP_OK;
}

static void set_receive_body(const void *body, size_t length)
{
    assert(length <= sizeof(receive_body));
    memcpy(receive_body, body, length);
    receive_body_length = length;
    receive_body_offset = 0;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *a, const char *b) { (void)r; (void)a; (void)b; return 0; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; strcpy(status_text, s); return 0; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { (void)r; strcpy(response_type,s); return 0; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *s, int size) { (void)r; assert(size == -1); strcpy(response,s); return send_error; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *s, size_t n) { (void)r; if(s) { size_t old = strlen(response); assert(old+n<sizeof(response)); memcpy(response+old,s,n); response[old+n]=0; } return send_error; }
static const char *request_header(const char *name)
{
    if (!strcmp(name, "X-OTA-Password")) return header;
    if (!strcmp(name, "X-Display-Settings")) return settings_header_value;
    if (!strcmp(name, "X-Display-Password")) return display_password_header;
    if (!strcmp(name, "X-Price-Test")) return price_test_header_value;
    if (!strcmp(name, "X-Animation-Test")) return animation_test_header_value;
    assert(!strcmp(name, "Content-Type"));
    return content_type_header;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *s) { (void)r; return strlen(request_header(s)); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *s, char *b, size_t size) { (void)r; if (!strcmp(s,"X-Display-Password") && display_password_header_read_error) return ESP_FAIL; if(header_read_error_name && !strcmp(s,header_read_error_name)) return ESP_FAIL; const char *value=request_header(s); assert(strlen(value)<size); strcpy(b, value); return 0; }
const char *esp_err_to_name(esp_err_t value) { (void)value; return "test error"; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { assert(!p); return have_partition ? &partition : NULL; }
esp_err_t matter_display_prepare_ota(void) {
    assert(!begin_calls && !write_calls && !boot_calls && !recv_calls);
    ++identity_prepare_calls;
    return identity_prepare_error;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) { assert(p == &partition); assert(n <= partition.size); assert(identity_prepare_calls == 1 && identity_prepare_error == ESP_OK); ++begin_calls; *h = 7; return begin_error; }
int httpd_req_recv(httpd_req_t *r, char *b, size_t n) {
    (void)r; ++recv_calls;
    int got = recv_index < recv_count ? recv_values[recv_index++] : (int)n;
    assert(got <= (int)n);
    if (got > 0) {
        if (receive_body_length) {
            assert(receive_body_offset + (size_t)got <= receive_body_length);
            memcpy(b, receive_body + receive_body_offset, (size_t)got);
            receive_body_offset += (size_t)got;
        } else memset(b, receive_byte, (size_t)got);
    }
    return got;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *b, size_t n) { (void)b; assert(h==7); ++write_calls; bytes_written+=n; return write_error; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { assert(h==7); ++abort_calls; return 0; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { assert(h==7); ++end_calls; return end_error; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { assert(p==&partition); ++boot_calls; return boot_error; }
uint32_t ulTaskNotifyTake(int a, uint32_t b) { (void)a; (void)b; return 1; }
void vTaskDelay(unsigned value) { assert(value==OTA_REBOOT_DELAY_MS); }
void esp_restart(void) { abort(); }
int xTaskCreate(void (*f)(void *), const char *name, unsigned stack, void *ctx, unsigned priority, TaskHandle_t *h) { assert(f==reboot_task); (void)name; (void)stack; (void)ctx; (void)priority; *h=(void*)8; return create_result; }
void vTaskDelete(TaskHandle_t h) { assert(h==(void*)8); ++delete_calls; }
void xTaskNotifyGive(TaskHandle_t h) { assert(h==(void*)8); ++notify_calls; }
esp_err_t httpd_start(httpd_handle_t *s, const httpd_config_t *c) { *s=(void*)9; server_config=*c; return 0; }
esp_err_t httpd_register_uri_handler(httpd_handle_t s, const httpd_uri_t *u) {
    assert(s==(void*)9); assert(u->handler);
    if (!strcmp(u->uri,"/password")) assert(u->method==HTTP_POST && u->handler==password_post_handler);
    if (!strcmp(u->uri,"/show-password")) assert(u->method==HTTP_POST && u->handler==show_password_post_handler);
    if (!strcmp(u->uri,"/price-url")) {
        assert((u->method==HTTP_GET && u->handler==price_url_get_handler) ||
               (u->method==HTTP_POST && u->handler==price_url_post_handler));
    }
    if (!strcmp(u->uri,"/test-price")) assert(u->method==HTTP_POST && u->handler==price_test_post_handler);
    if (!strcmp(u->uri,"/api/animations")) assert(u->method==HTTP_GET && u->handler==animations_get_handler);
    if (!strcmp(u->uri,"/api/animation")) assert((u->method==HTTP_GET && u->handler==animation_get_handler) || (u->method==HTTP_POST && u->handler==animation_post_handler));
    if (!strcmp(u->uri,"/lighting-gpios")) {
        assert((u->method==HTTP_GET && u->handler==lighting_gpios_get_handler) ||
               (u->method==HTTP_POST && u->handler==lighting_gpios_post_handler));
    }
    ++registered; return 0;
}
esp_err_t httpd_stop(httpd_handle_t s) { (void)s; return 0; }
display_settings_state_t display_settings_get(void) { return (display_settings_state_t){animation_enabled,stick_figure_enabled}; }
bool display_settings_animation_enabled(void) { return animation_enabled; }
bool display_settings_stick_figure_enabled(void) { return stick_figure_enabled; }
esp_err_t display_settings_set_animation_enabled(bool enabled) { ++settings_set_calls; if(settings_set_error) return settings_set_error; animation_enabled=enabled; return ESP_OK; }
esp_err_t display_settings_set_stick_figure_enabled(bool enabled) { ++figure_set_calls; if(figure_set_error) return figure_set_error; stick_figure_enabled=enabled; return ESP_OK; }
esp_err_t upgrade_password_load(char *destination, size_t capacity) {
    ++password_load_calls;
    if (password_load_error != ESP_OK) return password_load_error;
    assert(strlen(persisted_password) < capacity);
    strcpy(destination, persisted_password);
    return ESP_OK;
}
esp_err_t upgrade_password_save(const char *password) {
    ++password_save_calls;
    if (expected_password_during_save) assert(!strcmp(s_password, expected_password_during_save));
    assert(strlen(password) >= 1 && strlen(password) <= 128);
    for (const unsigned char *p=(const unsigned char *)password; *p; ++p) assert(*p >= 32 && *p <= 126);
    if (password_save_error != ESP_OK) return password_save_error;
    strcpy(persisted_password, password);
    return ESP_OK;
}
esp_err_t price_test_request(bool increase, char *destination, size_t capacity) {
    ++price_test_calls;
    price_test_increase = increase;
    assert(capacity == 64 && strlen(generated_price) < capacity);
    if (price_test_error != ESP_OK) return price_test_error;
    strcpy(destination, generated_price);
    return ESP_OK;
}
esp_err_t animation_test_request(animation_id_t id) {
    ++animation_test_calls;
    requested_animation = id;
    return animation_test_error;
}
animation_status_t animation_test_status(void) { return published_animation; }

esp_err_t service_get_url(char *destination, size_t capacity) {
    ++url_get_calls;
    if (url_get_error != ESP_OK) return url_get_error;
    assert(strlen(persisted_url) < capacity);
    strcpy(destination, persisted_url);
    return ESP_OK;
}
esp_err_t service_set_url(const char *url) {
    ++url_set_calls;
    assert(strlen(url) <= SERVICE_URL_MAX_LENGTH);
    strcpy(requested_url, url);
    if (url_set_error != ESP_OK) return url_set_error;
    strcpy(persisted_url, url);
    return ESP_OK;
}
esp_err_t password_display_request(const char *password) {
    ++display_password_calls;
    assert(password == s_password);
    if (display_password_error != ESP_OK) return display_password_error;
    strcpy(shown_password, password);
    return ESP_OK;
}

uint64_t lighting_settings_get(void) { return persisted_lighting_mask; }
uint64_t lighting_gpio_available_mask(void) {
    return LIGHTING_GPIO_DEFAULT_MASK | (UINT64_C(1)<<1) | (UINT64_C(1)<<2) | (UINT64_C(1)<<3);
}
esp_err_t lighting_gpio_parse(const char *text, uint64_t *mask) {
    ++lighting_parse_calls;
    assert(expected_lighting_text && !strcmp(text,expected_lighting_text));
    if (lighting_parse_error) return lighting_parse_error;
    *mask=parsed_lighting_mask;
    return ESP_OK;
}
esp_err_t lighting_settings_set(uint64_t mask) {
    ++lighting_set_calls;
    assert(mask==parsed_lighting_mask);
    if (lighting_set_error) return lighting_set_error;
    persisted_lighting_mask=mask;
    return ESP_OK;
}
esp_err_t lighting_gpio_format(uint64_t mask, char *text, size_t capacity) {
    if (lighting_format_error) return lighting_format_error;
    assert(mask && !(mask & ~lighting_gpio_available_mask()));
    size_t length=0;
    for (unsigned pin=0;pin<=48;++pin) {
        if (!(mask & (UINT64_C(1)<<pin))) continue;
        int added=snprintf(text+length,capacity-length,"%s%u",length?",":"",pin);
        assert(added>0 && (size_t)added<capacity-length);
        length+=(size_t)added;
    }
    return ESP_OK;
}

static void check_status(const char *prefix) { assert(strncmp(status_text,prefix,strlen(prefix))==0); }

static void check_settings(bool master, bool figure)
{
    char expected[96];
    snprintf(expected,sizeof(expected),"{\"animation_enabled\":%s,\"stick_figure_enabled\":%s}",
             master?"true":"false",figure?"true":"false");
    assert(!strcmp(response,expected));
}

static void test_animation_preferences(void)
{
    httpd_req_t request = {0};
    for (unsigned combination=0; combination<4; ++combination) {
        reset();
        animation_enabled=(combination&2)!=0;
        stick_figure_enabled=(combination&1)!=0;
        assert(settings_get_handler(&request)==ESP_OK); check_settings(animation_enabled,stick_figure_enabled);

        /* Each save touches exactly one preference, even while the master is off. */
        const bool initial_master=animation_enabled, initial_figure=stick_figure_enabled;
        request.content_len=8; set_receive_body(initial_figure?"figure=0":"figure=1",8);
        recv_values[0]=2; recv_values[1]=3; recv_values[2]=3; recv_count=3;
        assert(settings_post_handler(&request)==ESP_OK); check_status("200");
        assert(recv_calls==3 && figure_set_calls==1 && settings_set_calls==0);
        assert(animation_enabled==initial_master && stick_figure_enabled!=initial_figure);
        check_settings(initial_master,!initial_figure);
        request.content_len=1; set_receive_body(initial_master?"0":"1",1);
        assert(settings_post_handler(&request)==ESP_OK);
        assert(settings_set_calls==1 && figure_set_calls==1 && animation_enabled!=initial_master);
        check_settings(!initial_master,!initial_figure);

        reset(); animation_enabled=initial_master; stick_figure_enabled=initial_figure;
        char last_id[3], invalid_id[3];
        snprintf(last_id,sizeof(last_id),"%u",(unsigned)ANIMATION_COUNT);
        snprintf(invalid_id,sizeof(invalid_id),"%u",(unsigned)ANIMATION_COUNT+1);
        request.content_len=strlen(last_id); set_receive_body(last_id,request.content_len);
        const esp_err_t result=animation_post_handler(&request);
        if (initial_master && initial_figure) {
            assert(result==ESP_OK && animation_test_calls==1); check_status("200");
        } else {
            assert(result==ESP_FAIL && animation_test_calls==0); check_status("409");
            assert(strstr(response,"Enable Animation and Stick figure animation"));
        }
        /* Invalid requests keep their validation errors when scenes are disabled. */
        set_receive_body(invalid_id,request.content_len);
        assert(animation_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(animations_get_handler(&request)==ESP_OK && strstr(response,"\"id\":33,\"name\":\"Dig for treasure\""));
    }

    const char *bad_values[]={"Figure=1","figure=2","figure= ","figure=x","figUre=1","figure\0001"," 1", "figure=1\n"};
    for (size_t i=0;i<sizeof(bad_values)/sizeof(bad_values[0]);++i) {
        reset(); request.content_len=i==5?8:strlen(bad_values[i]);
        set_receive_body(bad_values[i],request.content_len);
        assert(settings_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(settings_set_calls==0 && figure_set_calls==0 && animation_enabled && stick_figure_enabled);
    }
    for (unsigned failure=0;failure<3;++failure) {
        reset(); request.content_len=8; set_receive_body("figure=0",8);
        recv_values[0]=3; recv_values[1]=failure==0?HTTPD_SOCK_ERR_TIMEOUT:failure==1?0:ESP_FAIL; recv_count=2;
        assert(settings_post_handler(&request)==ESP_FAIL);
        if (failure==0) check_status("408");
        assert(settings_set_calls==0 && figure_set_calls==0 && stick_figure_enabled);
    }
    reset(); request.content_len=8; set_receive_body("figure=0",8); figure_set_error=ESP_FAIL;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("500");
    assert(figure_set_calls==1 && settings_set_calls==0 && stick_figure_enabled);
    reset(); request.content_len=8; set_receive_body("figure=0",8); s_restart_pending=true;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("503");
    assert(recv_calls==0 && figure_set_calls==0);
    reset(); request.content_len=8; set_receive_body("figure=0",8); s_password_length=0;
    assert(settings_post_handler(&request)==ESP_OK && !stick_figure_enabled);
    check_settings(true,false);
    reset(); request.content_len=8; set_receive_body("figure=0",8); send_error=ESP_FAIL;
    assert(settings_post_handler(&request)==ESP_FAIL && !stick_figure_enabled && figure_set_calls==1);
    send_error=ESP_OK; assert(settings_get_handler(&request)==ESP_OK); check_settings(true,false);
}

static void test_animation_status(void)
{
    httpd_req_t request = {0};
    reset();
    assert(animation_get_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(response_type,"application/json"));
    assert(!strcmp(response,"{\"animation_id\":0,\"running\":false,\"pending\":false,\"name\":null}"));
    published_animation.pending=true;
    assert(animation_get_handler(&request)==ESP_OK);
    assert(strstr(response,"\"pending\":true") && strstr(response,"\"running\":false"));
    published_animation.pending=false;
    for (unsigned id=1;id<=ANIMATION_COUNT;++id) {
        published_animation.animation_id=(animation_id_t)id;
        assert(animation_get_handler(&request)==ESP_OK);
        char expected[256];
        snprintf(expected,sizeof(expected),"{\"animation_id\":%u,\"running\":true,\"pending\":false,\"name\":\"%s\"}",
                 id,animation_catalog_lookup((animation_id_t)id)->name);
        assert(!strcmp(response,expected));
    }
    published_animation.animation_id=ANIMATION_NONE;
    assert(animation_get_handler(&request)==ESP_OK && strstr(response,"\"running\":false"));
    /* A committed off switch hides stale GUI state before its next frame. */
    for (unsigned combination=0;combination<3;++combination) {
        animation_enabled=(combination&2)!=0; stick_figure_enabled=(combination&1)!=0;
        published_animation=(animation_status_t){.animation_id=(animation_id_t)1,.pending=true};
        assert(animation_get_handler(&request)==ESP_OK);
        assert(!strcmp(response,"{\"animation_id\":0,\"running\":false,\"pending\":false,\"name\":null}"));
    }
    reset(); s_restart_pending=true;
    assert(animation_get_handler(&request)==ESP_FAIL); check_status("503");
    reset(); send_error=ESP_FAIL; assert(animation_get_handler(&request)==ESP_FAIL);
}

static void test_show_password(void)
{
    httpd_req_t request = {.content_len = 0};
    const char *invalid_headers[] = {"", "0", "11", "wrong"};
    for (size_t i = 0; i < sizeof(invalid_headers) / sizeof(*invalid_headers); ++i) {
        reset(); strcpy(display_password_header, invalid_headers[i]);
        assert(show_password_post_handler(&request) == ESP_FAIL); check_status("400");
        assert(display_password_calls == 0 && recv_calls == 0);
        assert(!strstr(response, TEST_PASSWORD));
    }
    reset(); display_password_header_read_error = true;
    assert(show_password_post_handler(&request) == ESP_FAIL); check_status("400");
    assert(display_password_calls == 0);
    reset(); request.content_len = 1;
    assert(show_password_post_handler(&request) == ESP_FAIL); check_status("400");
    assert(display_password_calls == 0 && recv_calls == 0);
    request.content_len = 0;
    reset(); s_password_length = 0;
    assert(show_password_post_handler(&request) == ESP_FAIL); check_status("403");
    assert(strstr(response, "Password initialization failed."));
    assert(display_password_calls == 0 && !strstr(response, TEST_PASSWORD));
    reset(); s_restart_pending = true;
    assert(show_password_post_handler(&request) == ESP_FAIL); check_status("503");
    assert(display_password_calls == 0);
    const esp_err_t display_errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL};
    for (size_t i = 0; i < sizeof(display_errors) / sizeof(*display_errors); ++i) {
        reset(); header[0] = 0; display_password_error = display_errors[i];
        assert(show_password_post_handler(&request) == ESP_FAIL);
        check_status(display_errors[i] == ESP_ERR_INVALID_STATE ? "503" : "500");
        assert(display_password_calls == 1 && shown_password[0] == 0);
        assert(!strstr(response, TEST_PASSWORD) && recv_calls == 0);
    }
    /* Physical viewing requires no knowledge of the password, and HTTP never returns it. */
    reset(); header[0] = 0;
    assert(show_password_post_handler(&request) == ESP_OK); check_status("200");
    assert(display_password_calls == 1 && !strcmp(shown_password, TEST_PASSWORD));
    assert(!strstr(response, TEST_PASSWORD) && recv_calls == 0);
    assert(password_save_calls == 0 && boot_calls == 0);
    /* A saved replacement is revealed immediately; the previous password is not used. */
    strcpy(s_password, "current-display-secret"); s_password_length = strlen(s_password);
    assert(show_password_post_handler(&request) == ESP_OK);
    assert(!strcmp(shown_password, "current-display-secret"));
    assert(!strstr(response, "current-display-secret") && !strstr(response, TEST_PASSWORD));
    response[0] = 0;
    assert(page_handler(&request) == ESP_OK);
    assert(!strstr(response, "current-display-secret") && !strstr(response, TEST_PASSWORD));
    reset(); send_error = ESP_FAIL;
    assert(show_password_post_handler(&request) == ESP_FAIL);
    assert(display_password_calls == 1 && !strstr(response, TEST_PASSWORD));
}

static void test_lighting_gpios(void)
{
    httpd_req_t request = {.content_len=0};
    reset(); s_password_length=0; header[0]=0;
    assert(lighting_gpios_get_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(response,"{\"gpios\":[45,46],\"available_gpios\":[1,2,3,45,46]}"));
    assert(!strcmp(response_type,"application/json") && !recv_calls && !lighting_set_calls);
    const struct { const char *text; uint64_t mask; const char *pins; } valid[] = {
        {"45",UINT64_C(1)<<45,"45"},
        {"46,45",LIGHTING_GPIO_DEFAULT_MASK,"45,46"},
        {" 46, 3 ,45 ",LIGHTING_GPIO_DEFAULT_MASK|(UINT64_C(1)<<3),"3,45,46"},
        {"3,2,1,46,45",lighting_gpio_available_mask(),"1,2,3,45,46"},
    };
    for (size_t i=0;i<sizeof(valid)/sizeof(*valid);++i) {
        reset(); settings_header_value[0]=0;
        request.content_len=strlen(valid[i].text); set_receive_body(valid[i].text,request.content_len);
        expected_lighting_text=valid[i].text; parsed_lighting_mask=valid[i].mask;
        if (i==2) {recv_values[0]=2;recv_values[1]=3;recv_count=2;}
        assert(lighting_gpios_post_handler(&request)==ESP_OK); check_status("200");
        assert(lighting_parse_calls==1 && lighting_set_calls==1 && persisted_lighting_mask==valid[i].mask);
        char expected[256]; snprintf(expected,sizeof(expected),"{\"gpios\":[%s],\"available_gpios\":[1,2,3,45,46]}",valid[i].pins);
        assert(!strcmp(response,expected) && !strstr(response,TEST_PASSWORD));
    }
    /* Authentication precedes all body reads, GPIO parsing and persistence. */
    reset(); s_password_length=0; request.content_len=LIGHTING_GPIO_TEXT_CAPACITY;
    strcpy(content_type_header,"application/json");
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("403");
    assert(strstr(response,"Password initialization failed."));
    assert(!recv_calls && !lighting_parse_calls && !lighting_set_calls);
    const char *bad_passwords[]={"","wrong-secret","stored-test-secreu","\tstored-test-secret","stored-test-secret\n"};
    for (size_t i=0;i<sizeof(bad_passwords)/sizeof(*bad_passwords);++i) {
        reset();request.content_len=LIGHTING_GPIO_TEXT_CAPACITY;strcpy(header,bad_passwords[i]);
        strcpy(content_type_header,"application/json");
        assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("401");
        assert(!recv_calls && !lighting_parse_calls && !lighting_set_calls);
        assert(!strstr(response,TEST_PASSWORD));
    }
    reset();memset(header,'a',129);header[129]=0;request.content_len=2;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("401");
    assert(!recv_calls && !lighting_parse_calls && !lighting_set_calls);
    reset();header_read_error_name="X-OTA-Password";request.content_len=2;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("401");
    assert(!recv_calls && !lighting_parse_calls && !lighting_set_calls);

    /* A password changed through the existing UI is also the GPIO credential. */
    reset(); const char *replacement="current-gpio-secret";
    request.content_len=strlen(replacement);set_receive_body(replacement,request.content_len);
    assert(password_post_handler(&request)==ESP_OK && !strcmp(s_password,replacement));
    const int previous_recv_calls=recv_calls;
    request.content_len=2;set_receive_body("45",2);expected_lighting_text="45";
    parsed_lighting_mask=UINT64_C(1)<<45;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("401");
    assert(recv_calls==previous_recv_calls && !lighting_parse_calls && !lighting_set_calls);
    strcpy(header,replacement);settings_header_value[0]=0;
    assert(lighting_gpios_post_handler(&request)==ESP_OK);check_status("200");
    assert(lighting_set_calls==1 && persisted_lighting_mask==parsed_lighting_mask);
    assert(!strstr(response,replacement) && !strstr(response,TEST_PASSWORD));
    header[0]=0;assert(lighting_gpios_get_handler(&request)==ESP_OK);
    assert(!strstr(response,replacement) && !strstr(response,TEST_PASSWORD));
    response[0]=0;assert(page_handler(&request)==ESP_OK);
    assert(!strstr(response,replacement) && !strstr(response,TEST_PASSWORD));
    reset();request.content_len=0;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("400");assert(!recv_calls);
    reset();request.content_len=LIGHTING_GPIO_TEXT_CAPACITY;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("413");assert(!recv_calls);
    const char *bad_types[]={"","application/json","text/plainly","text/html"};
    for (size_t i=0;i<sizeof(bad_types)/sizeof(*bad_types);++i) {
        reset();request.content_len=2;strcpy(content_type_header,bad_types[i]);
        assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("415");assert(!recv_calls);
    }
    reset();request.content_len=2;header_read_error_name="Content-Type";
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("415");
    reset();request.content_len=2;memset(content_type_header,'x',64);content_type_header[64]=0;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("415");
    reset();request.content_len=2;recv_values[0]=HTTPD_SOCK_ERR_TIMEOUT;recv_count=1;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("408");assert(!lighting_set_calls);
    reset();request.content_len=2;recv_values[0]=0;recv_count=1;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);assert(!lighting_set_calls);
    reset();request.content_len=5;set_receive_body("45\0,3",5);
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("400");assert(!lighting_parse_calls);
    const char *invalid[]={" ","45,45","45,,46","45,","-1","49","19","45;46","45 x"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        reset();request.content_len=strlen(invalid[i]);set_receive_body(invalid[i],request.content_len);
        expected_lighting_text=invalid[i];lighting_parse_error=ESP_ERR_INVALID_ARG;
        assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("400");
        assert(lighting_parse_calls==1 && !lighting_set_calls && persisted_lighting_mask==LIGHTING_GPIO_DEFAULT_MASK);
    }
    const esp_err_t failures[]={ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,ESP_ERR_TIMEOUT,ESP_FAIL};
    for (size_t i=0;i<sizeof(failures)/sizeof(*failures);++i) {
        reset();request.content_len=2;set_receive_body("45",2);expected_lighting_text="45";
        parsed_lighting_mask=UINT64_C(1)<<45;lighting_set_error=failures[i];
        assert(lighting_gpios_post_handler(&request)==ESP_FAIL);
        check_status(i==0?"400":i<3?"503":"500");
        assert(lighting_set_calls==1 && persisted_lighting_mask==LIGHTING_GPIO_DEFAULT_MASK);
    }
    reset();s_restart_pending=true;request.content_len=2;
    assert(lighting_gpios_get_handler(&request)==ESP_FAIL);check_status("503");
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);check_status("503");assert(!recv_calls);
    reset();lighting_format_error=ESP_FAIL;
    assert(lighting_gpios_get_handler(&request)==ESP_FAIL);check_status("500");
    reset();request.content_len=2;set_receive_body("45",2);expected_lighting_text="45";
    parsed_lighting_mask=UINT64_C(1)<<45;strcpy(content_type_header,"Text/Plain; charset=UTF-8");send_error=ESP_FAIL;
    assert(lighting_gpios_post_handler(&request)==ESP_FAIL);
    assert(persisted_lighting_mask==parsed_lighting_mask && lighting_set_calls==1);
    send_error=ESP_OK;assert(lighting_gpios_get_handler(&request)==ESP_OK);
    assert(strstr(response,"\"gpios\":[45]") && !strstr(response,TEST_PASSWORD));
}

static void test_price_url(void)
{
    httpd_req_t request = {.content_len = 0};
    const char *url = "https://example.test/price?pair=USD%2FILS&label=\"<'";
    reset(); header[0] = 0;
    assert(price_url_get_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(response, "") && url_get_calls==1 && recv_calls==0 && url_set_calls==0);
    strcpy(persisted_url, url);
    assert(price_url_get_handler(&request)==ESP_OK);
    assert(!strcmp(response, url));
    response[0]=0;
    assert(page_handler(&request)==ESP_OK);
    assert(!strstr(response, url)); /* Load stored URLs as data, not HTML. */
    reset(); url_get_error=ESP_FAIL;
    assert(price_url_get_handler(&request)==ESP_FAIL); check_status("500");
    assert(url_get_calls==1 && url_set_calls==0);

    /* Authentication precedes payload parsing, validation, or persistence. */
    reset(); request.content_len=SERVICE_URL_MAX_LENGTH+1; s_password_length=0;
    strcpy(content_type_header,"application/json");
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("403");
    assert(strstr(response,"Password initialization failed."));
    assert(recv_calls==0 && url_set_calls==0);
    for (int missing=0; missing<2; ++missing) {
        reset(); strcpy(header,missing ? "" : "wrong-secret");
        strcpy(content_type_header,"application/json");
        assert(price_url_post_handler(&request)==ESP_FAIL); check_status("401");
        assert(recv_calls==0 && url_set_calls==0);
    }
    reset(); memset(header,'a',129); header[129]=0;
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(recv_calls==0 && url_set_calls==0);
    reset(); s_restart_pending=true;
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("503");
    assert(recv_calls==0 && url_set_calls==0);
    reset();
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("413");
    assert(recv_calls==0 && url_set_calls==0);

    const char *invalid_types[]={"", "application/json", "text/plainwrong"};
    for (size_t i=0; i<sizeof(invalid_types)/sizeof(invalid_types[0]); ++i) {
        reset(); request.content_len=strlen(url); strcpy(content_type_header,invalid_types[i]);
        assert(price_url_post_handler(&request)==ESP_FAIL); check_status("415");
        assert(recv_calls==0 && url_set_calls==0);
    }
    reset(); request.content_len=5; set_receive_body("https",5);
    recv_values[0]=2; recv_values[1]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=2;
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("408");
    assert(url_set_calls==0 && !persisted_url[0]);
    reset(); request.content_len=5; set_receive_body("https",5);
    recv_values[0]=2; recv_values[1]=0; recv_count=2;
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("400");
    assert(url_set_calls==0 && !persisted_url[0]);
    reset(); request.content_len=3;
    const char nul_url[]={'h',0,'x'}; set_receive_body(nul_url,sizeof(nul_url));
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("400");
    assert(url_set_calls==0 && !persisted_url[0]);

    /* Setter validation and flash errors must leave the active URL unchanged. */
    for (int invalid=0; invalid<2; ++invalid) {
        reset(); strcpy(persisted_url,"https://old.test/price");
        request.content_len=strlen(url); set_receive_body(url,request.content_len);
        url_set_error=invalid ? ESP_ERR_INVALID_ARG : ESP_FAIL;
        assert(price_url_post_handler(&request)==ESP_FAIL); check_status(invalid ? "400" : "500");
        assert(url_set_calls==1 && !strcmp(requested_url,url));
        assert(!strcmp(persisted_url,"https://old.test/price"));
    }

    /* The exact URL, including query punctuation, is committed after all chunks. */
    reset(); request.content_len=strlen(url); set_receive_body(url,request.content_len);
    recv_values[0]=3; recv_values[1]=5; recv_values[2]=(int)request.content_len-8; recv_count=3;
    strcpy(content_type_header,"Text/Plain; charset=UTF-8");
    assert(price_url_post_handler(&request)==ESP_OK); check_status("200");
    assert(recv_calls==3 && receive_body_offset==request.content_len && url_set_calls==1);
    assert(!strcmp(persisted_url,url) && !s_restart_pending && begin_calls==0 && password_save_calls==0);
    assert(price_url_get_handler(&request)==ESP_OK && !strcmp(response,url));
    request.content_len=0;
    assert(price_url_post_handler(&request)==ESP_OK); check_status("200");
    assert(url_set_calls==2 && !persisted_url[0] && recv_calls==3);
    assert(price_url_get_handler(&request)==ESP_OK && !strcmp(response,""));

    reset(); request.content_len=SERVICE_URL_MAX_LENGTH;
    char longest[SERVICE_URL_MAX_LENGTH]; memset(longest,'x',sizeof(longest));
    memcpy(longest,"https://example.test/",21); set_receive_body(longest,sizeof(longest));
    assert(price_url_post_handler(&request)==ESP_OK); check_status("200");
    assert(strlen(persisted_url)==SERVICE_URL_MAX_LENGTH && url_set_calls==1);

    /* A password change takes effect immediately for URL updates too. */
    reset(); request.content_len=strlen("replacement-password");
    set_receive_body("replacement-password",request.content_len);
    assert(password_post_handler(&request)==ESP_OK);
    request.content_len=strlen(url); set_receive_body(url,request.content_len);
    int received_before=recv_calls;
    assert(price_url_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(recv_calls==received_before && url_set_calls==0);
    strcpy(header,"replacement-password");
    assert(price_url_post_handler(&request)==ESP_OK);
    assert(url_set_calls==1 && !strcmp(persisted_url,url));

    /* A lost response cannot undo a successfully persisted URL. */
    reset(); request.content_len=strlen(url); set_receive_body(url,request.content_len); send_error=ESP_FAIL;
    assert(price_url_post_handler(&request)==ESP_FAIL);
    assert(url_set_calls==1 && !strcmp(persisted_url,url));
}

int main(int argc, char **argv)
{
    test_show_password();
    test_price_url();
    test_lighting_gpios();
    test_animation_preferences();
    test_animation_status();
    httpd_req_t request = {.content_len=128};
    reset();
    assert(ota_server_start()==ESP_OK);
    assert(registered==14 && server_config.max_uri_handlers==14 && server_config.stack_size==8192 && server_config.recv_wait_timeout==15);
    assert(password_load_calls==1);
    assert(!strcmp(s_password,TEST_PASSWORD) && s_password_length==strlen(TEST_PASSWORD));
    assert(authorized(&request));
    assert(page_handler(&request)==ESP_OK && strstr(response,"data-enabled=\"1\""));
    assert(ota_server_start()==ESP_OK && registered==14 && password_load_calls==1);

    reset(); strcpy(persisted_password,"saved-test-password");
    memset(s_password,0,sizeof(s_password)); s_password_length=0;
    assert(ota_server_start()==ESP_OK && password_load_calls==1);
    assert(!strcmp(s_password,"saved-test-password") && s_password_length==strlen(persisted_password));
    strcpy(header,TEST_PASSWORD); assert(!authorized(&request));
    strcpy(header,persisted_password); assert(authorized(&request));
    reset(); persisted_password[0]=0;
    assert(ota_server_start()==ESP_OK && s_password_length==0 && registered==14);
    assert(!authorized(&request));
    assert(page_handler(&request)==ESP_OK && strstr(response,"data-enabled=\"0\""));
    reset(); password_load_error=ESP_FAIL;
    assert(ota_server_start()==ESP_FAIL && registered==0);

    reset(); s_password_length=0;
    assert(update_handler(&request)==ESP_FAIL); check_status("403"); assert(begin_calls==0);
    assert(strstr(response,"Password initialization failed."));
    assert(page_handler(&request)==ESP_OK); assert(strstr(response,"data-enabled=\"0\"") && !strstr(response,TEST_PASSWORD));

    reset(); strcpy(header,"wrong-secret");
    assert(update_handler(&request)==ESP_FAIL); check_status("401"); assert(begin_calls==0);
    reset(); strcpy(header,TEST_PASSWORD); header[strlen(header)-1]^=1; assert(!authorized(&request));
    reset(); header[0]=0; assert(!authorized(&request));
    reset(); memset(header,'a',129); header[129]=0; assert(!authorized(&request));

    reset(); request.content_len=0;
    assert(update_handler(&request)==ESP_FAIL); check_status("400"); assert(begin_calls==0);
    reset(); request.content_len=5000;
    assert(update_handler(&request)==ESP_FAIL); check_status("413"); assert(begin_calls==0);
    reset(); request.content_len=128; have_partition=false;
    assert(update_handler(&request)==ESP_FAIL); check_status("500"); assert(begin_calls==0);
    const esp_err_t identity_errors[] = {ESP_FAIL, ESP_ERR_INVALID_STATE};
    for (size_t i=0; i<sizeof(identity_errors)/sizeof(identity_errors[0]); ++i) {
        reset(); identity_prepare_error=identity_errors[i];
        assert(update_handler(&request)==ESP_FAIL); check_status("503");
        assert(identity_prepare_calls==1 && !begin_calls && !recv_calls && !write_calls && !boot_calls);
        assert(strstr(response,"original Matter pairing identity could not be verified"));
        assert(!s_restart_pending && !notify_calls);
    }
    reset(); begin_error=ESP_FAIL;
    assert(update_handler(&request)==ESP_FAIL); check_status("500"); assert(abort_calls==0 && end_calls==0);

    reset(); recv_values[0]=64; recv_values[1]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=2;
    assert(update_handler(&request)==ESP_FAIL); check_status("408"); assert(abort_calls==1 && write_calls==1 && end_calls==0 && boot_calls==0);
    reset(); recv_values[0]=0; recv_count=1;
    assert(update_handler(&request)==ESP_FAIL); assert(abort_calls==1 && write_calls==0 && boot_calls==0);
    reset(); write_error=ESP_ERR_OTA_VALIDATE_FAILED;
    assert(update_handler(&request)==ESP_FAIL); check_status("400"); assert(abort_calls==1 && end_calls==0 && boot_calls==0);
    reset(); write_error=ESP_FAIL;
    assert(update_handler(&request)==ESP_FAIL); check_status("500"); assert(abort_calls==1 && boot_calls==0);
    reset(); end_error=ESP_ERR_OTA_VALIDATE_FAILED;
    assert(update_handler(&request)==ESP_FAIL); check_status("400"); assert(end_calls==1 && abort_calls==0 && boot_calls==0);
    reset(); create_result=0;
    assert(update_handler(&request)==ESP_FAIL); check_status("500"); assert(boot_calls==0 && notify_calls==0);
    reset(); boot_error=ESP_FAIL;
    assert(update_handler(&request)==ESP_FAIL); check_status("500"); assert(delete_calls==1 && notify_calls==0);

    reset(); recv_values[0]=32; recv_values[1]=64; recv_values[2]=32; recv_count=3;
    assert(update_handler(&request)==ESP_OK); check_status("200");
    assert(begin_calls==1 && write_calls==3 && bytes_written==128 && end_calls==1 && boot_calls==1 && notify_calls==1 && abort_calls==0);
    assert(s_restart_pending);
    assert(update_handler(&request)==ESP_FAIL); check_status("503"); assert(begin_calls==1);

    reset(); send_error=ESP_FAIL;
    assert(update_handler(&request)==ESP_FAIL); assert(boot_calls==1 && notify_calls==1);
    reset(); assert(page_handler(&request)==ESP_OK);
    assert(strstr(response,"data-enabled=\"1\"") && !strstr(response,TEST_PASSWORD));
    assert(strstr(response,"<span>Animation</span>") && strstr(response,"id=\"animation\""));
    assert(strstr(response,"<span>Stick figure animation</span>") && strstr(response,"id=\"stick-figure\""));
    assert(strstr(response,".toggle input:disabled+span{opacity:.5}"));
    assert(!strstr(response,"Odometer animation"));

    reset(); request.content_len=1;
    assert(settings_get_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(response,"{\"animation_enabled\":true,\"stick_figure_enabled\":true}"));
    animation_enabled=false;
    assert(settings_get_handler(&request)==ESP_OK);
    assert(!strcmp(response,"{\"animation_enabled\":false,\"stick_figure_enabled\":true}"));

    reset(); settings_header_value[0]=0;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("403"); assert(settings_set_calls==0);
    reset(); strcpy(settings_header_value,"0");
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("403"); assert(settings_set_calls==0);
    reset(); strcpy(settings_header_value,"11");
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("403"); assert(settings_set_calls==0);
    reset(); request.content_len=0;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("400"); assert(settings_set_calls==0);
    reset(); request.content_len=2;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("400"); assert(settings_set_calls==0);
    reset(); request.content_len=1; strcpy(content_type_header,"application/x-www-form-urlencoded");
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("415"); assert(settings_set_calls==0);
    reset(); strcpy(content_type_header,"text/plainwrong");
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("415"); assert(settings_set_calls==0);
    reset(); content_type_header[0]=0;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("415"); assert(settings_set_calls==0);
    reset(); receive_byte='x';
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("400"); assert(settings_set_calls==0);
    reset(); recv_values[0]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=1;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("408"); assert(settings_set_calls==0);
    reset(); recv_values[0]=0; recv_count=1;
    assert(settings_post_handler(&request)==ESP_FAIL); assert(settings_set_calls==0);
    reset(); s_restart_pending=true;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("503"); assert(settings_set_calls==0);
    reset(); receive_byte='0'; settings_set_error=ESP_FAIL;
    assert(settings_post_handler(&request)==ESP_FAIL); check_status("500");
    assert(settings_set_calls==1 && animation_enabled && strstr(response,"could not save"));

    reset(); s_password_length=0; header[0]=0; receive_byte='0';
    assert(settings_post_handler(&request)==ESP_OK); check_status("200");
    assert(settings_set_calls==1 && !animation_enabled && !strcmp(response,"{\"animation_enabled\":false,\"stick_figure_enabled\":true}"));
    receive_byte='1'; strcpy(content_type_header,"Text/Plain; charset=UTF-8");
    assert(settings_post_handler(&request)==ESP_OK); check_status("200");
    assert(settings_set_calls==2 && animation_enabled && !strcmp(response,"{\"animation_enabled\":true,\"stick_figure_enabled\":true}"));
    assert(begin_calls==0 && write_calls==0);

    /* Price tests validate the complete request before touching the backend. */
    const char *invalid_test_headers[]={"", "0", "11"};
    for (size_t i=0; i<sizeof(invalid_test_headers)/sizeof(invalid_test_headers[0]); ++i) {
        reset(); request.content_len=1; receive_byte='+';
        strcpy(price_test_header_value,invalid_test_headers[i]);
        assert(price_test_post_handler(&request)==ESP_FAIL); check_status("403");
        assert(recv_calls==0 && price_test_calls==0);
    }
    reset(); header_read_error_name="X-Price-Test";
    assert(price_test_post_handler(&request)==ESP_FAIL); check_status("403");
    assert(recv_calls==0 && price_test_calls==0);
    const size_t invalid_test_lengths[]={0, 2, 64, (size_t)-1};
    for (size_t i=0; i<sizeof(invalid_test_lengths)/sizeof(invalid_test_lengths[0]); ++i) {
        reset(); request.content_len=invalid_test_lengths[i];
        assert(price_test_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(recv_calls==0 && price_test_calls==0);
    }
    const char *invalid_price_types[]={"", "application/x-www-form-urlencoded", "text/plainwrong"};
    for (size_t i=0; i<sizeof(invalid_price_types)/sizeof(invalid_price_types[0]); ++i) {
        reset(); request.content_len=1; strcpy(content_type_header,invalid_price_types[i]);
        assert(price_test_post_handler(&request)==ESP_FAIL); check_status("415");
        assert(recv_calls==0 && price_test_calls==0);
    }
    reset(); memset(content_type_header,'x',64); content_type_header[64]=0;
    assert(price_test_post_handler(&request)==ESP_FAIL); check_status("415");
    assert(recv_calls==0 && price_test_calls==0);
    reset(); header_read_error_name="Content-Type";
    assert(price_test_post_handler(&request)==ESP_FAIL); check_status("415");
    assert(recv_calls==0 && price_test_calls==0);
    const unsigned char invalid_price_values[]={0, '1', '0', 'x', ' ', '\n', 255};
    for (size_t i=0; i<sizeof(invalid_price_values); ++i) {
        reset(); receive_byte=invalid_price_values[i];
        assert(price_test_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(recv_calls==1 && price_test_calls==0);
    }
    reset(); recv_values[0]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=1;
    assert(price_test_post_handler(&request)==ESP_FAIL); check_status("408");
    assert(price_test_calls==0);
    reset(); recv_values[0]=0; recv_count=1;
    assert(price_test_post_handler(&request)==ESP_FAIL); assert(price_test_calls==0);
    reset(); recv_values[0]=ESP_FAIL; recv_count=1;
    assert(price_test_post_handler(&request)==ESP_FAIL); assert(price_test_calls==0);
    reset(); s_restart_pending=true; receive_byte='+';
    assert(price_test_post_handler(&request)==ESP_FAIL); check_status("503");
    assert(recv_calls==0 && price_test_calls==0);
    const esp_err_t backend_errors[]={ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL, ESP_ERR_INVALID_ARG};
    for (size_t i=0; i<sizeof(backend_errors)/sizeof(backend_errors[0]); ++i) {
        reset(); receive_byte='+'; price_test_error=backend_errors[i];
        assert(price_test_post_handler(&request)==ESP_FAIL);
        check_status(i<2 ? "503" : "500");
        assert(price_test_calls==1 && price_test_increase && !strstr(response,generated_price));
    }
    /* No upgrade password or enabled digit animation is needed for testing. */
    reset(); receive_byte='+'; s_password_length=0; header[0]=0; animation_enabled=false;
    assert(price_test_post_handler(&request)==ESP_OK); check_status("200");
    assert(price_test_calls==1 && price_test_increase && !strcmp(response,generated_price));
    assert(!strcmp(response_type,"text/plain; charset=utf-8"));
    assert(begin_calls==0 && password_save_calls==0 && settings_set_calls==0 && !animation_enabled);
    reset(); receive_byte='-'; strcpy(content_type_header,"Text/Plain; charset=UTF-8");
    strcpy(generated_price,"$3,876.54");
    assert(price_test_post_handler(&request)==ESP_OK); check_status("200");
    assert(price_test_calls==1 && !price_test_increase && !strcmp(response,generated_price));
    reset(); receive_byte='+'; send_error=ESP_FAIL;
    assert(price_test_post_handler(&request)==ESP_FAIL); assert(price_test_calls==1);

    /* The catalog and manual controls cover every scene, regardless of state. */
    reset();
    assert(animations_get_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(response_type,"application/json") && response[0]=='[' && response[strlen(response)-1]==']');
    for (unsigned id=1; id<=ANIMATION_COUNT; ++id) {
        const animation_info_t *info=animation_catalog_lookup((animation_id_t)id);
        char entry[192];
        snprintf(entry,sizeof(entry),"{\"id\":%u,\"name\":\"%s\",\"allowed_states\":%u,\"duration_ms\":%lu}",
                 id,info->name,(unsigned)info->allowed_states,(unsigned long)info->duration_ms);
        assert(strstr(response,entry));
    }
    reset(); send_error=ESP_FAIL;
    assert(animations_get_handler(&request)==ESP_FAIL);
    for (unsigned id=1; id<=ANIMATION_COUNT; ++id) {
        reset(); s_password_length=0; header[0]=0;
        char body[3]; snprintf(body,sizeof(body),"%u",id);
        request.content_len=strlen(body); set_receive_body(body,request.content_len);
        if (id==ANIMATION_COUNT) { recv_values[0]=1; recv_values[1]=1; recv_count=2; }
        assert(animation_post_handler(&request)==ESP_OK); check_status("200");
        assert(animation_test_calls==1 && requested_animation==(animation_id_t)id);
        assert(!strcmp(response,animation_catalog_lookup((animation_id_t)id)->name));
        assert(begin_calls==0 && settings_set_calls==0 && password_save_calls==0 && price_test_calls==0);
    }
    for (size_t i=0; i<sizeof(invalid_test_headers)/sizeof(invalid_test_headers[0]); ++i) {
        reset(); request.content_len=1; receive_byte='1';
        strcpy(animation_test_header_value,invalid_test_headers[i]);
        assert(animation_post_handler(&request)==ESP_FAIL); check_status("403");
        assert(recv_calls==0 && animation_test_calls==0);
    }
    reset(); header_read_error_name="X-Animation-Test";
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("403");
    assert(recv_calls==0 && animation_test_calls==0);
    const size_t invalid_animation_lengths[]={0,3,64,(size_t)-1};
    for (size_t i=0; i<sizeof(invalid_animation_lengths)/sizeof(invalid_animation_lengths[0]); ++i) {
        reset(); request.content_len=invalid_animation_lengths[i];
        assert(animation_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(recv_calls==0 && animation_test_calls==0);
    }
    for (size_t i=0; i<sizeof(invalid_price_types)/sizeof(invalid_price_types[0]); ++i) {
        reset(); request.content_len=1; strcpy(content_type_header,invalid_price_types[i]);
        assert(animation_post_handler(&request)==ESP_FAIL); check_status("415");
        assert(recv_calls==0 && animation_test_calls==0);
    }
    reset(); memset(content_type_header,'x',64); content_type_header[64]=0;
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("415");
    assert(recv_calls==0 && animation_test_calls==0);
    reset(); header_read_error_name="Content-Type";
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("415");
    assert(recv_calls==0 && animation_test_calls==0);
    char past_last_id[3];
    snprintf(past_last_id,sizeof(past_last_id),"%u",(unsigned)ANIMATION_COUNT+1);
    const char *invalid_animation_ids[]={"0",past_last_id,"99","01","-1","+1"," 1","1 ","1\n","1x"};
    for (size_t i=0; i<sizeof(invalid_animation_ids)/sizeof(invalid_animation_ids[0]); ++i) {
        reset(); request.content_len=strlen(invalid_animation_ids[i]);
        set_receive_body(invalid_animation_ids[i],request.content_len);
        assert(animation_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(animation_test_calls==0);
    }
    reset(); request.content_len=1; receive_byte=0;
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("400"); assert(animation_test_calls==0);
    reset(); request.content_len=2; set_receive_body("32",2);
    recv_values[0]=1; recv_values[1]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=2;
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("408"); assert(animation_test_calls==0);
    reset(); request.content_len=1; recv_values[0]=0; recv_count=1;
    assert(animation_post_handler(&request)==ESP_FAIL); assert(animation_test_calls==0);
    reset(); request.content_len=1; receive_byte='1'; send_error=ESP_FAIL;
    assert(animation_post_handler(&request)==ESP_FAIL); assert(animation_test_calls==1);
    reset(); s_restart_pending=true;
    assert(animation_post_handler(&request)==ESP_FAIL); check_status("503");
    assert(recv_calls==0 && animation_test_calls==0);
    const esp_err_t animation_errors[]={ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,ESP_ERR_TIMEOUT,ESP_FAIL};
    for (size_t i=0; i<sizeof(animation_errors)/sizeof(animation_errors[0]); ++i) {
        reset(); request.content_len=1; receive_byte='1'; animation_test_error=animation_errors[i];
        assert(animation_post_handler(&request)==ESP_FAIL); check_status(i<3?"503":"500");
        assert(animation_test_calls==1);
    }

    /* Credential changes must authenticate before receiving or saving data. */
    reset(); request.content_len=20; s_password_length=0;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("403");
    assert(strstr(response,"Password initialization failed."));
    assert(recv_calls==0 && password_save_calls==0);
    reset(); strcpy(header,"wrong-secret");
    assert(password_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(recv_calls==0 && password_save_calls==0);
    reset(); header[0]=0;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(recv_calls==0 && password_save_calls==0);
    reset(); memset(header,'a',129); header[129]=0;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(recv_calls==0 && password_save_calls==0);
    reset(); s_restart_pending=true;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("503");
    assert(recv_calls==0 && password_save_calls==0);

    reset(); request.content_len=0;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("400");
    assert(recv_calls==0 && password_save_calls==0);
    reset(); request.content_len=129;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("400");
    assert(recv_calls==0 && password_save_calls==0);
    const char *invalid_types[]={"", "application/x-www-form-urlencoded", "text/plainwrong"};
    for (size_t i=0; i<sizeof(invalid_types)/sizeof(invalid_types[0]); ++i) {
        reset(); request.content_len=3; strcpy(content_type_header,invalid_types[i]);
        assert(password_post_handler(&request)==ESP_FAIL); check_status("415");
        assert(recv_calls==0 && password_save_calls==0);
    }
    const unsigned char invalid_bytes[]={0, '\n', '\r', 31, 127, 128, 255};
    for (size_t i=0; i<sizeof(invalid_bytes); ++i) {
        reset(); request.content_len=3;
        unsigned char bad_body[]={'n',invalid_bytes[i],'p'};
        set_receive_body(bad_body,sizeof(bad_body));
        assert(password_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(password_save_calls==0 && !strcmp(s_password,TEST_PASSWORD));
    }

    reset(); request.content_len=5; set_receive_body("newpw",5);
    recv_values[0]=2; recv_values[1]=HTTPD_SOCK_ERR_TIMEOUT; recv_count=2;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("408");
    assert(password_save_calls==0 && !strcmp(s_password,TEST_PASSWORD));
    reset(); request.content_len=5; set_receive_body("newpw",5);
    recv_values[0]=2; recv_values[1]=0; recv_count=2;
    assert(password_post_handler(&request)==ESP_FAIL);
    assert(password_save_calls==0 && !strcmp(s_password,TEST_PASSWORD));

    reset(); request.content_len=strlen("replacement-password");
    set_receive_body("replacement-password",request.content_len);
    password_save_error=ESP_FAIL; expected_password_during_save=TEST_PASSWORD;
    assert(password_post_handler(&request)==ESP_FAIL); check_status("500");
    assert(password_save_calls==1 && !strcmp(s_password,TEST_PASSWORD));
    assert(!strcmp(persisted_password,TEST_PASSWORD) && authorized(&request));
    assert(!strstr(response,"replacement-password") && !strstr(response,TEST_PASSWORD));

    /* Read all chunks, commit first, then immediately use the new credential. */
    reset(); request.content_len=strlen("replacement-password");
    set_receive_body("replacement-password",request.content_len);
    recv_values[0]=2; recv_values[1]=5; recv_values[2]=(int)request.content_len-7; recv_count=3;
    expected_password_during_save=TEST_PASSWORD;
    assert(password_post_handler(&request)==ESP_OK); check_status("200");
    assert(password_save_calls==1 && recv_calls==3 && receive_body_offset==request.content_len);
    assert(!strcmp(s_password,"replacement-password") && !strcmp(persisted_password,s_password));
    assert(s_password_length==request.content_len && !s_restart_pending && boot_calls==0 && notify_calls==0);
    assert(!authorized(&request));
    assert(password_post_handler(&request)==ESP_FAIL); check_status("401");
    assert(password_save_calls==1 && recv_calls==3);
    assert(update_handler(&request)==ESP_FAIL); check_status("401"); assert(begin_calls==0);
    strcpy(header,"replacement-password"); assert(authorized(&request));
    assert(page_handler(&request)==ESP_OK);
    assert(!strstr(response,"replacement-password") && !strstr(response,TEST_PASSWORD));
    s_server=NULL; registered=0; password_load_calls=0;
    memset(s_password,0,sizeof(s_password)); s_password_length=0;
    assert(ota_server_start()==ESP_OK && password_load_calls==1 && registered==14);
    assert(!strcmp(s_password,"replacement-password") && authorized(&request));

    /* Losing the success response must not roll back a committed credential. */
    reset(); request.content_len=strlen("replacement-password");
    set_receive_body("replacement-password",request.content_len); send_error=ESP_FAIL;
    assert(password_post_handler(&request)==ESP_FAIL);
    assert(password_save_calls==1 && !strcmp(s_password,"replacement-password"));
    assert(!strcmp(persisted_password,s_password));
    strcpy(header,"replacement-password"); assert(authorized(&request));

    /* Full ASCII boundaries and the exact length limit. */
    reset(); request.content_len=1; set_receive_body("!",1);
    assert(password_post_handler(&request)==ESP_OK); check_status("200");
    assert(!strcmp(s_password,"!") && s_password_length==1);
    strcpy(header,"!"); assert(authorized(&request));
    reset(); request.content_len=128;
    char longest[128]; memset(longest,'~',sizeof(longest)); set_receive_body(longest,sizeof(longest));
    strcpy(content_type_header,"Text/Plain; charset=UTF-8");
    assert(password_post_handler(&request)==ESP_OK); check_status("200");
    assert(s_password_length==128 && strlen(persisted_password)==128 && password_save_calls==1);
    strcpy(header,s_password); assert(authorized(&request));
    const char *edge_spaces[]={" "," new password","new password "};
    for (size_t i=0; i<sizeof(edge_spaces)/sizeof(edge_spaces[0]); ++i) {
        reset(); request.content_len=strlen(edge_spaces[i]);
        set_receive_body(edge_spaces[i],request.content_len);
        assert(password_post_handler(&request)==ESP_FAIL); check_status("400");
        assert(password_save_calls==0);
    }
    reset(); request.content_len=12; set_receive_body("new password",12);
    assert(password_post_handler(&request)==ESP_OK);
    assert(!strcmp(s_password,"new password"));

    /* Export the actual compiled page for independent browser-script checks. */
    reset();
    assert(page_handler(&request)==ESP_OK);
    if (argc>1) {
        FILE *page=fopen(argv[1],"w");
        assert(page && fputs(response,page)>=0 && fclose(page)==0);
    }
    assert(strstr(response,"LED lighting GPIOs") && strstr(response,"id=\"lighting-gpios-form\""));
    assert(strstr(response,"id=\"lighting-gpios\"") && strstr(response,"id=\"save-lighting-gpios\""));
    assert(strstr(response,"id=\"lighting-gpios-status\""));
    const char *firmware_section=strstr(response,"<section aria-labelledby=\"firmware-heading\">");
    assert(firmware_section);
    const char *firmware_end=strstr(firmware_section,"</section>");
    const char *lighting_section=strstr(response,"<section aria-labelledby=\"lighting-heading\">");
    assert(firmware_section && firmware_end && lighting_section && lighting_section>firmware_end);
    const char *lighting_end=strstr(lighting_section,"</section>");
    const char *password_section=strstr(response,"<section aria-label=\"Change password\">");
    assert(lighting_end && password_section && password_section>lighting_end);
    const char *show_password_button=strstr(response,"<button id=\"show-password\"");
    const char *show_password_status=strstr(response,"<p id=\"show-password-status\"");
    const char *password_details=strstr(password_section,"<details><summary>Change password</summary>");
    assert(show_password_button && show_password_status && password_details &&
           show_password_button>password_section && show_password_status>show_password_button &&
           password_details>show_password_status);
    assert(strstr(password_section,"<p id=\"show-password-status\" role=\"status\" aria-live=\"polite\"></p>"
                                   "<details><summary>Change password</summary>"));
    const char *password_end=strstr(password_section,"</form></details></section>");
    assert(password_end && !strncmp(password_end,"</form></details></section></main>",
                                   strlen("</form></details></section></main>")));
    assert(strstr(lighting_section,"<h2 id=\"lighting-heading\">LED lighting GPIOs</h2>"));
    assert(strstr(response,"id=\"lighting-gpios-password\" type=\"password\""));
    const char *gpio_password_input=strstr(response,"<input id=\"lighting-gpios-password\"");
    assert(gpio_password_input);
    const char *gpio_password_end=strchr(gpio_password_input,'>');
    assert(gpio_password_input && gpio_password_end);
    const char *autocomplete=strstr(gpio_password_input,"autocomplete=\"current-password\"");
    assert(autocomplete && autocomplete<gpio_password_end);
    const char *maxlength=strstr(gpio_password_input,"maxlength=\"128\"");
    assert(maxlength && maxlength<gpio_password_end);
    const char *required=strstr(gpio_password_input,"required");
    assert(required && required<gpio_password_end);
    assert(strstr(response,"id=\"change-password\"") && strstr(response,"id=\"current-password\""));
    assert(strstr(response,"id=\"new-password\"") && strstr(response,"id=\"confirm-password\""));
    assert(strstr(response,"id=\"save-password\"") && strstr(response,"id=\"password-status\""));
    assert(strstr(response,"Price animation test") && strstr(response,"id=\"test-price-increase\""));
    assert(strstr(response,"id=\"test-price-decrease\"") && strstr(response,"id=\"price-test-status\""));
    assert(!strstr(response,"Each test changes") && !strstr(response,"random 1–5%") && !strstr(response,"$4,000"));
    assert(strstr(response,"id=\"animation-choice\"") && strstr(response,"id=\"run-animation\""));
    assert(strstr(response,"id=\"animation-test-status\" role=\"status\" aria-live=\"polite\""));
    assert(strstr(response,"id=\"price-url-form\"") && strstr(response,"id=\"price-url\""));
    assert(strstr(response,"id=\"price-url-password\"") && strstr(response,"id=\"save-price-url\""));
    assert(strstr(response,"id=\"price-url-status\"") && strstr(response,"type=\"url\""));
    assert(strstr(response,"maxlength=\"1024\""));
    assert(strstr(response,"id=\"show-password\"") && strstr(response,"id=\"show-password-status\""));
    assert(strstr(response,"<summary>Change password</summary>"));
    assert(!strstr(response,"Upgrade password") && !strstr(response,"upgrade password"));
    assert(!strstr(response,TEST_PASSWORD));
    puts("Web server host checks passed: settings/password/OTA, URL/GPIO authentication and persistence, screen-only password privacy, all 33 animation requests/status, price test validation and restart handling.");
}
