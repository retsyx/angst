#include "password_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "text_bitmap.h"
#include "upgrade_password.h"

namespace {

constexpr lv_coord_t SCREEN_WIDTH = 536;
constexpr lv_coord_t SCREEN_HEIGHT = 240;
constexpr lv_coord_t VIEWPORT_WIDTH = 504;
constexpr lv_coord_t VIEWPORT_HEIGHT = 64;
constexpr uint32_t FIT_DURATION_MS = 15000;
constexpr uint32_t END_HOLD_MS = 2000;
constexpr uint32_t SCROLL_PIXELS_PER_SECOND = 90;
constexpr unsigned SCROLL_PASSES = 4;

portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
bool ready;
bool pending;
bool busy;
char private_password[UPGRADE_PASSWORD_MAX_LENGTH + 1];

// All objects and timing state below belong exclusively to the LVGL task.
lv_obj_t *overlay;
lv_obj_t *password_label;
TextBitmap password_bitmap;
bool showing;
uint32_t shown_at;
lv_coord_t text_width;
uint32_t scroll_distance;
uint32_t scroll_duration;

void draw_password_bitmap(lv_event_t *event)
{
    if (!password_bitmap.pixels) return;
    lv_area_t area;
    lv_obj_get_coords(password_label, &area);
    const lv_label_t *label = reinterpret_cast<const lv_label_t *>(password_label);
    area.x1 += label->offset.x;
    area.y1 += label->offset.y;
    area.x2 = area.x1 + password_bitmap.width - 1;
    area.y2 = area.y1 + password_bitmap.height - 1;
    password_bitmap.draw(lv_event_get_draw_ctx(event), area);
}

void set_text_offset(lv_coord_t x)
{
    // Use LVGL 8.4's native text offset, leaving the label's layout fixed.
    // Moving the object would remeasure the password and relayout its parent.
    lv_label_t *label = reinterpret_cast<lv_label_t *>(password_label);
    if (label->offset.x == x) return;
    label->offset.x = x;
    lv_obj_invalidate(password_label);
}

void clear_password()
{
    volatile char *cursor = private_password;
    for (size_t i = 0; i < sizeof(private_password); ++i) cursor[i] = '\0';
}

void finish_display()
{
    // LVGL must lose its reference before the buffer can be erased or reused.
    lv_label_set_text_static(password_label, "");
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    password_bitmap.clear(true);
    clear_password();
    showing = false;
    portENTER_CRITICAL(&state_lock);
    busy = false;
    portEXIT_CRITICAL(&state_lock);
}

void begin_display()
{
    lv_point_t size;
    lv_txt_get_size(&size, private_password, &lv_font_montserrat_40, 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    text_width = size.x;
    scroll_distance = text_width > VIEWPORT_WIDTH ? text_width - VIEWPORT_WIDTH : 0;
    scroll_duration = (scroll_distance * 1000U + SCROLL_PIXELS_PER_SECOND - 1U) /
                      SCROLL_PIXELS_PER_SECOND;
    // Rasterize once; scrolling frames only copy the visible bitmap portion.
    // Keep the ordinary label as a fallback if the cache cannot be allocated.
    password_bitmap.clear(true);
    const bool cached = password_bitmap.render(private_password, &lv_font_montserrat_40,
                                                text_width, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_opa(password_label, cached ? LV_OPA_TRANSP : LV_OPA_COVER,
                             LV_PART_MAIN);
    lv_label_set_text_static(password_label, private_password);
    set_text_offset(scroll_distance ? 0 : (VIEWPORT_WIDTH - text_width) / 2);
    lv_obj_move_foreground(overlay);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    // Initial rasterization must not consume the first hold or skip motion.
    shown_at = lv_tick_get();
    showing = true;
}

} // namespace

extern "C" esp_err_t password_display_init(lv_obj_t *screen)
{
    if (!screen) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&state_lock);
    const bool already_ready = ready;
    portEXIT_CRITICAL(&state_lock);
    if (already_ready) return ESP_ERR_INVALID_STATE;

    overlay = lv_obj_create(screen);
    if (!overlay) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                              LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *viewport = lv_obj_create(overlay);
    if (!viewport) {
        lv_obj_del(overlay);
        overlay = nullptr;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    lv_obj_set_pos(viewport, (SCREEN_WIDTH - VIEWPORT_WIDTH) / 2,
                   (SCREEN_HEIGHT - VIEWPORT_HEIGHT) / 2);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                               LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    password_label = lv_label_create(viewport);
    if (!password_label) {
        lv_obj_del(overlay);
        overlay = nullptr;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(password_label);
    lv_obj_set_style_text_font(password_label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(password_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(password_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(password_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(password_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(password_label, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(password_label, false);
    lv_label_set_text_static(password_label, "");
    lv_obj_set_size(password_label, VIEWPORT_WIDTH, lv_font_montserrat_40.line_height);
    lv_obj_set_pos(password_label, 0, (VIEWPORT_HEIGHT - lv_font_montserrat_40.line_height) / 2);
    lv_obj_add_event_cb(password_label, draw_password_bitmap, LV_EVENT_DRAW_MAIN, nullptr);

    portENTER_CRITICAL(&state_lock);
    ready = true;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}

extern "C" esp_err_t password_display_request(const char *password)
{
    if (!password || password[0] == ' ') return ESP_ERR_INVALID_ARG;
    size_t length = 0;
    for (; length <= UPGRADE_PASSWORD_MAX_LENGTH; ++length) {
        const unsigned char character = static_cast<unsigned char>(password[length]);
        if (!character) break;
        if (character < 32 || character > 126) return ESP_ERR_INVALID_ARG;
    }
    if (!length || length > UPGRADE_PASSWORD_MAX_LENGTH || password[length - 1] == ' ')
        return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&state_lock);
    if (!ready || busy) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(private_password, password, length + 1);
    busy = true;
    pending = true;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}

extern "C" bool password_display_update(void)
{
    portENTER_CRITICAL(&state_lock);
    const bool start = pending;
    pending = false;
    portEXIT_CRITICAL(&state_lock);
    if (start) begin_display();
    if (!showing) return false;
    const uint32_t now = lv_tick_get();

    // Unsigned subtraction also handles the LVGL clock wrapping at UINT32_MAX.
    const uint32_t elapsed = now - shown_at;
    if (!scroll_distance) {
        if (elapsed < FIT_DURATION_MS) return true;
        finish_display();
        return false;
    }

    const uint32_t pass_duration = scroll_duration + END_HOLD_MS;
    if (elapsed >= END_HOLD_MS + pass_duration * SCROLL_PASSES) {
        finish_display();
        return false;
    }
    uint32_t offset = 0;
    if (elapsed > END_HOLD_MS) {
        const uint32_t scrolling_elapsed = elapsed - END_HOLD_MS;
        const uint32_t pass = scrolling_elapsed / pass_duration;
        const uint32_t pass_elapsed = scrolling_elapsed % pass_duration;
        const uint32_t travelled = pass_elapsed >= scroll_duration ? scroll_distance :
            (pass_elapsed * SCROLL_PIXELS_PER_SECOND) / 1000U;
        // Alternate direction from the current endpoint, without resetting X.
        offset = pass % 2U ? scroll_distance - travelled : travelled;
    }
    set_text_offset(-static_cast<lv_coord_t>(offset));
    return true;
}
