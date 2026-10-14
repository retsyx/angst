#include "sdkconfig.h"

#include "board_display.h"
#include "animation_test.h"
#include "display_settings.h"
#include "led_lighting.h"
#include "lighting_settings.h"
#include "matter_display.h"
#include "odometer.h"
#include "ota_server.h"
#include "password_display.h"
#include "price_test.h"
#include "service.h"
#include "stick_figure.h"
#include "upgrade_password.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_FONT_DECLARE(bodoni_72_smallcaps);

static const char *TAG = "display";
static OdometerDisplay odometer;
static StickFigure stick_figure;
static constexpr uint32_t FIGURE_FRAME_PERIOD_MS = 32;
static constexpr uint32_t PASSWORD_FRAME_PERIOD_MS = 16;
static constexpr TickType_t PASSWORD_FRAME_TICKS = pdMS_TO_TICKS(PASSWORD_FRAME_PERIOD_MS)
    ? pdMS_TO_TICKS(PASSWORD_FRAME_PERIOD_MS) : 1;

static void display_price(const char *text)
{
    const uint32_t revision = odometer.valueChangeRevision();
    odometer.setValue(text);
    if (revision != odometer.valueChangeRevision()) {
        stick_figure.notifyPriceChange(odometer.valueChangeDirection());
    }
}

#if !LV_TICK_CUSTOM
static void lvgl_tick(void *)
{
    lv_tick_inc(5);
}
#endif

static void confirm_firmware()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "Firmware startup checks passed; OTA image confirmed");
    }
}

static void display_task(void *)
{
    // Preserve existing settings and Matter fabrics on storage errors.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition("matter"));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(lighting_settings_init());
    ESP_ERROR_CHECK(led_lighting_init(lighting_settings_get()));

    lv_init();
    lv_disp_t *display = nullptr;
    ESP_ERROR_CHECK(board_display_init(&display));
    lv_obj_t *root = lv_disp_get_scr_act(display);
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    odometer.begin(root, &bodoni_72_smallcaps);
    odometer.setValue("Angst");
    stick_figure.begin(root, CONFIG_DISPLAY_FIGURE_SCALE_PERCENT / 100.0f);
    stick_figure.setAutoplay(true);
    stick_figure.setRandomSeed(esp_random());
    ESP_ERROR_CHECK(password_display_init(lv_disp_get_layer_top(display)));

#if !LV_TICK_CUSTOM
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = lvgl_tick;
    tick_args.name = "lvgl_tick";
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 5000));
#endif
    lv_timer_handler();

    QueueHandle_t messages = xQueueCreate(1, sizeof(service_message_t));
    ESP_ERROR_CHECK(messages ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(price_test_init());
    ESP_ERROR_CHECK(animation_test_init());
    ESP_ERROR_CHECK(service_start(messages));
    ESP_ERROR_CHECK(matter_display_start());
    // Matter has initialized the radio entropy source for first-boot generation.
    ESP_ERROR_CHECK(upgrade_password_init());
    ESP_ERROR_CHECK(display_settings_init());
    const display_settings_state_t initial_settings = display_settings_get();
    odometer.setAnimationEnabled(initial_settings.animation_enabled);
    stick_figure.setEnabled(initial_settings.animation_enabled && initial_settings.stick_figure_enabled);
    animation_test_publish(ANIMATION_NONE);
#if CONFIG_DISPLAY_OTA_ENABLE
    ESP_ERROR_CHECK(ota_server_start());
#endif
    confirm_firmware();

    bool was_animating = false;
    bool was_showing_password = false;
    TickType_t password_frame_tick = 0;
    uint32_t last_figure_frame = lv_tick_get() - FIGURE_FRAME_PERIOD_MS;
    animation_id_t previous_scene = ANIMATION_NONE;
    service_message_t message;
    int applied_panel_brightness = -1;
    int applied_lights_brightness = -1;
    uint32_t last_lighting_failure = 0;
    bool lighting_retry_pending = false;
    for (;;) {
        // Only this task mutates LVGL objects; network and OTA run separately.
        bool showing_password = password_display_update();
        if (showing_password != was_showing_password) {
            if (showing_password) {
                // A top-layer overlay otherwise still renders the price below it.
                lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
                // The task drives 16 ms frames; make refresh due on every frame.
                lv_timer_set_period(display->refr_timer, 1);
                password_frame_tick = xTaskGetTickCount();
            } else {
                lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
                lv_timer_set_period(display->refr_timer, LV_DISP_DEF_REFR_PERIOD);
            }
        }
        if (showing_password) service_set_display_idle(false);
        uint8_t brightness = matter_display_brightness();
        const uint64_t lighting_gpios = lighting_settings_get();
        const uint32_t lighting_now = lv_tick_get();
        if (lighting_gpios == led_lighting_get_gpios()) {
            lighting_retry_pending = false;
        } else if (!lighting_retry_pending || lighting_now - last_lighting_failure >= 1000U) {
            esp_err_t error = led_lighting_set_gpios(lighting_gpios);
            lighting_retry_pending = error != ESP_OK;
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "LED GPIO configuration: %s", esp_err_to_name(error));
                last_lighting_failure = lighting_now;
            }
        }
        // A physical password reveal remains readable even if Matter is off.
        uint8_t panel_brightness = showing_password ? 255 : brightness;
        if (panel_brightness != applied_panel_brightness) {
            esp_err_t error = board_display_set_brightness(panel_brightness);
            if (error == ESP_OK) applied_panel_brightness = panel_brightness;
            else ESP_LOGW(TAG, "Display brightness: %s", esp_err_to_name(error));
        }
        if (brightness != applied_lights_brightness) {
            esp_err_t error = led_lighting_set_brightness(brightness);
            if (error == ESP_OK) applied_lights_brightness = brightness;
            else ESP_LOGW(TAG, "LED brightness: %s", esp_err_to_name(error));
        }
        const display_settings_state_t settings = display_settings_get();
        odometer.setAnimationEnabled(!showing_password && settings.animation_enabled);
        stick_figure.setEnabled(!showing_password && settings.animation_enabled && settings.stick_figure_enabled);
        if (!showing_password && xQueueReceive(messages, &message, 0) == pdTRUE) {
            if (price_test_accept_live(message.text)) display_price(message.text);
            service_set_display_idle(!odometer.isAnimating());
        }
        if (!showing_password) {
            bool used_fallback = false;
            bool is_test = false;
            if (price_test_take(message.text, sizeof(message.text), &used_fallback, &is_test)) {
                if (used_fallback) display_price("$4,000");
                display_price(message.text);
                service_set_display_idle(!odometer.isAnimating());
            }
            animation_id_t requested_scene;
            if (animation_test_take(&requested_scene)) stick_figure.playAnimation(requested_scene);
            const uint32_t now = lv_tick_get();
            if (now - last_figure_frame >= FIGURE_FRAME_PERIOD_MS) {
                last_figure_frame = now;
                lv_area_t number_bounds;
                lv_area_t digit_bounds;
                int direction = 0;
                const bool has_number = odometer.numberBounds(&number_bounds);
                const bool has_digit = odometer.interactionTarget(&digit_bounds, &direction);
                lv_area_t reel_bounds[StickScenes::MaxReels];
                const size_t reel_count = odometer.digitBounds(reel_bounds, StickScenes::MaxReels);
                stick_figure.update(now, has_number ? &number_bounds : nullptr,
                                    has_digit ? &digit_bounds : nullptr, direction,
                                    odometer.isAnimating(), odometer.valueChangeDirection(),
                                    reel_bounds, reel_count);
                if (stick_figure.takeReelRequest() && !odometer.isAnimating()) {
                    odometer.replayAnimation();
                    service_set_display_idle(!odometer.isAnimating());
                }
            }
        }
        const animation_id_t scene = static_cast<animation_id_t>(stick_figure.animationId());
        if (scene != previous_scene) {
            animation_test_publish(scene);
            previous_scene = scene;
        }
        const uint32_t next_handler_ms = lv_timer_handler();
        bool animating = odometer.isAnimating();
        if (!showing_password && (was_showing_password || (was_animating && !animating))) {
            service_set_display_idle(!animating);
        }
        was_animating = animating;
        was_showing_password = showing_password;
        if (showing_password) {
            const TickType_t elapsed_ticks = xTaskGetTickCount() - password_frame_tick;
            // Keep render time inside the frame budget and skip missed frames.
            password_frame_tick += (elapsed_ticks / PASSWORD_FRAME_TICKS) * PASSWORD_FRAME_TICKS;
            vTaskDelayUntil(&password_frame_tick, PASSWORD_FRAME_TICKS);
        } else {
            // Honor LVGL's next deadline without repeatedly polling a due timer.
            const uint32_t delay_ms = next_handler_ms < 1 ? 1 :
                                      next_handler_ms > 5 ? 5 : next_handler_ms;
            const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
            vTaskDelay(delay_ticks ? delay_ticks : 1);
        }
    }
}

extern "C" void app_main(void)
{
    // Keep rendering and its SPI interrupts separate from the radio tasks.
    // Initialization stays with the GUI task because it owns the display driver.
    BaseType_t result = xTaskCreatePinnedToCore(
        display_task, "display", ESP_TASK_MAIN_STACK, nullptr, 6, nullptr, 1);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
