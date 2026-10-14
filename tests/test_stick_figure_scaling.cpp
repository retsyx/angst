#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#define private public
#include "odometer.h"
#undef private

// Observe the actual vector primitives and still render them through LVGL.
// This catches disappearing eyes and zero-width strokes at fractional scales.
static lv_area_t last_eye;
static unsigned eye_draws = 0;
static unsigned stroke_draws = 0;
static void observedRect(lv_draw_ctx_t *context, const lv_draw_rect_dsc_t *style, const lv_area_t *area) {
    if (style->border_width == 0 && style->bg_opa == LV_OPA_COVER &&
        style->bg_color.full == lv_color_hex(0xf2c87b).full) {
        assert(area->x1 <= area->x2 && area->y1 <= area->y2);
        last_eye = *area;
        ++eye_draws;
    }
    lv_draw_rect(context, style, area);
}
static void observedLine(lv_draw_ctx_t *context, const lv_draw_line_dsc_t *style,
                         const lv_point_t *a, const lv_point_t *b) {
    assert(style->width >= 1);
    ++stroke_draws;
    lv_draw_line(context, style, a, b);
}
#define lv_draw_rect observedRect
#define lv_draw_line observedLine
#ifdef STICK_FIGURE_BASELINE_HEADER
#include STICK_FIGURE_BASELINE_HEADER
#else
#include "stick_figure.h"
#endif
#undef lv_draw_rect
#undef lv_draw_line

LV_FONT_DECLARE(bodoni_48_smallcaps);
static constexpr int WIDTH = 480;
static constexpr int HEIGHT = 270;
static lv_color_t pixels[WIDTH * HEIGHT];
static lv_color_t draw_pixels[WIDTH * 16];
static unsigned checks = 0;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < WIDTH && area->y2 < HEIGHT);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * WIDTH + x] = *colors++;
    }
    lv_disp_flush_ready(driver);
}
static void beginFigure(StickFigure &figure, lv_obj_t *root, float scale) {
#ifdef STICK_FIGURE_BASELINE_HEADER
    assert(scale == 1.0f);
    figure.begin(root);
#else
    figure.begin(root, scale);
#endif
}
static float figureScale(const StickFigure &figure) {
#ifdef STICK_FIGURE_BASELINE_HEADER
    (void)figure;
    return 1.0f;
#else
    return figure.scale();
#endif
}

struct Scene {
    lv_obj_t *root;
    lv_obj_t *actor;
    OdometerDisplay price;
    StickFigure figure;
    uint32_t now = 0;
    bool has_price;
    bool center_price_for_golden = false;

    Scene(int width, int height, float scale, bool with_price = false, bool padded = false) : has_price(with_price) {
        root = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(root);
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_size(root, width, height);
        lv_obj_set_pos(root, padded ? 13 : 0, padded ? 9 : 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        if (padded) {
            lv_obj_set_style_pad_left(root, 5, 0);
            lv_obj_set_style_pad_right(root, 3, 0);
            lv_obj_set_style_pad_top(root, 4, 0);
            lv_obj_set_style_pad_bottom(root, 2, 0);
        }
        if (has_price) {
            price.begin(root, &bodoni_48_smallcaps);
            price.setValue("$4,123.75");
        }
        beginFigure(figure, root, scale);
        actor = lv_obj_get_child(root, -1);
        lv_obj_update_layout(root);
    }
    ~Scene() {
        price.setAnimationEnabled(false);
        if (root) lv_obj_del(root);
    }
    void bounds() {
        lv_area_t area;
        assert(figure.bounds(&area));
        assert(area.x1 >= 0 && area.y1 >= 0);
        assert(area.x2 < lv_obj_get_content_width(root));
        assert(area.y2 < lv_obj_get_content_height(root));
        ++checks;
    }
    void at(uint32_t timestamp) {
        lv_tick_inc(timestamp - now);
        now = timestamp;
        lv_timer_handler();
        lv_obj_update_layout(root);
        if (center_price_for_golden && price.numberLayer) {
            // The old golden frames used a centered price. Keep that fixture
            // while the other cases exercise this firmware's physical offsets.
            lv_obj_set_pos(price.numberLayer,
                           (lv_obj_get_content_width(root) - lv_obj_get_width(price.numberLayer)) / 2,
                           (lv_obj_get_content_height(root) - lv_obj_get_height(price.numberLayer)) / 2);
            lv_obj_update_layout(root);
        }
        lv_area_t number, digit;
        int direction = 0;
        bool has_number = has_price && price.numberBounds(&number);
        bool has_digit = has_price && price.interactionTarget(&digit, &direction);
        figure.update(now, has_number ? &number : nullptr, has_digit ? &digit : nullptr,
                      direction, has_price && price.isAnimating(), has_price ? price.valueChangeDirection() : 0);
        lv_obj_update_layout(root);
    }
    void step(unsigned milliseconds) {
        unsigned elapsed = 0;
        while (elapsed < milliseconds) {
            unsigned delta = std::min(32u, milliseconds - elapsed);
            at(now + delta);
            elapsed += delta;
        }
    }
    void manual(const lv_area_t &number, const lv_area_t &digit, bool animating, int roll, int change) {
        now += 32;
        lv_tick_inc(32);
        lv_timer_handler();
        lv_obj_update_layout(root);
        figure.update(now, &number, &digit, roll, animating, change);
        lv_obj_update_layout(root);
        bounds();
    }
    void render() {
        eye_draws = stroke_draws = 0;
        lv_obj_invalidate(root);
        lv_refr_now(nullptr);
    }
};

static bool gold(lv_color_t pixel) {
    return pixel.ch.red >= 8 && pixel.ch.red > pixel.ch.blue + 3;
}
struct Ink { int x1 = WIDTH, y1 = HEIGHT, x2 = -1, y2 = -1; unsigned count = 0; };
static Ink figureInk(Scene &scene) {
    Ink ink;
    lv_area_t content;
    lv_obj_get_content_coords(scene.root, &content);
    for (int y = std::max(0, static_cast<int>(content.y1)); y <= std::min(HEIGHT - 1, static_cast<int>(content.y2)); ++y) {
        for (int x = std::max(0, static_cast<int>(content.x1)); x <= std::min(WIDTH - 1, static_cast<int>(content.x2)); ++x) {
            if (!gold(pixels[y * WIDTH + x])) continue;
            ink.x1 = std::min(ink.x1, x);
            ink.y1 = std::min(ink.y1, y);
            ink.x2 = std::max(ink.x2, x);
            ink.y2 = std::max(ink.y2, y);
            ++ink.count;
        }
    }
    return ink;
}
static void checkInk(Scene &scene) {
    scene.render();
    assert(eye_draws > 0 && stroke_draws > 0);
    unsigned eye_pixels = 0;
    for (int y = std::max(0, static_cast<int>(last_eye.y1)); y <= std::min(HEIGHT - 1, static_cast<int>(last_eye.y2)); ++y) {
        for (int x = std::max(0, static_cast<int>(last_eye.x1)); x <= std::min(WIDTH - 1, static_cast<int>(last_eye.x2)); ++x) {
            if (gold(pixels[y * WIDTH + x])) ++eye_pixels;
        }
    }
    assert(eye_pixels > 0);
    Ink ink = figureInk(scene);
    assert(ink.count > 5);
    scene.bounds();
    ++checks;
}
static void checkPriceReadability(Scene &scene) {
    lv_area_t number, content;
    assert(scene.price.numberBounds(&number));
    lv_obj_get_content_coords(scene.root, &content);
    lv_obj_add_flag(scene.actor, LV_OBJ_FLAG_HIDDEN);
    scene.render();
    std::vector<lv_color_t> baseline(std::begin(pixels), std::end(pixels));
    lv_obj_clear_flag(scene.actor, LV_OBJ_FLAG_HIDDEN);
    scene.render();
    unsigned glyph_pixels = 0, modified = 0;
    for (int y = number.y1 + content.y1; y <= number.y2 + content.y1; ++y) {
        for (int x = number.x1 + content.x1; x <= number.x2 + content.x1; ++x) {
            unsigned index = y * WIDTH + x;
            if (baseline[index].full == lv_color_black().full) continue;
            ++glyph_pixels;
            if (baseline[index].full != pixels[index].full) ++modified;
        }
    }
    assert(glyph_pixels > 0 && modified * 100 <= glyph_pixels * 5);
    ++checks;
}
static uint64_t frameHash(int width, int height) {
    uint64_t hash = 1469598103934665603ULL;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) hash = (hash ^ pixels[y * WIDTH + x].full) * 1099511628211ULL;
    }
    return hash;
}

struct Golden { uint32_t time; uint64_t hash; int x1, y1, x2, y2, activity; };
// Recorded once with the unscaled header, through this same LVGL renderer.
static const Golden goldens[] = {
    {32, UINT64_C(2492325668011050506), 0, 86, 45, 133, 0},
    {320, UINT64_C(7799080554880790331), 9, 86, 54, 133, 0},
    {1024, UINT64_C(732670769666761843), 32, 86, 77, 133, 0},
    {6400, UINT64_C(171859193218348167), 204, 80, 239, 127, 0},
    {8512, UINT64_C(5822755719618349353), 204, 4, 239, 51, 0},
    {12000, UINT64_C(9092154520821666541), 204, 1, 239, 48, 1},
    {14016, UINT64_C(13532747635917423391), 110, 80, 155, 127, 1},
    {16032, UINT64_C(14607863643039018546), 24, 80, 69, 127, 1},
    {16256, UINT64_C(15385884109604674252), 43, 80, 88, 127, 1},
    {16608, UINT64_C(2370295566238017633), 68, 80, 113, 127, 3},
    {17088, UINT64_C(11207772434975902009), 68, 80, 113, 127, 3},
    {18656, UINT64_C(2152901840272153380), 118, 84, 163, 131, 5},
    {19232, UINT64_C(17305698163731744934), 118, 84, 163, 131, 5},
    {20992, UINT64_C(3000365219577711765), 125, 86, 170, 133, 0},
};
static void goldenFrames(bool print) {
    const uint32_t times[] = {32, 320, 1024, 6400, 8512, 12000, 14016, 16032, 16256, 16608, 17088, 18656, 19232, 20992};
    Scene scene(240, 135, 1.0f, true);
    scene.center_price_for_golden = true;
    unsigned next = 0;
    for (uint32_t time = 32; time <= times[sizeof(times) / sizeof(times[0]) - 1]; time += 32) {
        if (time == 16000) scene.price.setValue("$4,123.76");
        if (time == 18016) scene.price.setValue("$4,123.74");
        scene.at(time);
        if (time != times[next]) continue;
        scene.render();
        lv_area_t bounds;
        assert(scene.figure.bounds(&bounds));
        uint64_t hash = frameHash(240, 135);
        int activity = static_cast<int>(scene.figure.activity());
        if (print) {
            std::printf("{%u, UINT64_C(%llu), %d, %d, %d, %d, %d},\n", time,
                        static_cast<unsigned long long>(hash), bounds.x1, bounds.y1, bounds.x2, bounds.y2, activity);
        } else {
            assert(next < sizeof(goldens) / sizeof(goldens[0]));
            const Golden &expected = goldens[next];
            assert(expected.time == time && expected.hash == hash);
            assert(expected.x1 == bounds.x1 && expected.x2 == bounds.x2);
            assert(expected.y1 == bounds.y1 && expected.y2 == bounds.y2);
            assert(expected.activity == activity);
        }
        ++checks;
        if (++next == sizeof(times) / sizeof(times[0])) break;
    }
}

static void normalization() {
    struct Case { float supplied, expected; };
    const Case cases[] = {{0, 1}, {-2, 1}, {std::numeric_limits<float>::quiet_NaN(), 1},
        {std::numeric_limits<float>::infinity(), 1}, {-std::numeric_limits<float>::infinity(), 1},
        {.1f, .25f}, {9, 8}, {.5f, .5f}, {.75f, .75f}, {1, 1}, {1.5f, 1.5f}, {2, 2}, {2.234f, 2.234375f}};
    for (const Case &item : cases) {
        Scene scene(240, 135, item.supplied);
        assert(std::abs(figureScale(scene.figure) - item.expected) < .000001f);
        ++checks;
    }
}
static void fitting() {
    for (float scale : {.25f, .5f, 1.0f, 2.0f, 4.0f}) {
        int minimum = static_cast<int>(std::ceil(36 * scale));
        Scene scene(minimum, minimum, scale);
        scene.step(32);
        scene.bounds();
        scene.render();
        assert(figureInk(scene).count > 0);
        unsigned children = lv_obj_get_child_cnt(scene.root);
        lv_obj_set_width(scene.root, minimum - 1);
        scene.step(32);
        lv_area_t bounds;
        assert(!scene.figure.bounds(&bounds));
        assert(lv_obj_has_flag(scene.actor, LV_OBJ_FLAG_HIDDEN));
        scene.render();
        assert(eye_draws == 0 && figureInk(scene).count == 0);
        lv_obj_set_width(scene.root, minimum);
        lv_obj_set_height(scene.root, minimum - 1);
        scene.step(32);
        assert(!scene.figure.bounds(&bounds));
        lv_obj_set_height(scene.root, minimum);
        scene.step(32);
        scene.bounds();
        assert(lv_obj_get_child_cnt(scene.root) == children);
        ++checks;
    }
    Scene oversized(480, 270, 8.0f);
    oversized.step(32);
    lv_area_t bounds;
    assert(!oversized.figure.bounds(&bounds));
    oversized.render();
    assert(figureInk(oversized).count == 0);
}

static void scaledMotionAndInk() {
    int base_distance = 0, base_height = 0;
    for (float scale : {1.0f, .5f, .75f, 1.5f, 2.0f}) {
        Scene scene(240, 135, scale);
        scene.step(32);
        lv_area_t first;
        assert(scene.figure.bounds(&first));
        scene.step(992);
        lv_area_t last;
        assert(scene.figure.bounds(&last));
        int distance = last.x1 - first.x1;
        checkInk(scene);
        Ink ink = figureInk(scene);
        int body_height = ink.y2 - ink.y1 + 1;
        if (scale == 1) {
            base_distance = distance;
            base_height = body_height;
        } else {
            assert(std::abs(distance - base_distance * scale) <= 3);
            assert(std::abs(body_height - base_height * scale) <= 4);
        }
        assert(distance > 3);
        ++checks;
    }
    for (float scale : {3.0f, 4.0f}) {
        Scene scene(480, 270, scale);
        scene.step(320);
        checkInk(scene);
    }
}

static void interaction(float scale, int width, int height, bool padded) {
    Scene scene(width, height, scale, false, padded);
    int w = lv_obj_get_content_width(scene.root);
    int h = lv_obj_get_content_height(scene.root);
    lv_area_t number = {40, 40, static_cast<lv_coord_t>(w - 30), static_cast<lv_coord_t>(h / 2)};
    int center = w * 3 / 4;
    lv_area_t digit = {static_cast<lv_coord_t>(center - 10), 40,
                      static_cast<lv_coord_t>(center + 10), static_cast<lv_coord_t>(h / 2)};
    for (unsigned i = 0; i < 400 && scene.figure.activity() != StickFigure::Activity::Assist; ++i) {
        scene.manual(number, digit, true, 1, 1);
    }
    if (scene.figure.activity() != StickFigure::Activity::Assist) {
        std::fprintf(stderr, "Target not reached: scale %.2f, content %dx%d, digit [%d,%d,%d,%d], activity %d\n",
                     static_cast<double>(scale), w, h, digit.x1, digit.y1, digit.x2, digit.y2,
                     static_cast<int>(scene.figure.activity()));
    }
    assert(scene.figure.activity() == StickFigure::Activity::Assist);
    lv_area_t content;
    lv_obj_get_content_coords(scene.root, &content);
    bool contact = false;
    int radius = static_cast<int>(std::ceil(4 * scale)) + 1;
    for (unsigned i = 0; i < 24; ++i) {
        scene.manual(number, digit, true, 1, 1);
        scene.render();
        for (int y = std::max(0, digit.y2 + content.y1 + 1 - radius);
             y <= std::min(HEIGHT - 1, digit.y2 + content.y1 + 1 + radius); ++y) {
            for (int x = std::max(0, center + content.x1 - radius);
                 x <= std::min(WIDTH - 1, center + content.x1 + radius); ++x) {
                if (gold(pixels[y * WIDTH + x])) contact = true;
            }
        }
    }
    assert(contact); // Target coordinates stay in actual pixels, at every size.
    for (int change : {1, -1}) {
        scene.manual(number, digit, true, -change, change); // Negative prices can reverse reel direction.
        scene.manual(number, digit, false, 0, change);
        const auto mood = change < 0 ? StickFigure::Activity::Sad : StickFigure::Activity::Celebrate;
        assert(scene.figure.activity() == mood);
        uint32_t started = scene.now;
        unsigned duration = change < 0 ? 2100 : 1200;
        while (scene.now - started + 32 < duration) {
            scene.manual(number, digit, false, 0, change);
            assert(scene.figure.activity() == mood);
        }
        scene.manual(number, digit, false, 0, change);
        assert(scene.figure.activity() == StickFigure::Activity::Patrol);
        ++checks;
    }
    // A final non-rolling sign replacement overrides the ongoing direction.
    scene.manual(number, digit, true, 1, 1);
    scene.manual(number, digit, false, 0, -1);
    assert(scene.figure.activity() == StickFigure::Activity::Sad);
    scene.manual(number, digit, true, 1, -1);
    scene.manual(number, digit, false, 0, 1);
    assert(scene.figure.activity() == StickFigure::Activity::Celebrate);
    checkInk(scene);
}

static void pricesAndMemory(float scale) {
    Scene scene(448, 248, scale, true, true);
    scene.price.setAnimationEnabled(false);
    scene.price.setValue("-$100.00");
    scene.step(32);
    scene.price.setAnimationEnabled(true);
    scene.price.setValue("-$101.00");
    assert(scene.price.valueChangeDirection() == -1);
    scene.step(672);
    assert(!scene.price.isAnimating());
    assert(scene.figure.activity() == StickFigure::Activity::Sad);
    scene.price.setValue("-$99.00");
    scene.step(672);
    assert(scene.figure.activity() == StickFigure::Activity::Celebrate);
    scene.price.setValue("Error 500");
    scene.step(2400);
    scene.price.setValue("Error " + std::string(1023, 'x'));
    scene.step(320);
    assert(!scene.price.isAnimating());
    scene.price.setValue("$4,123.75");
    scene.step(320);
    checkInk(scene);
    checkPriceReadability(scene); // Larger figures use the larger display here.
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    unsigned children = lv_obj_get_child_cnt(scene.root);
    for (unsigned i = 0; i < 2000; ++i) {
        scene.step(32);
        scene.bounds();
        assert(lv_obj_get_child_cnt(scene.root) == children);
        if (i % 100 == 0) {
            scene.render();
            assert(lv_mem_test() == LV_RES_OK);
        }
    }
    lv_mem_monitor(&after);
    assert(after.used_cnt <= before.used_cnt + 1);
    assert(after.free_size + 128 >= before.free_size);
    scene.price.setAnimationEnabled(false);
    lv_obj_del(scene.root);
    scene.root = nullptr;
    lv_area_t bounds;
    assert(!scene.figure.bounds(&bounds));
    scene.figure.update(scene.now + 32, nullptr, nullptr, 0, false);
    assert(!scene.figure.bounds(&bounds));
    ++checks;
}

static void preview(const char *directory) {
    for (float scale : {.5f, .75f, 1.0f, 1.5f, 2.0f}) {
        Scene scene(240, 135, scale, true);
        lv_obj_t *label = lv_label_create(scene.root);
        char text[32];
        std::snprintf(text, sizeof(text), "%.2fx", static_cast<double>(scale));
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_pos(label, 6, 4);
        scene.step(640);
        scene.render();
        char path[4096];
        int length = std::snprintf(path, sizeof(path), "%s/scale_%03d.ppm", directory,
                                  static_cast<int>(std::round(scale * 100)));
        assert(length > 0 && static_cast<size_t>(length) < sizeof(path));
        std::ofstream output(path, std::ios::binary);
        output << "P6\n240 135\n255\n";
        for (int y = 0; y < 135; ++y) {
            for (int x = 0; x < 240; ++x) {
                lv_color32_t rgb;
                rgb.full = lv_color_to32(pixels[y * WIDTH + x]);
                char bytes[] = {static_cast<char>(rgb.ch.red), static_cast<char>(rgb.ch.green), static_cast<char>(rgb.ch.blue)};
                output.write(bytes, sizeof(bytes));
            }
        }
    }
}

int main(int argc, char **argv) {
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
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    if (argc > 1 && std::string(argv[1]) == "--golden") {
        goldenFrames(true);
        return 0;
    }
    goldenFrames(false);
    normalization();
    fitting();
    scaledMotionAndInk();
    for (float scale : {.5f, .75f, 1.0f, 1.5f, 2.0f}) {
        interaction(scale, 240, 135, false);
        interaction(scale, 448, 248, true);
        pricesAndMemory(scale);
    }
    assert(lv_mem_test() == LV_RES_OK);
    if (argc > 1) preview(argv[1]);
    std::printf("PASS %u scalable-figure raster, pixel-target, timing, bounds, 1x-golden and lifecycle checks\n", checks);
}
