// Compare fixed text offsets with the former moving-reel geometry using the
// actual LVGL renderer and the firmware's Bodoni font. All prices are dummy.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <lvgl.h>

namespace {
int bitmap_fail_after = -1;
std::unordered_set<void *> bitmap_allocations;
void *bitmap_allocate(size_t bytes) {
    if (bitmap_fail_after == 0) return nullptr;
    if (bitmap_fail_after > 0) --bitmap_fail_after;
    void *memory = lv_mem_alloc(bytes);
    if (memory) assert(bitmap_allocations.insert(memory).second);
    return memory;
}
void bitmap_free(void *memory) {
    assert(bitmap_allocations.erase(memory) == 1);
    lv_mem_free(memory);
}
}

#define DISPLAY_TEXT_BITMAP_ALLOC(bytes) bitmap_allocate(bytes)
#define DISPLAY_TEXT_BITMAP_FREE(memory) bitmap_free(memory)
#include "../main/text_bitmap.h"
#undef DISPLAY_TEXT_BITMAP_ALLOC
#undef DISPLAY_TEXT_BITMAP_FREE

#define private public
#include "../main/odometer.h"
#include "../main/stick_figure.h"
#undef private

LV_FONT_DECLARE(bodoni_72_smallcaps);

namespace {

constexpr int WIDTH = 536;
constexpr int HEIGHT = 240;
unsigned style_events;
unsigned size_events;
unsigned comparisons;

void disable_cache(OdometerDisplay &display) {
    // The reference must keep using LVGL glyph rendering, independently of the
    // new digit atlas, rather than compare two copies of the cached renderer.
    display.digitBitmapsReady = false;
    for (TextBitmap &bitmap : display.digitBitmaps) bitmap.clear();
}

struct Framebuffer {
    std::vector<lv_color_t> pixels = std::vector<lv_color_t>(WIDTH * HEIGHT);
    std::vector<lv_color_t> buffer = std::vector<lv_color_t>(WIDTH * 16);
    lv_disp_draw_buf_t draw_buffer{};
    lv_disp_drv_t driver{};
    lv_disp_t *display;
    std::vector<lv_area_t> flushed_areas;
    size_t flushed_pixels = 0;

    Framebuffer() {
        lv_disp_draw_buf_init(&draw_buffer, buffer.data(), nullptr, buffer.size());
        lv_disp_drv_init(&driver);
        driver.hor_res = WIDTH;
        driver.ver_res = HEIGHT;
        driver.draw_buf = &draw_buffer;
        driver.user_data = this;
        driver.rounder_cb = [](lv_disp_drv_t *, lv_area_t *area) {
            area->x1 &= ~1;
            area->y1 &= ~1;
            area->x2 |= 1;
            area->y2 |= 1;
        };
        driver.flush_cb = [](lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
            auto *frame = static_cast<Framebuffer *>(drv->user_data);
            frame->flushed_areas.push_back(*area);
            frame->flushed_pixels += static_cast<size_t>(area->x2 - area->x1 + 1) *
                                     (area->y2 - area->y1 + 1);
            for (int y = area->y1; y <= area->y2; ++y) {
                for (int x = area->x1; x <= area->x2; ++x) {
                    assert(x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT);
                    frame->pixels[y * WIDTH + x] = *colors++;
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
    lv_obj_t *screen() { return lv_disp_get_scr_act(display); }
    void refresh() { lv_refr_now(display); }
    void reset_flushes() { flushed_areas.clear(); flushed_pixels = 0; }
};

void count_events(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_STYLE_CHANGED) ++style_events;
    if (lv_event_get_code(event) == LV_EVENT_SIZE_CHANGED) ++size_events;
}

void compare(Framebuffer &actual, Framebuffer &reference, const char *scenario, int progress) {
    actual.refresh();
    reference.refresh();
    for (size_t pixel = 0; pixel < actual.pixels.size(); ++pixel) {
        if (actual.pixels[pixel].full != reference.pixels[pixel].full) {
            std::fprintf(stderr, "%s at progress %d: pixel (%zu,%zu), %u != %u\n",
                         scenario, progress, pixel % WIDTH, pixel / WIDTH,
                         actual.pixels[pixel].full, reference.pixels[pixel].full);
            assert(false);
        }
    }
    ++comparisons;
}

// Reproduce the former renderer independently of the new callback: full-height
// labels move inside their clipped parents, with no text offset.
void use_moving_reels(OdometerDisplay &reference) {
    lv_anim_del(&reference, OdometerDisplay::scrollDigits);
    for (const auto &roll : reference.rolls) {
        const char *text = lv_label_get_text(roll.strip);
        unsigned lines = 1;
        for (const char *cursor = text; *cursor; ++cursor) if (*cursor == '\n') ++lines;
        reinterpret_cast<lv_label_t *>(roll.strip)->offset.y = 0;
        lv_obj_set_height(roll.strip, lines * reference.height);
        lv_obj_set_y(roll.strip, roll.startY);
    }
}

void move_reference(OdometerDisplay &reference, int progress) {
    for (const auto &roll : reference.rolls) {
        lv_obj_set_y(roll.strip, roll.startY +
                     (roll.endY - roll.startY) * progress / 1000);
    }
}

void attach_counters(OdometerDisplay &actual) {
    for (const auto &roll : actual.rolls)
        lv_obj_add_event_cb(roll.strip, count_events, LV_EVENT_ALL, nullptr);
    style_events = size_events = 0;
}

void check_fixed_geometry(const OdometerDisplay &actual) {
    for (const auto &roll : actual.rolls) {
        assert(lv_obj_get_y(roll.strip) == 0);
        assert(lv_obj_get_height(roll.strip) == actual.height);
        assert(lv_obj_get_width(roll.strip) == actual.digitWidth);
        const auto *label = reinterpret_cast<const lv_label_t *>(roll.strip);
        assert(label->long_mode == LV_LABEL_LONG_CLIP && label->expand);
    }
}

void finish(OdometerDisplay &display) {
    lv_anim_del(&display, OdometerDisplay::scrollDigits);
    lv_anim_t animation{};
    animation.var = &display;
    OdometerDisplay::animationFinished(&animation);
    assert(!display.isAnimating() && display.rolls.empty());
}

void check_final(Framebuffer &actual_frame, OdometerDisplay &actual, const char *target) {
    Framebuffer expected;
    OdometerDisplay fresh;
    fresh.begin(expected.screen(), &bodoni_72_smallcaps);
    disable_cache(fresh);
    fresh.setValue(target);
    assert(!fresh.isAnimating());
    compare(actual_frame, expected, "finished value equals fresh static value", 1000);
    assert(actual.currentText == target);
    fresh.stopAnimation();
}

void check_transition(const char *initial, const char *target, bool expect_animation) {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    actual.begin(actual_frame.screen(), &bodoni_72_smallcaps);
    reference.begin(reference_frame.screen(), &bodoni_72_smallcaps);
    assert(actual.digitBitmapsReady);
    disable_cache(reference);
    actual.setValue(initial);
    reference.setValue(initial);
    compare(actual_frame, reference_frame, "initial static value", 0);
    actual.setValue(target);
    reference.setValue(target);
    assert(actual.isAnimating() == expect_animation);
    assert(reference.isAnimating() == expect_animation);
    use_moving_reels(reference);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    compare(actual_frame, reference_frame, target, 0);
    check_fixed_geometry(actual);
    attach_counters(actual);
    if (expect_animation) {
        // Include repeated callbacks, subpixel movement, one-way progress,
        // rewinds, and endpoints. Only translated glyphs may change.
        constexpr int progress_values[] = {
            0, 0, 1, 1, 2, 7, 17, 57, 101, 249, 500, 500, 501,
            777, 997, 999, 1000, 1000, 501, 17, 0, 1000,
        };
        for (int progress : progress_values) {
            OdometerDisplay::scrollDigits(&actual, progress);
            move_reference(reference, progress);
            compare(actual_frame, reference_frame, target, progress);
            check_fixed_geometry(actual);
            assert(style_events == 0 && size_events == 0);
        }
        finish(actual);
        finish(reference);
        compare(actual_frame, reference_frame, "finished transition", 1000);
    }
    check_final(actual_frame, actual, target);
    actual.stopAnimation();
    reference.stopAnimation();
}

void check_timed_and_interrupted() {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    actual.begin(actual_frame.screen(), &bodoni_72_smallcaps);
    reference.begin(reference_frame.screen(), &bodoni_72_smallcaps);
    assert(actual.digitBitmapsReady);
    disable_cache(reference);
    actual.setValue("$99.99");
    reference.setValue("$99.99");
    for (const char *target : {"$100.00", "$98.37", "$9.2"}) {
        actual.setValue(target);
        reference.setValue(target);
        assert(actual.isAnimating());
        use_moving_reels(reference);
        compare(actual_frame, reference_frame, "scheduled animation starts", 0);
        attach_counters(actual);
        // Exercise LVGL's real ease-in/out timer rather than just its exec callback.
        for (unsigned frame = 0; frame < 40; ++frame) {
            lv_tick_inc(16);
            lv_timer_handler();
            lv_anim_t *animation = lv_anim_get(&actual, OdometerDisplay::scrollDigits);
            assert(animation && actual.isAnimating());
            move_reference(reference, animation->current_value);
            compare(actual_frame, reference_frame, "scheduled interrupted animation", animation->current_value);
            check_fixed_geometry(actual);
            assert(style_events == 0 && size_events == 0);
        }
        // Repeating the same fetch cannot reset an in-progress animation.
        lv_anim_t *animation = lv_anim_get(&actual, OdometerDisplay::scrollDigits);
        const auto *layer = actual.numberLayer;
        actual.setValue(target);
        assert(actual.numberLayer == layer);
        assert(lv_anim_get(&actual, OdometerDisplay::scrollDigits) == animation);
    }
    // Allow the last transition to complete through LVGL's actual ready callback.
    unsigned frames = 0;
    while (actual.isAnimating()) {
        lv_tick_inc(16);
        lv_timer_handler();
        lv_anim_t *animation = lv_anim_get(&actual, OdometerDisplay::scrollDigits);
        if (animation) {
            move_reference(reference, animation->current_value);
            assert(style_events == 0 && size_events == 0);
        } else {
            assert(!actual.isAnimating());
            finish(reference);
        }
        compare(actual_frame, reference_frame, "scheduled animation completes", animation ? animation->current_value : 1000);
        assert(++frames < ODOMETER_ANIMATION_DURATION_MS / 16 + 5);
    }
    check_final(actual_frame, actual, "$9.2");
    actual.setValue("$10.3");
    reference.setValue("$10.3");
    assert(actual.isAnimating());
    actual.setAnimationEnabled(false);
    reference.setAnimationEnabled(false);
    assert(!actual.isAnimating());
    compare(actual_frame, reference_frame, "disabling animation snaps to value", 1000);
    check_final(actual_frame, actual, "$10.3");
    actual.stopAnimation();
    reference.stopAnimation();
}

void check_cache_allocation_failure(unsigned successful_allocations) {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    const size_t allocations_before = bitmap_allocations.size();
    bitmap_fail_after = successful_allocations;
    actual.begin(actual_frame.screen(), &bodoni_72_smallcaps);
    bitmap_fail_after = -1;
    assert(!actual.digitBitmapsReady);
    assert(!actual.frameCacheReady && actual.frameParts.empty());
    assert(bitmap_allocations.size() == allocations_before);
    for (const TextBitmap &bitmap : actual.digitBitmaps) {
        assert(!bitmap.pixels && !bitmap.width && !bitmap.height);
    }
    reference.begin(reference_frame.screen(), &bodoni_72_smallcaps);
    disable_cache(reference);
    actual.setValue("$99.99");
    reference.setValue("$99.99");
    compare(actual_frame, reference_frame, "OOM fallback static value", 0);
    actual.setValue("$100.00");
    reference.setValue("$100.00");
    assert(actual.isAnimating());
    use_moving_reels(reference);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    compare(actual_frame, reference_frame, "OOM fallback animation starts", 0);
    attach_counters(actual);
    for (int progress : {1, 77, 500, 777, 999, 1000}) {
        OdometerDisplay::scrollDigits(&actual, progress);
        move_reference(reference, progress);
        compare(actual_frame, reference_frame, "OOM fallback moving value", progress);
        assert(style_events == 0 && size_events == 0);
    }
    finish(actual);
    finish(reference);
    check_final(actual_frame, actual, "$100.00");
    actual.stopAnimation();
    reference.stopAnimation();
}

void check_punctuation_allocation_failure(unsigned successful_allocations) {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    actual.begin(actual_frame.screen(), &bodoni_72_smallcaps);
    reference.begin(reference_frame.screen(), &bodoni_72_smallcaps);
    disable_cache(reference);
    const size_t atlas_allocations = bitmap_allocations.size();
    actual.setValue("$99.99");
    reference.setValue("$99.99");
    assert(actual.frameCacheReady);
    assert(bitmap_allocations.size() == atlas_allocations + 2);
    compare(actual_frame, reference_frame, "cached frame before punctuation OOM", 0);

    // The next price needs three separately cached punctuation groups. Fail
    // each allocation in turn, after replacing the previous cached frame.
    bitmap_fail_after = successful_allocations;
    actual.setValue("$1,000.00");
    bitmap_fail_after = -1;
    reference.setValue("$1,000.00");
    assert(actual.digitBitmapsReady && !actual.frameCacheReady);
    assert(actual.frameParts.empty());
    assert(bitmap_allocations.size() == atlas_allocations);
    for (unsigned child = 0; child < lv_obj_get_child_cnt(actual.numberLayer); ++child)
        assert(!lv_obj_has_flag(lv_obj_get_child(actual.numberLayer, child), LV_OBJ_FLAG_HIDDEN));
    use_moving_reels(reference);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    compare(actual_frame, reference_frame, "punctuation OOM native animation starts", 0);
    attach_counters(actual);
    for (int progress : {1, 77, 500, 777, 999, 1000}) {
        OdometerDisplay::scrollDigits(&actual, progress);
        move_reference(reference, progress);
        compare(actual_frame, reference_frame, "punctuation OOM native animation", progress);
        assert(style_events == 0 && size_events == 0);
    }
    finish(actual);
    finish(reference);
    // Memory becomes available again: the next static frame can cache normally.
    assert(actual.frameCacheReady);
    check_final(actual_frame, actual, "$1,000.00");
    actual.stopAnimation();
    reference.stopAnimation();
}

void check_bounded_dirty_area(const char *target, unsigned changing_digits) {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    actual.begin(actual_frame.screen(), &bodoni_72_smallcaps);
    reference.begin(reference_frame.screen(), &bodoni_72_smallcaps);
    disable_cache(reference);
    actual.setValue("$100.00");
    reference.setValue("$100.00");
    actual.setValue(target);
    reference.setValue(target);
    use_moving_reels(reference);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    assert(actual.frameCacheReady && actual.rolls.size() == changing_digits);
    compare(actual_frame, reference_frame, "bounded dirty frame starts", 0);
    actual_frame.reset_flushes();
    assert(actual_frame.display->inv_p == 0);
    OdometerDisplay::scrollDigits(&actual, 500);
    assert(actual_frame.display->inv_p == 1);
    move_reference(reference, 500);
    compare(actual_frame, reference_frame, "bounded moving digit area", 500);
    assert(actual_frame.flushed_pixels > 0);
    // LVGL 8 expands transformed invalidation bounds by five pixels on each
    // side even at the default transform; the board adds even DMA alignment.
    const size_t width_limit = changing_digits * actual.digitWidth + 12;
    const size_t area_limit = width_limit * (actual.height + 12);
    if (actual_frame.flushed_pixels > area_limit) {
        std::fprintf(stderr, "Dirty area: %zu pixels > %zu limit, digit width %d, height %d, %zu flushes\n",
                     actual_frame.flushed_pixels, area_limit, actual.digitWidth,
                     actual.height, actual_frame.flushed_areas.size());
        for (const lv_area_t &area : actual_frame.flushed_areas)
            std::fprintf(stderr, "  flush %d,%d - %d,%d\n", area.x1, area.y1, area.x2, area.y2);
    }
    assert(actual_frame.flushed_pixels <= area_limit);
    assert(actual_frame.flushed_pixels <
           static_cast<size_t>(lv_obj_get_width(actual.numberLayer)) * actual.height / 2);
    for (const lv_area_t &area : actual_frame.flushed_areas)
        assert(static_cast<size_t>(area.x2 - area.x1 + 1) <= width_limit);

    actual_frame.reset_flushes();
    OdometerDisplay::scrollDigits(&actual, 500);
    assert(actual_frame.display->inv_p == 0);
    actual_frame.refresh();
    assert(actual_frame.flushed_areas.empty() && actual_frame.flushed_pixels == 0);
    finish(actual);
    finish(reference);
    check_final(actual_frame, actual, target);
    actual.stopAnimation();
    reference.stopAnimation();
}

void checkerboard_background(lv_event_t *event) {
    auto *context = lv_event_get_draw_ctx(event);
    lv_draw_rect_dsc_t style;
    lv_draw_rect_dsc_init(&style);
    style.bg_opa = LV_OPA_COVER;
    for (int y = 0; y < HEIGHT; y += 19) {
        for (int x = 0; x < WIDTH; x += 23) {
            style.bg_color = lv_color_hex(((x / 23 + y / 19) % 2) ? 0x19658b : 0x884a29);
            const lv_area_t tile = {static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y),
                                   static_cast<lv_coord_t>(std::min(x + 22, WIDTH - 1)),
                                   static_cast<lv_coord_t>(std::min(y + 18, HEIGHT - 1))};
            lv_draw_rect(context, &style, &tile);
        }
    }
}

lv_obj_t *background_root(Framebuffer &frame, bool checkerboard, bool clipped) {
    lv_obj_t *screen = frame.screen();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x285e89), LV_PART_MAIN);
    if (checkerboard) lv_obj_add_event_cb(screen, checkerboard_background, LV_EVENT_DRAW_MAIN, nullptr);
    if (!clipped) return screen;
    // Clip both horizontal and vertical glyph edges without changing the
    // price's native geometry or relying on a second implementation of it.
    lv_obj_t *window = lv_obj_create(screen);
    lv_obj_remove_style_all(window);
    lv_obj_clear_flag(window, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(window, 184, 70);
    lv_obj_set_pos(window, 29, 11);
    lv_obj_t *root = lv_obj_create(window);
    lv_obj_remove_style_all(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(root, WIDTH, HEIGHT);
    lv_obj_set_pos(root, -31, -57);
    return root;
}

void check_glyph_opacity(bool checkerboard, bool clipped) {
    Framebuffer actual_frame;
    Framebuffer reference_frame;
    OdometerDisplay actual;
    OdometerDisplay reference;
    actual.begin(background_root(actual_frame, checkerboard, clipped), &bodoni_72_smallcaps);
    reference.begin(background_root(reference_frame, checkerboard, clipped), &bodoni_72_smallcaps);
    assert(actual.digitBitmapsReady);
    disable_cache(reference);
    actual.setValue("-$1,999.89");
    reference.setValue("-$1,999.89");
    compare(actual_frame, reference_frame, "glyph opacity over colored background", 0);
    actual.setValue("-$2,001.00");
    reference.setValue("-$2,001.00");
    assert(actual.frameCacheReady && actual.isAnimating() && reference.isAnimating());
    use_moving_reels(reference);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    for (int progress : {0, 1, 57, 249, 500, 777, 999, 1000}) {
        OdometerDisplay::scrollDigits(&actual, progress);
        move_reference(reference, progress);
        compare(actual_frame, reference_frame, "moving glyph opacity over colored background", progress);
    }
    finish(actual);
    finish(reference);
    compare(actual_frame, reference_frame, "finished glyph opacity over colored background", 1000);
    actual.stopAnimation();
    reference.stopAnimation();
}

void check_background_through_glyph_gaps(Framebuffer &mask_frame,
                                       OdometerDisplay &mask,
                                       Framebuffer &actual_frame,
                                       const std::vector<lv_color_t> &background) {
    mask_frame.refresh();
    actual_frame.refresh();
    unsigned glyph_pixels = 0;
    for (size_t pixel = 0; pixel < background.size(); ++pixel) {
        if (mask_frame.pixels[pixel].full == lv_color_black().full) {
            // Zero glyph coverage includes counters, spacing, punctuation
            // backgrounds, and the gaps between moving reel rows.
            assert(actual_frame.pixels[pixel].full == background[pixel].full);
        } else if (mask_frame.pixels[pixel].full == lv_color_white().full) {
            assert(actual_frame.pixels[pixel].full == lv_color_white().full);
            ++glyph_pixels;
        }
    }
    assert(glyph_pixels > 0);
    if (mask.numberLayer) {
        lv_area_t coordinates;
        lv_obj_get_content_coords(mask.numberLayer, &coordinates);
        unsigned punctuation_parts = 0, digit_parts = 0;
        for (const auto &part : mask.frameParts) {
            unsigned transparent_pixels = 0, opaque_pixels = 0;
            for (int y = std::max(0, static_cast<int>(coordinates.y1));
                 y <= std::min(HEIGHT - 1, static_cast<int>(coordinates.y2)); ++y) {
                for (int x = std::max(0, coordinates.x1 + part.x);
                     x <= std::min(WIDTH - 1, coordinates.x1 + part.x + part.width - 1); ++x) {
                    const auto color = mask_frame.pixels[y * WIDTH + x];
                    transparent_pixels += color.full == lv_color_black().full;
                    opaque_pixels += color.full == lv_color_white().full;
                }
            }
            assert(transparent_pixels > 0 && opaque_pixels > 0);
            if (part.punctuation) ++punctuation_parts;
            else ++digit_parts;
        }
        assert(punctuation_parts == 3 && digit_parts == 6); // $, comma, decimal, and every digit.
    }
    ++comparisons;
}

void check_foreground_glyph_compositing(unsigned failure_mode) {
    Framebuffer mask_frame;
    Framebuffer actual_frame;
    lv_obj_t *actual_root = background_root(actual_frame, true, false);
    actual_frame.refresh();
    const auto background = actual_frame.pixels;
    OdometerDisplay mask;
    OdometerDisplay actual;
    mask.begin(mask_frame.screen(), &bodoni_72_smallcaps);
    // Exercise normal caching, a partial atlas failure, and punctuation OOM.
    if (failure_mode == 1) bitmap_fail_after = 3;
    actual.begin(actual_root, &bodoni_72_smallcaps);
    bitmap_fail_after = -1;
    assert(actual.digitBitmapsReady == (failure_mode != 1));
    const auto set_value = [&](const char *value) {
        mask.setValue(value);
        if (failure_mode == 2) bitmap_fail_after = 0;
        actual.setValue(value);
        bitmap_fail_after = -1;
        assert(actual.frameCacheReady == (failure_mode == 0));
    };
    set_value("$8,888.88");
    check_background_through_glyph_gaps(mask_frame, mask, actual_frame, background);
    set_value("$9,001.00");
    lv_anim_del(&mask, OdometerDisplay::scrollDigits);
    lv_anim_del(&actual, OdometerDisplay::scrollDigits);
    for (int progress : {0, 1, 57, 249, 500, 777, 999, 1000}) {
        OdometerDisplay::scrollDigits(&mask, progress);
        OdometerDisplay::scrollDigits(&actual, progress);
        check_background_through_glyph_gaps(mask_frame, mask, actual_frame, background);
    }
    finish(mask);
    if (failure_mode == 2) bitmap_fail_after = 0;
    finish(actual);
    bitmap_fail_after = -1;
    check_background_through_glyph_gaps(mask_frame, mask, actual_frame, background);
    mask.setValue("Connecting...");
    actual.setValue("Connecting...");
    check_background_through_glyph_gaps(mask_frame, mask, actual_frame, background);
    mask.stopAnimation();
    actual.stopAnimation();
}

void check_native_stick_scene(animation_id_t id) {
    Framebuffer frame;
    lv_obj_t *root = lv_obj_create(frame.screen());
    lv_obj_remove_style_all(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, WIDTH, HEIGHT);
    OdometerDisplay price;
    StickFigure figure;
    price.begin(root, &bodoni_72_smallcaps);
    price.setValue("$4,123.75");
    figure.begin(root, 2.0f);
    figure.setAutoplay(false);
    figure.setRandomSeed(37);
    lv_obj_update_layout(root);
    lv_area_t number, digit, reels[StickScenes::MaxReels];
    int direction = 0;
    assert(price.numberBounds(&number) && price.interactionTarget(&digit, &direction));
    const size_t reel_count = price.digitBounds(reels, StickScenes::MaxReels);
    figure.update(0, &number, &digit, direction, false, 0, reels, reel_count);
    assert(figure.playAnimation(id));
    unsigned rendered_frames = 0;
    for (uint32_t elapsed = 16; elapsed < 30000; elapsed += 16) {
        lv_tick_inc(16);
        lv_timer_handler();
        figure.update(elapsed, &number, &digit, direction, false, 0, reels, reel_count);
        lv_area_t bounds;
        assert(figure.bounds(&bounds));
        assert(bounds.x1 >= 0 && bounds.y1 >= 0 && bounds.x2 < WIDTH && bounds.y2 < HEIGHT);
        if (elapsed % 160 == 0) {
            lv_obj_add_flag(figure.actor, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(root);
            frame.refresh();
            const auto expected = frame.pixels;
            lv_obj_clear_flag(figure.actor, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(root);
            frame.refresh();
            unsigned protected_pixels = 0;
            for (size_t pixel = 0; pixel < expected.size(); ++pixel) {
                if (expected[pixel].full != lv_color_white().full) continue;
                ++protected_pixels;
                assert(frame.pixels[pixel].full == expected[pixel].full);
            }
            assert(protected_pixels > 0);
            ++rendered_frames;
        }
        if (!figure.isSceneActive()) break;
    }
    assert(!figure.isSceneActive() && rendered_frames > 20);
    price.stopAnimation();
    lv_obj_del(root);
    std::printf("Native 536x240 Bodoni72 scene %u: %u rendered frames protect price glyphs at 2x.\n",
                static_cast<unsigned>(id), rendered_frames);
}

} // namespace

int main() {
    lv_init();
    struct Scenario { const char *initial; const char *target; bool animated; };
    const Scenario scenarios[] = {
        {"$1.00", "$1.01", true}, {"$1.01", "$1.00", true},
        {"$9.99", "$10.00", true}, {"$10.00", "$9.99", true},
        {"$99.99", "$100.00", true}, {"$100.00", "$99.99", true},
        {"$199.99", "$200.00", true}, {"$200.00", "$199.99", true},
        {"$1.000", "$1.001", true}, {"$1.009", "$1.000", true},
        {"$12.345", "$13.2", true}, {"$13.2", "$12.345", true},
        {"42", "41.99", true}, {"41.99", "42", true},
        {"0009", "0010", true}, {"0010", "0009", true},
        {"-$42.00", "-$42.17", true}, {"-$42.17", "-$42.00", true},
        {"$-9.9", "$-10.1", true}, {"$-10.1", "$-9.9", true},
        {"$1,999.99", "$2,000.00", true}, {"$2,000.00", "$1,999.99", true},
        {"$10.00", "$10", false}, {"9", "009", false},
        {"Angst", "$1.23", false}, {"$1.23", "Error 500", false},
    };
    for (const auto &scenario : scenarios)
        check_transition(scenario.initial, scenario.target, scenario.animated);
    check_timed_and_interrupted();
    for (unsigned successful_allocations = 0; successful_allocations < 10; ++successful_allocations)
        check_cache_allocation_failure(successful_allocations);
    for (unsigned successful_allocations = 0; successful_allocations < 3; ++successful_allocations)
        check_punctuation_allocation_failure(successful_allocations);
    check_bounded_dirty_area("$100.01", 1);
    check_bounded_dirty_area("$100.11", 2);
    for (bool checkerboard : {false, true})
        for (bool clipped : {false, true}) check_glyph_opacity(checkerboard, clipped);
    for (unsigned failure_mode = 0; failure_mode < 3; ++failure_mode)
        check_foreground_glyph_compositing(failure_mode);
    check_native_stick_scene(ANIMATION_FLY);
    check_native_stick_scene(ANIMATION_TREASURE);
    assert(lv_anim_count_running() == 0);
    assert(bitmap_allocations.empty());
    std::printf("Actual LVGL odometer: %u pixel comparisons, colored/checkerboard glyph opacity and clipping, flat digit/punctuation cache, bounded dirty areas, OOM fallback, zero per-frame style/size events.\n", comparisons);
}
