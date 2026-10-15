#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;

using portMUX_TYPE = unsigned;
#define portMUX_INITIALIZER_UNLOCKED 0
inline unsigned critical_depth;
inline bool gui_context = true;
inline void mock_enter_critical(portMUX_TYPE *lock) { ++*lock; ++critical_depth; }
inline void mock_exit_critical(portMUX_TYPE *lock)
{
    assert(*lock && critical_depth);
    --*lock;
    --critical_depth;
}
#define portENTER_CRITICAL(lock) mock_enter_critical(lock)
#define portEXIT_CRITICAL(lock) mock_exit_critical(lock)

using lv_coord_t = int16_t;
using lv_style_selector_t = uint32_t;
using lv_obj_flag_t = uint32_t;
using lv_color_t = uint32_t;
using lv_text_flag_t = uint8_t;
using lv_text_align_t = unsigned;
struct lv_point_t { lv_coord_t x; lv_coord_t y; };
struct lv_area_t { lv_coord_t x1; lv_coord_t y1; lv_coord_t x2; lv_coord_t y2; };
struct lv_draw_ctx_t {};
struct lv_obj_t;
struct lv_event_t { lv_obj_t *target; lv_draw_ctx_t *draw_ctx; };
using lv_event_cb_t = void (*)(lv_event_t *);
constexpr unsigned LV_EVENT_DRAW_MAIN = 1;
struct lv_font_t { lv_coord_t line_height; };
inline const lv_font_t lv_font_montserrat_40 = {44};
inline const lv_font_t mock_default_font = {14};
#define LV_FONT_DEFAULT &mock_default_font
#define LV_FONT_DECLARE(name) extern const lv_font_t name
constexpr lv_coord_t LV_COORD_MAX = 32767;
constexpr lv_style_selector_t LV_PART_MAIN = 0;
constexpr uint8_t LV_OPA_COVER = 255;
constexpr uint8_t LV_OPA_TRANSP = 0;
constexpr lv_obj_flag_t LV_OBJ_FLAG_HIDDEN = 1u << 0;
constexpr lv_obj_flag_t LV_OBJ_FLAG_SCROLLABLE = 1u << 1;
constexpr lv_obj_flag_t LV_OBJ_FLAG_OVERFLOW_VISIBLE = 1u << 2;
constexpr lv_obj_flag_t LV_OBJ_FLAG_CLICKABLE = 1u << 3;
constexpr lv_text_flag_t LV_TEXT_FLAG_EXPAND = 2;
constexpr unsigned LV_LABEL_LONG_CLIP = 4;
constexpr unsigned LV_SCROLLBAR_MODE_OFF = 0;
constexpr unsigned LV_ALIGN_TOP_MID = 0;
constexpr unsigned LV_ALIGN_CENTER = 1;
constexpr unsigned LV_TEXT_ALIGN_LEFT = 0;
constexpr unsigned LV_TEXT_ALIGN_CENTER = 1;
constexpr lv_coord_t LV_SIZE_CONTENT = -1;

struct lv_obj_t {
    lv_obj_t *parent = nullptr;
    bool label = false;
    bool deleted = false;
    lv_coord_t x = 0, y = 0, width = 536, height = 240;
    lv_point_t offset = {0, 0};
    lv_obj_flag_t flags = LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE;
    const char *static_text = nullptr;
    std::string dynamic_text;
    const lv_font_t *font = LV_FONT_DEFAULT;
    lv_coord_t letter_space = 0, line_space = 0;
    uint8_t opacity = 0;
    uint8_t text_opacity = LV_OPA_COVER;
    lv_color_t background = 0;
    lv_coord_t padding = 0, border = 0, radius = 0;
    bool recolor = false;
    unsigned long_mode = 0;
    unsigned foreground_calls = 0;
    unsigned invalidation_calls = 0;
    lv_event_cb_t draw_callback = nullptr;
};
using lv_label_t = lv_obj_t;

inline lv_obj_t mock_objects[12];
inline unsigned object_count;
inline unsigned creation_calls;
inline unsigned fail_creation_at;
inline unsigned lvgl_calls;
inline unsigned geometry_calls;
inline unsigned style_calls;
inline unsigned text_measure_calls;
inline uint32_t mock_tick;
inline lv_coord_t mock_text_width = 180;
inline void mock_gui()
{
    assert(gui_context && critical_depth == 0);
    ++lvgl_calls;
}
inline lv_obj_t *mock_create(lv_obj_t *parent, bool label)
{
    mock_gui();
    assert(parent && !parent->deleted);
    ++creation_calls;
    if (creation_calls == fail_creation_at) return nullptr;
    assert(object_count < sizeof(mock_objects) / sizeof(mock_objects[0]));
    lv_obj_t *object = &mock_objects[object_count++];
    object->parent = parent;
    object->label = label;
    return object;
}
inline lv_obj_t *lv_obj_create(lv_obj_t *parent) { return mock_create(parent, false); }
inline lv_obj_t *lv_label_create(lv_obj_t *parent) { return mock_create(parent, true); }
inline void lv_obj_del(lv_obj_t *object)
{
    mock_gui();
    assert(object && !object->deleted);
    object->deleted = true;
    for (unsigned i = 0; i < object_count; ++i) {
        if (mock_objects[i].parent == object && !mock_objects[i].deleted) lv_obj_del(&mock_objects[i]);
    }
}
inline void lv_obj_remove_style_all(lv_obj_t *object) { mock_gui(); assert(object); ++style_calls; }
inline void lv_obj_add_flag(lv_obj_t *object, lv_obj_flag_t flags)
{ mock_gui(); assert(object); object->flags |= flags; }
inline void lv_obj_clear_flag(lv_obj_t *object, lv_obj_flag_t flags)
{ mock_gui(); assert(object); object->flags &= ~flags; }
inline void lv_obj_set_size(lv_obj_t *object, lv_coord_t width, lv_coord_t height)
{ mock_gui(); assert(object); ++geometry_calls; object->width = width; object->height = height; }
inline void lv_obj_set_width(lv_obj_t *object, lv_coord_t width)
{ mock_gui(); assert(object); ++geometry_calls; object->width = width; }
inline void lv_obj_set_height(lv_obj_t *object, lv_coord_t height)
{ mock_gui(); assert(object); ++geometry_calls; object->height = height; }
inline void lv_obj_set_pos(lv_obj_t *object, lv_coord_t x, lv_coord_t y)
{ mock_gui(); assert(object); ++geometry_calls; object->x = x; object->y = y; }
inline void lv_obj_set_x(lv_obj_t *object, lv_coord_t x)
{ mock_gui(); assert(object); ++geometry_calls; object->x = x; }
inline void lv_obj_invalidate(lv_obj_t *object)
{ mock_gui(); assert(object && !object->deleted); ++object->invalidation_calls; }
inline lv_coord_t lv_obj_get_width(lv_obj_t *object)
{ mock_gui(); assert(object); return object->width; }
inline lv_coord_t lv_obj_get_height(lv_obj_t *object)
{ mock_gui(); assert(object); return object->height; }
inline void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area)
{
    mock_gui();
    assert(object && area);
    lv_coord_t x = object->x, y = object->y;
    for (lv_obj_t *parent = object->parent; parent; parent = parent->parent) {
        x += parent->x;
        y += parent->y;
    }
    *area = {x, y, static_cast<lv_coord_t>(x + object->width - 1),
                   static_cast<lv_coord_t>(y + object->height - 1)};
}
inline void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, unsigned code, void *)
{ mock_gui(); assert(object && code == LV_EVENT_DRAW_MAIN); object->draw_callback = callback; }
inline lv_draw_ctx_t *lv_event_get_draw_ctx(lv_event_t *event)
{ mock_gui(); assert(event); return event->draw_ctx; }
inline lv_coord_t lv_obj_get_content_width(lv_obj_t *object)
{ mock_gui(); assert(object); return object->width - 2 * object->padding; }
inline void lv_obj_update_layout(lv_obj_t *object) { mock_gui(); assert(object); }
inline void lv_obj_move_foreground(lv_obj_t *object)
{ mock_gui(); assert(object); ++object->foreground_calls; }
inline void lv_obj_align(lv_obj_t *object, unsigned align, lv_coord_t x, lv_coord_t y)
{
    mock_gui();
    assert(object && object->parent);
    ++geometry_calls;
    object->x = (object->parent->width - object->width) / 2 + x;
    object->y = align == LV_ALIGN_CENTER ? (object->parent->height - object->height) / 2 + y : y;
}
inline void lv_obj_set_scrollbar_mode(lv_obj_t *object, unsigned mode)
{ mock_gui(); assert(object && mode == LV_SCROLLBAR_MODE_OFF); }
inline void lv_obj_set_style_bg_color(lv_obj_t *object, lv_color_t color, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->background = color; }
inline void lv_obj_set_style_bg_opa(lv_obj_t *object, uint8_t opacity, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->opacity = opacity; }
inline void lv_obj_set_style_pad_all(lv_obj_t *object, lv_coord_t padding, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->padding = padding; }
inline void lv_obj_set_style_border_width(lv_obj_t *object, lv_coord_t border, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->border = border; }
inline void lv_obj_set_style_radius(lv_obj_t *object, lv_coord_t radius, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->radius = radius; }
inline void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, lv_style_selector_t)
{ mock_gui(); assert(object && font); ++style_calls; object->font = font; }
inline void lv_obj_set_style_text_color(lv_obj_t *object, lv_color_t, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; }
inline void lv_obj_set_style_text_opa(lv_obj_t *object, uint8_t opacity, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->text_opacity = opacity; }
inline void lv_obj_set_style_text_letter_space(lv_obj_t *object, lv_coord_t space, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->letter_space = space; }
inline void lv_obj_set_style_text_line_space(lv_obj_t *object, lv_coord_t space, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; object->line_space = space; }
inline void lv_obj_set_style_text_align(lv_obj_t *object, unsigned, lv_style_selector_t)
{ mock_gui(); assert(object); ++style_calls; }
inline lv_coord_t lv_obj_get_style_text_letter_space(lv_obj_t *object, lv_style_selector_t)
{ mock_gui(); assert(object); return object->letter_space; }
inline lv_coord_t lv_obj_get_style_text_line_space(lv_obj_t *object, lv_style_selector_t)
{ mock_gui(); assert(object); return object->line_space; }
inline lv_color_t lv_color_black() { return 0; }
inline lv_color_t lv_color_white() { return 0xffffff; }
inline uint32_t lv_tick_get() { mock_gui(); return mock_tick; }
inline uint32_t lv_tick_elaps(uint32_t previous) { mock_gui(); return mock_tick - previous; }
inline void lv_label_set_text_static(lv_obj_t *object, const char *text)
{ mock_gui(); assert(object && object->label && text); object->static_text = text; object->dynamic_text.clear(); }
inline void lv_label_set_text(lv_obj_t *object, const char *text)
{ mock_gui(); assert(object && object->label && text); object->dynamic_text = text; object->static_text = nullptr; }
inline void lv_label_set_long_mode(lv_obj_t *object, unsigned mode)
{ mock_gui(); assert(object && object->label); object->long_mode = mode; }
inline void lv_label_set_recolor(lv_obj_t *object, bool recolor)
{ mock_gui(); assert(object && object->label); object->recolor = recolor; }
inline void lv_txt_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
                            lv_coord_t letter_space, lv_coord_t line_space,
                            lv_coord_t max_width, lv_text_flag_t flags)
{
    mock_gui();
    ++text_measure_calls;
    assert(size && text && *text && font == &lv_font_montserrat_40);
    assert(letter_space == 0 && line_space == 0);
    assert(max_width == LV_COORD_MAX && flags == LV_TEXT_FLAG_EXPAND);
    size->x = mock_text_width;
    size->y = 44;
}

// Exercise lifecycle and drawing decisions here; the actual-LVGL harness checks
// the real rasterizer and every pixel of the bitmap scroll separately.
#define DISPLAY_TEXT_BITMAP_H
inline bool mock_bitmap_allocation_failure;
inline unsigned mock_bitmap_render_calls;
inline unsigned mock_bitmap_draw_calls;
inline unsigned mock_bitmap_erased_allocations;
inline uint32_t mock_bitmap_render_delay;
inline lv_area_t mock_bitmap_draw_area;

class TextBitmap {
public:
    lv_color_t *pixels = nullptr;
    lv_coord_t width = 0, height = 0;

    bool render(const char *text, const lv_font_t *font, lv_coord_t requested_width,
                lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
    {
        mock_gui();
        ++mock_bitmap_render_calls;
        assert(text && *text && font == &lv_font_montserrat_40);
        assert(requested_width == mock_text_width && align == LV_TEXT_ALIGN_LEFT);
        clear();
        mock_tick += mock_bitmap_render_delay;
        if (mock_bitmap_allocation_failure) return false;
        width = requested_width;
        height = static_cast<lv_coord_t>(font->line_height);
        storage.assign(static_cast<size_t>(width) * height, lv_color_white());
        pixels = storage.data();
        return true;
    }

    void draw(lv_draw_ctx_t *context, const lv_area_t &area) const
    {
        mock_gui();
        assert(context && pixels);
        assert(area.x2 - area.x1 + 1 == width && area.y2 - area.y1 + 1 == height);
        ++mock_bitmap_draw_calls;
        mock_bitmap_draw_area = area;
    }

    void clear(bool erase = false)
    {
        if (erase && pixels) {
            volatile lv_color_t *cursor = pixels;
            for (size_t i = 0; i < storage.size(); ++i) cursor[i] = 0;
            for (lv_color_t pixel : storage) assert(pixel == 0);
            ++mock_bitmap_erased_allocations;
        }
        storage.clear();
        pixels = nullptr;
        width = height = 0;
    }

private:
    std::vector<lv_color_t> storage;
};
