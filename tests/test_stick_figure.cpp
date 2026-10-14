#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#define private public
#include "odometer.h"
#undef private
#include "stick_figure.h"

LV_FONT_DECLARE(bodoni_48_smallcaps);

static constexpr int WIDTH = 240;
static constexpr int HEIGHT = 135;
static lv_color_t pixels[WIDTH * HEIGHT];
static lv_color_t draw_pixels[WIDTH * 16];
static uint32_t now_ms = 0;
static unsigned previews = 0;
static unsigned checks = 0;
static const char *preview_directory = nullptr;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    assert(area->x1 >= 0 && area->y1 >= 0);
    assert(area->x2 < WIDTH && area->y2 < HEIGHT);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * WIDTH + x] = *colors++;
    }
    lv_disp_flush_ready(driver);
}

static lv_obj_t *makeRoot(int width = WIDTH, int height = HEIGHT, int x = 0, int y = 0) {
    lv_obj_t *root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_size(root, width, height);
    lv_obj_set_pos(root, x, y);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

static bool sameArea(const lv_area_t &a, const lv_area_t &b) {
    return a.x1 == b.x1 && a.x2 == b.x2 && a.y1 == b.y1 && a.y2 == b.y2;
}

static lv_area_t relativeArea(lv_obj_t *root, lv_obj_t *object) {
    lv_area_t area, content;
    lv_obj_get_coords(object, &area);
    lv_obj_get_content_coords(root, &content);
    area.x1 -= content.x1;
    area.x2 -= content.x1;
    area.y1 -= content.y1;
    area.y2 -= content.y1;
    return area;
}

static void checkActorBounds(StickFigure &figure, lv_obj_t *root) {
    lv_area_t area;
    assert(figure.bounds(&area));
    assert(area.x1 >= 0 && area.y1 >= 0);
    assert(area.x2 < lv_obj_get_content_width(root));
    assert(area.y2 < lv_obj_get_content_height(root));
    assert(area.x1 <= area.x2 && area.y1 <= area.y2);
    ++checks;
}

static void checkTargets(OdometerDisplay &display, lv_obj_t *root, int expected_direction) {
    lv_obj_update_layout(root);
    lv_area_t number, digit;
    int direction = 123;
    assert(display.numberBounds(&number));
    assert(display.interactionTarget(&digit, &direction));
    assert(direction == expected_direction);
    assert(sameArea(number, relativeArea(root, display.numberLayer)));
    // Native's price stays 15 pixels left and 10 pixels above center. The
    // geometry hooks must report that placement even inside a padded root.
    assert(number.x1 == (lv_obj_get_content_width(root) - lv_area_get_width(&number)) / 2 - 15);
    assert(number.y1 == (lv_obj_get_content_height(root) - lv_area_get_height(&number)) / 2 - 10);
    lv_obj_t *expected = nullptr;
    if (display.isAnimating()) {
        assert(!display.rolls.empty());
        expected = lv_obj_get_parent(display.rolls.front().strip);
    } else {
        for (unsigned i = 0; i < lv_obj_get_child_cnt(display.numberLayer); ++i) {
            lv_obj_t *child = lv_obj_get_child(display.numberLayer, i);
            if (!lv_obj_check_type(child, &lv_label_class)) {
                expected = child;
                break;
            }
        }
    }
    assert(expected);
    lv_area_t viewport = relativeArea(root, expected);
    if (display.isAnimating()) {
        assert(sameArea(digit, viewport));
        // A rolling strip can have eleven rows, but the target has one row.
        assert(lv_area_get_height(&digit) == display.height);
    } else {
        assert(digit.x1 >= viewport.x1 && digit.x2 <= viewport.x2);
        assert(digit.y1 >= viewport.y1 && digit.y2 <= viewport.y2);
        // Check the idle geometry against rendered ink, independently of the
        // font descriptor arithmetic used by the public geometry hook.
        std::vector<lv_obj_t *> hidden;
        for (unsigned i = 0; i < lv_obj_get_child_cnt(root); ++i) {
            lv_obj_t *child = lv_obj_get_child(root, i);
            if (child != display.numberLayer && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                hidden.push_back(child);
            }
        }
        lv_refr_now(nullptr);
        lv_area_t content;
        lv_obj_get_content_coords(root, &content);
        lv_area_t ink = {viewport.x2, viewport.y2, viewport.x1, viewport.y1};
        unsigned ink_pixels = 0;
        for (int y = viewport.y1; y <= viewport.y2; ++y) {
            for (int x = viewport.x1; x <= viewport.x2; ++x) {
                if (pixels[(y + content.y1) * WIDTH + x + content.x1].full == lv_color_black().full) continue;
                ++ink_pixels;
                ink.x1 = std::min(ink.x1, static_cast<lv_coord_t>(x));
                ink.x2 = std::max(ink.x2, static_cast<lv_coord_t>(x));
                ink.y1 = std::min(ink.y1, static_cast<lv_coord_t>(y));
                ink.y2 = std::max(ink.y2, static_cast<lv_coord_t>(y));
            }
        }
        for (lv_obj_t *child : hidden) lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(nullptr);
        assert(ink_pixels > 0);
        assert(ink.x1 >= digit.x1 && ink.x2 <= digit.x2);
        assert(ink.y1 >= digit.y1 && ink.y2 <= digit.y2);
        assert(ink.x1 - digit.x1 <= 2 && digit.x2 - ink.x2 <= 2);
        assert(ink.y1 - digit.y1 <= 2 && digit.y2 - ink.y2 <= 2);
    }
    assert(digit.x1 >= number.x1 && digit.x2 <= number.x2);
    assert(digit.y1 >= number.y1 && digit.y2 <= number.y2);
    ++checks;
}

static void updateFigure(OdometerDisplay &display, StickFigure &figure, lv_obj_t *root) {
    lv_obj_update_layout(root);
    lv_area_t number, digit;
    int direction = 0;
    bool has_number = display.numberBounds(&number);
    bool has_digit = display.interactionTarget(&digit, &direction);
    figure.update(now_ms, has_number ? &number : nullptr,
                  has_digit ? &digit : nullptr, direction, display.isAnimating(),
                  display.valueChangeDirection());
    lv_obj_update_layout(root);
    checkActorBounds(figure, root);
}

static void advance(OdometerDisplay &display, StickFigure &figure, lv_obj_t *root, unsigned ms) {
    for (unsigned elapsed = 0; elapsed < ms;) {
        unsigned delta = std::min(20u, ms - elapsed);
        now_ms += delta;
        lv_tick_inc(delta);
        lv_timer_handler();
        updateFigure(display, figure, root);
        elapsed += delta;
    }
    lv_refr_now(nullptr);
}

static uint64_t frameHash() {
    uint64_t hash = 1469598103934665603ULL;
    for (const auto &pixel : pixels) hash = (hash ^ pixel.full) * 1099511628211ULL;
    return hash;
}

static void writePreview(const char *path) {
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output << "P6\n" << WIDTH << ' ' << HEIGHT << "\n255\n";
    for (const auto &pixel : pixels) {
        lv_color32_t rgb;
        rgb.full = lv_color_to32(pixel);
        char bytes[] = {static_cast<char>(rgb.ch.red), static_cast<char>(rgb.ch.green),
                        static_cast<char>(rgb.ch.blue)};
        output.write(bytes, sizeof(bytes));
    }
}

static void savePreview() {
    if (!preview_directory) return;
    char path[4096];
    int length = std::snprintf(path, sizeof(path), "%s/frame_%04u.ppm", preview_directory, previews++);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(path));
    writePreview(path);
}

static void saveNamedPreview(const char *name) {
    if (!preview_directory) return;
    char path[4096];
    int length = std::snprintf(path, sizeof(path), "%s/%s.ppm", preview_directory, name);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(path));
    writePreview(path);
}

static void checkReadability(OdometerDisplay &display, lv_obj_t *root, lv_obj_t *actor) {
    lv_area_t number;
    if (!display.numberBounds(&number)) return;
    lv_area_t content;
    lv_obj_get_content_coords(root, &content);
    lv_obj_add_flag(actor, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    std::vector<lv_color_t> baseline(std::begin(pixels), std::end(pixels));
    lv_obj_clear_flag(actor, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    unsigned glyph_pixels = 0, modified_pixels = 0;
    for (int y = number.y1 + content.y1; y <= number.y2 + content.y1; ++y) {
        for (int x = number.x1 + content.x1; x <= number.x2 + content.x1; ++x) {
            const unsigned index = y * WIDTH + x;
            if (baseline[index].full == lv_color_black().full) continue;
            ++glyph_pixels;
            if (baseline[index].full != pixels[index].full) ++modified_pixels;
        }
    }
    assert(glyph_pixels > 0);
    // A hand may touch a digit, but the price remains legible throughout.
    assert(modified_pixels * 100 <= glyph_pixels * 5);
    ++checks;
}

static void checkTextFallback(OdometerDisplay &display, StickFigure &figure, lv_obj_t *root,
                              const std::string &text) {
    display.setValue(text);
    advance(display, figure, root, 120);
    lv_area_t area;
    int direction = 123;
    // Visible service text also needs protection from overlapping figures.
    assert(display.numberBounds(&area) == !text.empty());
    assert(!display.interactionTarget(&area, &direction));
    assert(!display.isAnimating());
    assert(!display.numberLayer);
    assert(lv_obj_get_child_cnt(root) == 2); // One text label plus one actor.
    assert(std::string(lv_label_get_text(display.textLabel)) == text);
    ++checks;
}

static void finishReaction(OdometerDisplay &display, StickFigure &figure, lv_obj_t *root,
                           lv_obj_t *actor, int expected_change) {
#if ODOMETER_ANIMATION_DURATION_MS > 0
    unsigned elapsed = 0;
    while (display.isAnimating() && elapsed <= ODOMETER_ANIMATION_DURATION_MS + 100) {
        advance(display, figure, root, 40);
        savePreview();
        elapsed += 40;
    }
    assert(!display.isAnimating());
    advance(display, figure, root, 40);
    assert(figure.activity() == (expected_change < 0 ? StickFigure::Activity::Sad
                                                   : StickFigure::Activity::Celebrate));
    checkReadability(display, root, actor);
    ++checks;
#else
    (void)expected_change;
    (void)actor;
    advance(display, figure, root, 80);
    assert(!display.isAnimating());
    assert(figure.activity() != StickFigure::Activity::Sad);
    assert(figure.activity() != StickFigure::Activity::Celebrate);
#endif
}

struct ActorInk {
    std::vector<uint16_t> pixels;
    int top = 48;
    unsigned count = 0;
};

static ActorInk actorInk(lv_obj_t *actor) {
    ActorInk result;
    lv_area_t area;
    lv_obj_get_coords(actor, &area);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 46; ++x) {
            lv_color_t pixel = lv_color_black();
            int px = area.x1 + x, py = area.y1 + y;
            if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) pixel = pixels[py * WIDTH + px];
            // Keep gold character strokes; grayscale prices and the dark
            // ground shadow cannot make two different poses appear distinct.
            bool gold = pixel.ch.red >= 8 && pixel.ch.red > pixel.ch.blue + 3;
            result.pixels.push_back(gold ? pixel.full : 0);
            if (gold) {
                result.top = std::min(result.top, y);
                ++result.count;
            }
        }
    }
    return result;
}

static void checkLegacyMoods() {
    lv_obj_t *root = makeRoot();
    OdometerDisplay display;
    display.begin(root, &bodoni_48_smallcaps);
    display.setValue("$100.00");
    StickFigure figure;
    ActorInk happy, sad;
    for (int direction : {1, -1}) {
        figure.begin(root);
        lv_obj_t *actor = lv_obj_get_child(root, -1);
        lv_area_t number, digit;
        assert(display.numberBounds(&number));
        int idle_direction;
        assert(display.interactionTarget(&digit, &idle_direction));
        // Omit the optional signed-value argument, as existing callers do.
        for (unsigned i = 0; i < 25; ++i) {
            now_ms += 40;
            figure.update(now_ms, &number, &digit, direction, true);
            checkActorBounds(figure, root);
        }
        now_ms += 40;
        figure.update(now_ms, &number, &digit, 0, false);
        StickFigure::Activity mood = direction < 0 ? StickFigure::Activity::Sad
                                                  : StickFigure::Activity::Celebrate;
        assert(figure.activity() == mood);
        for (unsigned i = 0; i < 6; ++i) {
            now_ms += 40;
            figure.update(now_ms, &number, &digit, 0, false);
            checkActorBounds(figure, root);
        }
        lv_refr_now(nullptr);
        assert(figure.activity() == mood);
        checkReadability(display, root, actor);
        ActorInk ink = actorInk(actor);
        assert(ink.count > 30);
        if (direction > 0) happy = ink;
        else sad = ink;
        for (unsigned i = 0; i < 60; ++i) {
            now_ms += 40;
            figure.update(now_ms, &number, &digit, 0, false);
            checkActorBounds(figure, root);
        }
        assert(figure.activity() == StickFigure::Activity::Patrol);
        ++checks;
    }
    assert(happy.pixels != sad.pixels);
    assert(happy.top + 6 < sad.top); // Raised/jumping versus slumped posture.
    lv_obj_del(root);
    ++checks;
}

int main(int argc, char **argv) {
    if (argc > 1) preview_directory = argv[1];
    lv_init();
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, draw_pixels, nullptr, WIDTH * 16);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.draw_buf = &buffer;
    driver.flush_cb = flush;
    driver.hor_res = WIDTH;
    driver.ver_res = HEIGHT;
    lv_disp_drv_register(&driver);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    lv_obj_t *root = makeRoot();
    OdometerDisplay display;
    display.begin(root, &bodoni_48_smallcaps);
    display.setValue("$4,123.75");
    unsigned children_before = lv_obj_get_child_cnt(root);
    StickFigure figure;
    lv_area_t initial_area;
    assert(!figure.bounds(&initial_area));
    assert(!figure.bounds(nullptr));
    figure.update(now_ms, nullptr, nullptr, 0, false);
    figure.begin(root);
    assert(!figure.bounds(&initial_area));
    assert(lv_obj_get_child_cnt(root) == children_before + 1);
    lv_obj_t *actor = lv_obj_get_child(root, -1);
    assert(lv_obj_get_child_cnt(actor) == 0);
    updateFigure(display, figure, root);
    checkTargets(display, root, 0);

    std::vector<uint64_t> walking_frames;
    int min_x = WIDTH, max_x = 0, min_y = HEIGHT, max_y = 0;
    for (unsigned frame = 0; frame < 300; ++frame) {
        advance(display, figure, root, 80);
        lv_area_t area;
        assert(figure.bounds(&area));
        min_x = std::min(min_x, static_cast<int>(area.x1));
        max_x = std::max(max_x, static_cast<int>(area.x1));
        min_y = std::min(min_y, static_cast<int>(area.y1));
        max_y = std::max(max_y, static_cast<int>(area.y1));
        walking_frames.push_back(frameHash());
        if (frame % 10 == 0) checkReadability(display, root, actor);
        savePreview();
    }
    std::sort(walking_frames.begin(), walking_frames.end());
    assert(std::unique(walking_frames.begin(), walking_frames.end()) - walking_frames.begin() > 20);
    assert(max_x - min_x > 40 && max_y - min_y > 20);
    assert(!display.isAnimating()); // Walking does not hold the price service idle gate.

    struct Transition { const char *text; int roll; int change; const char *preview; };
    for (const auto &transition : std::vector<Transition>{
             {"$4,123.76", 1, 1, "increase_happy"}, {"$4,123.74", -1, -1, "decrease_sad"},
             {"-$4,123.75", 1, -1, "negative_decrease_sad"},
             {"-$4,123.74", -1, 1, "negative_increase_happy"},
             {"$123,456.78", 1, 1, "sign_flip_increase_happy"}}) {
        display.setValue(transition.text);
        assert(display.valueChangeDirection() == transition.change);
#if ODOMETER_ANIMATION_DURATION_MS > 0
        assert(display.isAnimating());
        checkTargets(display, root, transition.roll);
        if (std::string(transition.text) == "$4,123.76") {
            lv_obj_t *currency = lv_obj_get_child(display.numberLayer, 0);
            lv_obj_t *leading_digit = lv_obj_get_child(display.numberLayer, 1);
            assert(std::string(lv_label_get_text(currency)) == "$");
            assert(std::string(lv_label_get_text(lv_obj_get_child(leading_digit, 0))) == "4");
            lv_area_t active, leading = relativeArea(root, leading_digit);
            int direction;
            assert(display.interactionTarget(&active, &direction));
            assert(active.x1 > leading.x2);
            assert(relativeArea(root, currency).x2 < leading.x1);
            assert(lv_obj_get_parent(display.rolls.front().strip) != leading_digit);
            ++checks;
        }
        unsigned reactions = 0;
        for (unsigned frame = 0; frame < ODOMETER_ANIMATION_DURATION_MS / 80 + 2; ++frame) {
            advance(display, figure, root, 80);
            if (figure.activity() != StickFigure::Activity::Patrol) ++reactions;
            if (frame % 8 == 0) checkReadability(display, root, actor);
            savePreview();
        }
        assert(reactions > 0);
        assert(!display.isAnimating());
        StickFigure::Activity expected = transition.change < 0 ? StickFigure::Activity::Sad
                                                             : StickFigure::Activity::Celebrate;
        assert(figure.activity() == expected);
        for (unsigned frame = 0; frame < 12; ++frame) {
            advance(display, figure, root, 80);
            checkReadability(display, root, actor);
            savePreview();
            if (frame == (transition.change < 0 ? 4u : 1u)) saveNamedPreview(transition.preview);
        }
        ++checks;
#else
        checkTargets(display, root, 0);
        advance(display, figure, root, 160);
#endif
        checkTargets(display, root, 0);
    }

    // A replacement roll must finish with the newest numerical direction.
    display.setAnimationEnabled(false);
    display.setValue("$100.00");
    advance(display, figure, root, 2400);
    display.setAnimationEnabled(true);
    for (const auto &interruption : std::vector<Transition>{
             {"$99.00", -1, -1, nullptr}, {"$101.00", 1, 1, "falling_to_rising_happy"},
             {"$102.00", 1, 1, nullptr}, {"$100.00", -1, -1, "rising_to_falling_sad"},
             {"-$100.00", 0, -1, nullptr}, {"-$101.00", 1, -1, "negative_drop_sad"},
             {"-$99.00", -1, 1, "negative_rise_happy"}}) {
        display.setValue(interruption.text);
        assert(display.valueChangeDirection() == interruption.change);
        advance(display, figure, root, 80);
        if (interruption.preview) {
            finishReaction(display, figure, root, actor, interruption.change);
            advance(display, figure, root, interruption.change < 0 ? 480 : 240);
            checkReadability(display, root, actor);
            saveNamedPreview(interruption.preview);
            savePreview();
        }
    }

    // A same-magnitude sign flip cancels the reel immediately. Its numerical
    // direction must replace the previous roll's mood even on that last frame.
    struct ImmediateCompletion { const char *initial; const char *rolling; const char *final; int change; };
    for (const auto &completion : std::vector<ImmediateCompletion>{
             {"$100.00", "$101.00", "-$101.00", -1},
             {"-$100.00", "-$101.00", "$101.00", 1}}) {
        display.setAnimationEnabled(false);
        display.setValue(completion.initial);
        advance(display, figure, root, 2400);
        display.setAnimationEnabled(true);
        display.setValue(completion.rolling);
        assert(display.valueChangeDirection() == -completion.change);
        advance(display, figure, root, 80);
#if ODOMETER_ANIMATION_DURATION_MS > 0
        assert(display.isAnimating());
#endif
        display.setValue(completion.final);
        assert(display.valueChangeDirection() == completion.change);
        assert(!display.isAnimating());
        finishReaction(display, figure, root, actor, completion.change);
        checkTargets(display, root, 0);
        ++checks;
    }

    // A new value or disabled rolls must never retain a deleted digit pointer.
    display.setValue("$123,456.79");
    advance(display, figure, root, 80);
    display.setValue("$123,456.77");
    advance(display, figure, root, 80);
    display.setAnimationEnabled(false);
    advance(display, figure, root, 80);
    assert(!display.isAnimating());
    checkTargets(display, root, 0);
    display.setAnimationEnabled(true);
    checkTextFallback(display, figure, root, "Error 500");
    checkTextFallback(display, figure, root, "Error ESP_ERR_HTTP_CONNECTION_CLOSED");
    checkTextFallback(display, figure, root, "Error " + std::string(1023, 'x'));
    checkTextFallback(display, figure, root, "");
    display.setValue("$4,123.75");
    advance(display, figure, root, 2000);
    checkTargets(display, root, 0);
    assert(display.font == &bodoni_48_smallcaps);

    // Render once before monitoring so LVGL's reusable draw buffers are warm.
    lv_mem_monitor_t memory_before, memory_after;
    lv_mem_monitor(&memory_before);
    unsigned steady_children = lv_obj_get_child_cnt(root);
    for (unsigned i = 0; i < 10000; ++i) {
        now_ms += 32;
        updateFigure(display, figure, root);
        assert(lv_obj_get_child_cnt(root) == steady_children);
        assert(lv_obj_get_child_cnt(actor) == 0);
        if (i % 100 == 0) {
            lv_refr_now(nullptr);
            assert(lv_mem_test() == LV_RES_OK);
        }
    }
    lv_mem_monitor(&memory_after);
    assert(memory_after.used_cnt <= memory_before.used_cnt + 1);
    assert(memory_after.free_size + 128 >= memory_before.free_size);

    now_ms = UINT32_MAX - 80;
    for (unsigned i = 0; i < 12; ++i) {
        now_ms += 32;
        updateFigure(display, figure, root);
        lv_refr_now(nullptr);
    }
    assert(now_ms < 1000); // The same actor safely crosses the 32-bit clock wrap.

    display.setAnimationEnabled(false);
    lv_obj_del(root);
    lv_area_t area;
    assert(!figure.bounds(&area));
    figure.update(now_ms + 32, nullptr, nullptr, 0, false);
    assert(!figure.bounds(&area));

    // Reattach after root deletion; hooks are relative to padded root content.
    root = makeRoot(208, 112, 16, 12);
    lv_obj_set_style_pad_left(root, 5, 0);
    lv_obj_set_style_pad_right(root, 3, 0);
    lv_obj_set_style_pad_top(root, 4, 0);
    lv_obj_set_style_pad_bottom(root, 2, 0);
    OdometerDisplay offset_display;
    offset_display.begin(root, &bodoni_48_smallcaps);
    offset_display.setValue("$19.75");
    figure.begin(root);
    advance(offset_display, figure, root, 120);
    checkTargets(offset_display, root, 0);
    offset_display.setValue("$19.76");
#if ODOMETER_ANIMATION_DURATION_MS > 0
    checkTargets(offset_display, root, 1);
#endif
    advance(offset_display, figure, root, 120);
    offset_display.setAnimationEnabled(false);
    lv_obj_del(root);
    assert(!figure.bounds(&area));

    root = makeRoot();
    children_before = lv_obj_get_child_cnt(root);
    {
        StickFigure temporary;
        temporary.begin(root);
        temporary.update(now_ms + 32, nullptr, nullptr, 0, false);
        assert(lv_obj_get_child_cnt(root) == children_before + 1);
        lv_refr_now(nullptr);
        temporary.begin(nullptr);
        assert(lv_obj_get_child_cnt(root) == children_before);
        assert(!temporary.bounds(&area));
        temporary.begin(root);
        temporary.update(now_ms + 64, nullptr, nullptr, 0, false);
        lv_refr_now(nullptr);
    }
    assert(lv_obj_get_child_cnt(root) == children_before);
    lv_refr_now(nullptr); // No callback can refer to the destroyed owner.
    lv_obj_del(root);
    assert(lv_mem_test() == LV_RES_OK);
    checkLegacyMoods();
    assert(lv_mem_test() == LV_RES_OK);
    std::printf("PASS %u stick-figure moods, geometry, walking, interaction, lifetime and bounded-memory checks; %d ms; %u previews\n",
                checks, ODOMETER_ANIMATION_DURATION_MS, previews);
}
