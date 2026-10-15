// Actual LVGL rendering regression for dummy passwords only. No device secrets
// or password files are loaded by this test.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "lvgl.h"

namespace {

bool fail_next_bitmap_allocation;
unsigned bitmap_allocations;
unsigned erased_bitmap_allocations;
std::unordered_map<void *, size_t> bitmap_sizes;

void *allocate_private_bitmap(size_t bytes)
{
    if (fail_next_bitmap_allocation) {
        fail_next_bitmap_allocation = false;
        return nullptr;
    }
    void *pixels = lv_mem_alloc(bytes);
    assert(pixels);
    bitmap_sizes.emplace(pixels, bytes);
    ++bitmap_allocations;
    return pixels;
}

void free_private_bitmap(void *pixels)
{
    const auto allocation = bitmap_sizes.find(pixels);
    assert(allocation != bitmap_sizes.end());
    // Verify the actual cached password pixels are erased before releasing them.
    const unsigned char *bytes = static_cast<const unsigned char *>(pixels);
    for (size_t i = 0; i < allocation->second; ++i) assert(bytes[i] == 0);
    bitmap_sizes.erase(allocation);
    ++erased_bitmap_allocations;
    lv_mem_free(pixels);
}

} // namespace

#define DISPLAY_TEXT_BITMAP_ALLOC(bytes) allocate_private_bitmap(bytes)
#define DISPLAY_TEXT_BITMAP_FREE(pointer) free_private_bitmap(pointer)
#include "../main/password_display.cpp"

namespace {

struct Framebuffer {
    int width;
    int height;
    std::vector<lv_color_t> pixels;
    std::vector<lv_color_t> draw_buffer;
    lv_disp_draw_buf_t lv_buffer{};
    lv_disp_drv_t driver{};
    lv_disp_t *display;

    Framebuffer(int width_value, int height_value)
        : width(width_value), height(height_value), pixels(width * height), draw_buffer(width * 16)
    {
        lv_disp_draw_buf_init(&lv_buffer, draw_buffer.data(), nullptr, draw_buffer.size());
        lv_disp_drv_init(&driver);
        driver.hor_res = width;
        driver.ver_res = height;
        driver.draw_buf = &lv_buffer;
        driver.user_data = this;
        driver.flush_cb = [](lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
            auto *frame = static_cast<Framebuffer *>(drv->user_data);
            for (int y = area->y1; y <= area->y2; ++y) {
                for (int x = area->x1; x <= area->x2; ++x) {
                    assert(x >= 0 && x < frame->width && y >= 0 && y < frame->height);
                    frame->pixels[y * frame->width + x] = *colors++;
                }
            }
            lv_disp_flush_ready(drv);
        };
        display = lv_disp_drv_register(&driver);
        assert(display);
        lv_obj_t *screen = lv_disp_get_scr_act(display);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }

    ~Framebuffer() {
        lv_disp_remove(display);
        // LVGL 8.4 leaves the driver's software draw context for its owner to free.
        lv_mem_free(driver.draw_ctx);
        driver.draw_ctx = nullptr;
    }
    void refresh() { lv_refr_now(display); }
};

unsigned style_events;
unsigned coordinate_events;

void count_events(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_STYLE_CHANGED) ++style_events;
    if (lv_event_get_code(event) == LV_EVENT_SIZE_CHANGED) ++coordinate_events;
}

std::vector<lv_color_t> reference_text(const char *text, lv_coord_t width)
{
    Framebuffer reference(width, lv_font_montserrat_40.line_height);
    lv_obj_t *label = lv_label_create(lv_disp_get_scr_act(reference.display));
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(label, false);
    lv_label_set_text_static(label, text);
    lv_obj_set_size(label, width, lv_font_montserrat_40.line_height);
    lv_obj_set_pos(label, 0, 0);
    reference.refresh();
    return reference.pixels;
}

void compare_viewport(const Framebuffer &frame, const std::vector<lv_color_t> &reference,
                      lv_coord_t reference_width, int offset)
{
    const int text_y = (SCREEN_HEIGHT - lv_font_montserrat_40.line_height) / 2;
    const int text_x = (SCREEN_WIDTH - VIEWPORT_WIDTH) / 2;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            uint16_t expected = 0;
            const int glyph_x = x - text_x - offset;
            const int glyph_y = y - text_y;
            if (x >= text_x && x < text_x + VIEWPORT_WIDTH && glyph_x >= 0 &&
                glyph_x < reference_width && glyph_y >= 0 &&
                glyph_y < lv_font_montserrat_40.line_height) {
                expected = reference[glyph_y * reference_width + glyph_x].full;
            }
            const uint16_t actual = frame.pixels[y * frame.width + x].full;
            if (actual != expected) {
                std::fprintf(stderr, "Pixel mismatch at (%d,%d), offset %d: %u != %u\n",
                             x, y, offset, actual, expected);
                assert(actual == expected);
            }
        }
    }
}

void verify_password(Framebuffer &frame, const char *dummy, bool allocation_failure = false)
{
    lv_point_t measured;
    lv_txt_get_size(&measured, dummy, &lv_font_montserrat_40, 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    const auto reference = reference_text(dummy, measured.x);
    // The reference display is temporary; explicitly select the application
    // display again before creating or refreshing any app objects.
    lv_disp_set_default(frame.display);
    fail_next_bitmap_allocation = allocation_failure;
    const unsigned allocations_before = bitmap_allocations;
    const unsigned erased_before = erased_bitmap_allocations;
    assert(password_display_request(dummy) == ESP_OK);
    assert(password_display_update());
    assert(!fail_next_bitmap_allocation);
    assert(bool(password_bitmap.pixels) != allocation_failure);
    assert(bitmap_allocations == allocations_before + (allocation_failure ? 0u : 1u));
    assert(lv_obj_get_style_text_opa(password_label, LV_PART_MAIN) ==
           (allocation_failure ? LV_OPA_COVER : LV_OPA_TRANSP));
    frame.refresh();
    auto *label = reinterpret_cast<lv_label_t *>(password_label);
    assert(label->long_mode == LV_LABEL_LONG_CLIP && label->expand && !label->recolor);
    assert(label->static_txt && label->text == private_password);
    assert(std::strcmp(label->text, dummy) == 0);
    assert(lv_obj_get_width(password_label) == VIEWPORT_WIDTH);
    assert(lv_obj_get_x(password_label) == 0);
    assert(lv_obj_get_height(password_label) == lv_font_montserrat_40.line_height);
    const uint32_t start = shown_at;
    if (measured.x <= VIEWPORT_WIDTH) {
        compare_viewport(frame, reference, measured.x, (VIEWPORT_WIDTH - measured.x) / 2);
        lv_tick_inc(FIT_DURATION_MS);
    } else {
        compare_viewport(frame, reference, measured.x, 0);
        style_events = coordinate_events = 0;
        const lv_color_t *cached_pixels = password_bitmap.pixels;
        const auto verify_scroll_position = [&](uint32_t elapsed, lv_coord_t offset) {
            lv_tick_inc(start + elapsed - lv_tick_get());
            assert(password_display_update());
            frame.refresh();
            assert(label->offset.x == offset);
            assert(lv_obj_get_x(password_label) == 0);
            assert(lv_obj_get_width(password_label) == VIEWPORT_WIDTH);
            assert(password_bitmap.pixels == cached_pixels);
            assert(bitmap_allocations == allocations_before + (allocation_failure ? 0u : 1u));
            compare_viewport(frame, reference, measured.x, offset);
        };
        verify_scroll_position(END_HOLD_MS - 1, 0);
        const lv_coord_t far_offset = -static_cast<lv_coord_t>(scroll_distance);
        // Every leg preserves all glyphs one pixel at a time, while repeated
        // turns and endpoint holds reuse the same private bitmap allocation.
        for (unsigned pass = 0; pass < SCROLL_PASSES; ++pass) {
            const bool returning = pass % 2 != 0;
            const lv_coord_t initial_offset = returning ? far_offset : 0;
            const lv_coord_t final_offset = returning ? 0 : far_offset;
            const uint32_t leg_start = END_HOLD_MS + pass * (scroll_duration + END_HOLD_MS);
            if (pass) verify_scroll_position(leg_start - 1, initial_offset);
            verify_scroll_position(leg_start, initial_offset);
            verify_scroll_position(leg_start + 1, initial_offset);
            if (pass == 2) {
                assert(password_display_request("Blocked dummy replacement") == ESP_ERR_INVALID_STATE);
                assert(label->text == private_password && std::strcmp(label->text, dummy) == 0);
                assert(erased_bitmap_allocations == erased_before);
            }
            for (uint32_t pixel = 1; pixel < scroll_distance; ++pixel) {
                const uint32_t elapsed = leg_start +
                    (pixel * 1000U + SCROLL_PIXELS_PER_SECOND - 1U) / SCROLL_PIXELS_PER_SECOND;
                const lv_coord_t offset = returning ?
                    -static_cast<lv_coord_t>(scroll_distance - pixel) :
                    -static_cast<lv_coord_t>(pixel);
                verify_scroll_position(elapsed, offset);
            }
            const uint32_t arrival = leg_start + scroll_duration;
            verify_scroll_position(arrival - 1, final_offset + (returning ? -1 : 1));
            verify_scroll_position(arrival, final_offset);
            verify_scroll_position(arrival + 1, final_offset);
        }
        const uint32_t duration = END_HOLD_MS + (scroll_duration + END_HOLD_MS) * SCROLL_PASSES;
        verify_scroll_position(duration - 1, 0);
        assert(style_events == 0 && coordinate_events == 0);
        std::printf("Actual LVGL: %u legs of %u exact pixel translations, continuous turns, zero style/size events\n",
                    SCROLL_PASSES, scroll_distance);
        lv_tick_inc(start + duration - lv_tick_get());
    }
    assert(!password_display_update());
    assert(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    assert(label->text != private_password && !label->text[0]);
    assert(!password_bitmap.pixels && password_bitmap.width == 0 && password_bitmap.height == 0);
    assert(bitmap_sizes.empty());
    assert(erased_bitmap_allocations == erased_before + (allocation_failure ? 0u : 1u));
    for (char character : private_password) assert(character == '\0');
}

} // namespace

int main()
{
    lv_init();
    {
        Framebuffer frame(SCREEN_WIDTH, SCREEN_HEIGHT);
        assert(password_display_init(lv_disp_get_layer_top(frame.display)) == ESP_OK);
        lv_obj_add_event_cb(password_label, count_events, LV_EVENT_ALL, nullptr);
        verify_password(frame, "Aa#12!:");
        verify_password(frame, "#AbCd:MixedCASE dummy P4ssword! abcdefghijklmnopQRStuv1234567890");
        char longest_dummy[UPGRADE_PASSWORD_MAX_LENGTH + 1];
        std::memset(longest_dummy, 'W', UPGRADE_PASSWORD_MAX_LENGTH);
        longest_dummy[UPGRADE_PASSWORD_MAX_LENGTH] = '\0';
        verify_password(frame, longest_dummy);
        verify_password(frame, "#AbCd:MixedCASE dummy P4ssword! abcdefghijklmnopQRStuv1234567890", true);
        verify_password(frame, "Recovered cache after allocation failure!");
    }
    std::puts("Actual LVGL password rendering passed (only dummy strings). ");
}
