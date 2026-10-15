// Regression coverage for the actual board geometry, font and figure scale.
#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <memory>
#include <vector>

#define private public
#include "odometer.h"
#include "stick_figure.h"
#undef private

LV_FONT_DECLARE(bodoni_72_smallcaps);
static constexpr int Width = 536, Height = 240;
static unsigned checks;

struct Display {
    std::vector<lv_color_t> pixels = std::vector<lv_color_t>(Width * Height);
    std::vector<lv_color_t> buffer = std::vector<lv_color_t>(Width * 16);
    lv_disp_draw_buf_t draw_buffer{};
    lv_disp_drv_t driver{};
    lv_disp_t *display;

    Display() {
        lv_disp_draw_buf_init(&draw_buffer, buffer.data(), nullptr, buffer.size());
        lv_disp_drv_init(&driver);
        driver.hor_res = Width;
        driver.ver_res = Height;
        driver.draw_buf = &draw_buffer;
        driver.user_data = this;
        driver.flush_cb = [](lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
            auto *self = static_cast<Display *>(driver->user_data);
            assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < Width && area->y2 < Height);
            for (int y = area->y1; y <= area->y2; ++y)
                for (int x = area->x1; x <= area->x2; ++x)
                    self->pixels[y * Width + x] = *colors++;
            lv_disp_flush_ready(driver);
        };
        display = lv_disp_drv_register(&driver);
        assert(display);
        lv_obj_remove_style_all(root());
        lv_obj_clear_flag(root(), LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(root(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root(), LV_OPA_COVER, 0);
    }
    ~Display() {
        lv_disp_remove(display);
        // LVGL 8.4 leaves the driver's software draw context for its owner to free.
        lv_mem_free(driver.draw_ctx);
        driver.draw_ctx = nullptr;
    }
    lv_obj_t *root() { return lv_disp_get_scr_act(display); }
    void render() {
        lv_obj_invalidate(root());
        lv_refr_now(display);
        assert(lv_mem_test() == LV_RES_OK);
    }
};

struct Fixture {
    Display display;
    OdometerDisplay price;
    StickFigure figure;
    uint32_t now = 0;

    explicit Fixture(const char *value = "$4,123.75", uint32_t seed = 359, bool fallback = false) {
        price.begin(display.root(), &bodoni_72_smallcaps);
        if (fallback) price.digitBitmapsReady = false;
        price.setValue(value);
        figure.begin(display.root(), 1.4f);
        figure.setRandomSeed(seed);
        tick();
        assert(figure.scaleQ8 == 358);
        assert(bodoni_72_smallcaps.line_height == 133);
    }
    ~Fixture() {
        price.setAnimationEnabled(false);
        figure.setEnabled(false);
        lv_obj_clean(display.root());
    }
    void tick() {
        now += 32;
        lv_tick_inc(32);
        lv_timer_handler();
        lv_obj_update_layout(display.root());
        lv_area_t number, digit, reels[StickScenes::MaxReels];
        int direction = 0;
        const bool has_number = price.numberBounds(&number);
        const bool has_digit = price.interactionTarget(&digit, &direction);
        const size_t count = price.digitBounds(reels, StickScenes::MaxReels);
        figure.update(now, has_number ? &number : nullptr, has_digit ? &digit : nullptr,
                      direction, price.isAnimating(), price.valueChangeDirection(), reels, count);
        lv_obj_update_layout(display.root());
        lv_area_t actor;
        assert(figure.bounds(&actor));
        assert(actor.x1 >= 0 && actor.y1 >= 0 && actor.x2 < Width && actor.y2 < Height);
        ++checks;
    }
    void awaitPlay(int id) {
        assert(figure.playAnimation(id));
        for (unsigned wait = 0; figure.scenePhase != StickFigure::ScenePhase::Play; wait += 32) {
            assert(wait < 12000);
            tick();
        }
        assert(figure.animationId() == id);
    }
    void startWalking(int from_x = 31, int to_x = 504) {
        figure.setAutoplay(true);
        figure.x = from_x * 256;
        figure.y = 230 * 256;
        figure.roamTargetX = to_x;
        figure.roamTargetY = 220;
        figure.roamSpeed = 40;
        figure.roamHasTarget = true;
        figure.roamPauseUntil = 0;
        // An expired deadline must wait for arrival, rather than interrupt.
        figure.nextIdleAt = now - 1;
    }
};


static void checkScreenAnchorsIgnorePrice() {
    Fixture narrow("$4,123.75"), wide("$116,235.67"), empty("");
    lv_area_t number;
    assert(narrow.price.numberBounds(&number));
    assert(number.x1 == 11 && number.y1 == 43 && number.x2 == 493 && number.y2 == 175);
    for (int id = 1; id <= ANIMATION_COUNT; ++id) {
        const auto space = narrow.figure.sceneSpace(id);
        for (int face : {-1, 1}) {
            for (const auto wanted : {lv_point_t{90, 80}, lv_point_t{268, 120},
                                      lv_point_t{440, 224}, lv_point_t{-100, 0},
                                      lv_point_t{636, 340}}) {
                const auto anchor = narrow.figure.chooseSceneAnchor(space, wanted.x, wanted.y, face);
                for (const auto *fixture : {&wide, &empty}) {
                    const auto other = fixture->figure.chooseSceneAnchor(space, wanted.x, wanted.y, face);
                    assert(other.x == anchor.x && other.y == anchor.y && other.face == anchor.face);
                }
                const int back = narrow.figure.scaled(anchor.face > 0 ? space.back : space.front);
                const int front = narrow.figure.scaled(anchor.face > 0 ? space.front : space.back);
                assert(anchor.x - back >= 0 && anchor.x + front < Width);
                assert(anchor.y - narrow.figure.scaled(space.above) >= 0);
                assert(anchor.y + narrow.figure.scaled(space.below) < Height);
                // The center is suitable even when it lies behind price glyphs.
                if (wanted.x == 268 && wanted.y == 120)
                    assert(anchor.x == wanted.x && anchor.y == wanted.y);
                ++checks;
            }
        }
    }
    // Local props must keep their nearest on-screen location, including an
    // actor initially inside the price footprint. No escape to the bottom lane.
    for (int id : {ANIMATION_CLEAN, ANIMATION_MINE, ANIMATION_BROOM, ANIMATION_SLEEP,
                   ANIMATION_VIDEO_GAMES, ANIMATION_UPSET, ANIMATION_COFFEE,
                   ANIMATION_COOK, ANIMATION_MUSIC, ANIMATION_DRAW, ANIMATION_PHONE}) {
        for (int face : {-1, 1}) {
            for (int y : {120, 224}) {
                narrow.figure.x = 268 * 256;
                narrow.figure.y = y * 256;
                narrow.figure.facing = face;
                narrow.figure.beginScene(id, true, narrow.now);
                assert(narrow.figure.sceneAnchorX == 268 && narrow.figure.sceneAnchorY == y);
                assert(narrow.figure.scenePhase == StickFigure::ScenePhase::Enter);
                ++checks;
            }
        }
    }
    const auto tall = narrow.figure.sceneSpace(ANIMATION_MUSIC);
    for (int face : {-1, 1}) {
        const auto left = narrow.figure.chooseSceneAnchor(tall, -100, 120, face);
        const auto right = narrow.figure.chooseSceneAnchor(tall, Width + 100, 120, face);
        assert(left.x == narrow.figure.scaled(tall.back));
        assert(right.x == Width - 1 - narrow.figure.scaled(tall.back));
        assert(left.face == 1 && right.face == -1);
        assert(left.y == 120 && right.y == 120);
        const auto away = narrow.figure.chooseSceneAnchor(tall, 268, 120, face, 268);
        assert(std::abs(away.x - 268) >= narrow.figure.scaled(38));
        ++checks;
    }
}

static lv_obj_t *addBackdrop(Fixture &scene) {
    auto *backdrop = lv_obj_create(scene.display.root());
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, Width, Height);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(backdrop, lv_color_hex(0xb24b1f), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_COVER, 0);
    lv_obj_move_background(backdrop);
    lv_obj_add_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN);
    return backdrop;
}

static unsigned counterPixels(const Fixture &scene, lv_obj_t *cell,
                              const std::vector<lv_color_t> &price_only,
                              const std::vector<lv_color_t> &composite) {
    lv_area_t area;
    assert(scene.price.relativeBounds(cell, &area));
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < Width && area.y2 < Height);
    const int width = area.x2 - area.x1 + 1, height = area.y2 - area.y1 + 1;
    std::vector<bool> outside(width * height);
    std::vector<int> flood;
    auto visit = [&](int x, int y) {
        const int index = y * width + x;
        if (!outside[index] && price_only[(area.y1 + y) * Width + area.x1 + x].full == lv_color_black().full) {
            outside[index] = true;
            flood.push_back(index);
        }
    };
    for (int x = 0; x < width; ++x) { visit(x, 0); visit(x, height - 1); }
    for (int y = 0; y < height; ++y) { visit(0, y); visit(width - 1, y); }
    for (size_t next = 0; next < flood.size(); ++next) {
        const int x = flood[next] % width, y = flood[next] / width;
        if (x) visit(x - 1, y);
        if (x + 1 < width) visit(x + 1, y);
        if (y) visit(x, y - 1);
        if (y + 1 < height) visit(x, y + 1);
    }
    unsigned holes = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int pixel = (area.y1 + y) * Width + area.x1 + x;
        if (!outside[y * width + x] && price_only[pixel].full == lv_color_black().full) {
            assert(composite[pixel].full == lv_color_hex(0xb24b1f).full);
            ++holes;
        }
    }
    return holes;
}

static void checkGlyphCompositingAndFallback() {
    Fixture cached("$8,008.00"), fallback("$8,008.00", 359, true);
    assert(cached.price.frameCacheReady && !fallback.price.frameCacheReady);
    lv_obj_add_flag(cached.figure.actor, LV_OBJ_FLAG_HIDDEN);
    cached.display.render();
    const auto price_only = cached.display.pixels;
    addBackdrop(cached);
    addBackdrop(fallback);
    cached.display.render();
    fallback.display.render();
    unsigned opaque = 0, antialiased = 0, transparent = 0;
    for (int pixel = 0; pixel < Width * Height; ++pixel) {
        const auto foreground = price_only[pixel];
        const auto composite = cached.display.pixels[pixel];
        // Ordinary LVGL label rendering is independent of the bitmap cache.
        assert(composite.full == fallback.display.pixels[pixel].full);
        if (foreground.full == lv_color_white().full) {
            assert(composite.full == lv_color_white().full);
            ++opaque;
        } else if (foreground.full == lv_color_black().full) {
            assert(composite.full == lv_color_hex(0xb24b1f).full);
            ++transparent;
        } else {
            assert(composite.full != foreground.full);
            ++antialiased;
        }
    }
    assert(opaque > 1000 && antialiased > 500 && transparent > 50000);
    unsigned dollar_gaps = 0, comma_gaps = 0, dot_gaps = 0, counters = 0;
    for (unsigned child = 0; child < lv_obj_get_child_cnt(cached.price.numberLayer); ++child) {
        auto *part = lv_obj_get_child(cached.price.numberLayer, child);
        if (lv_obj_get_child_cnt(part)) {
            counters += counterPixels(cached, part, price_only, cached.display.pixels);
            continue;
        }
        lv_area_t area;
        assert(cached.price.relativeBounds(part, &area));
        unsigned blanks = 0;
        for (int y = area.y1; y <= area.y2; ++y) for (int x = area.x1; x <= area.x2; ++x) {
            const int pixel = y * Width + x;
            if (price_only[pixel].full == lv_color_black().full) {
                assert(cached.display.pixels[pixel].full == lv_color_hex(0xb24b1f).full);
                ++blanks;
            }
        }
        const char *punctuation = lv_label_get_text(part);
        if (std::string(punctuation).find('$') != std::string::npos) dollar_gaps += blanks;
        if (std::string(punctuation).find(',') != std::string::npos) comma_gaps += blanks;
        if (std::string(punctuation).find('.') != std::string::npos) dot_gaps += blanks;
    }
    assert(dollar_gaps > 100 && comma_gaps > 100 && dot_gaps > 100 && counters > 100);
    cached.price.setValue("$8,009.11");
    fallback.price.setValue("$8,009.11");
    unsigned animated_frames = 0;
    for (unsigned elapsed = 0; elapsed < 4500; elapsed += 64) {
        cached.tick();
        fallback.tick();
        lv_obj_add_flag(cached.figure.actor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fallback.figure.actor, LV_OBJ_FLAG_HIDDEN);
        cached.display.render();
        fallback.display.render();
        assert(cached.price.isAnimating() && fallback.price.isAnimating());
        for (int pixel = 0; pixel < Width * Height; ++pixel)
            assert(cached.display.pixels[pixel].full == fallback.display.pixels[pixel].full);
        ++animated_frames;
        ++checks;
    }
    assert(animated_frames >= 70);
    std::printf("Native glyph composition: %u opaque, %u AA, %u counter pixels; punctuation gaps and %u animated cache/fallback frames.\n",
                opaque, antialiased, counters, animated_frames);
}

static void checkScenesVisibleThroughGlyphGaps() {
    unsigned visible_gaps = 0, aa_overlaps = 0, opaque_ink = 0;
    for (bool fallback : {false, true}) {
        for (int id : {ANIMATION_CLEAN, ANIMATION_SLEEP, ANIMATION_COFFEE, ANIMATION_MUSIC}) {
            for (int face : {-1, 1}) {
                Fixture scene("$8,008.00", 359, fallback);
                scene.figure.x = 268 * 256;
                scene.figure.y = 120 * 256;
                scene.figure.facing = face;
                scene.awaitPlay(id);
                assert(scene.figure.x == 268 * 256 && scene.figure.y == 120 * 256);
                scene.price.setValue("$8,009.11");
                unsigned scene_gaps = 0;
                for (unsigned elapsed = 0; elapsed < 4500; elapsed += 32) {
                    scene.tick();
                    if (elapsed % 256) continue;
                    lv_obj_add_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN);
                    scene.display.render();
                    const auto price_only = scene.display.pixels;
                    lv_obj_clear_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(scene.price.numberLayer, LV_OBJ_FLAG_HIDDEN);
                    scene.display.render();
                    const auto actor_only = scene.display.pixels;
                    lv_obj_clear_flag(scene.price.numberLayer, LV_OBJ_FLAG_HIDDEN);
                    scene.display.render();
                    const auto area = scene.figure.sceneNumber;
                    for (int y = area.y1; y <= area.y2; ++y) for (int x = area.x1; x <= area.x2; ++x) {
                        const int pixel = y * Width + x;
                        const auto foreground = price_only[pixel];
                        const auto background = actor_only[pixel];
                        const auto composite = scene.display.pixels[pixel];
                        if (foreground.full == lv_color_white().full) {
                            assert(composite.full == foreground.full);
                            ++opaque_ink;
                        } else if (foreground.full == lv_color_black().full && background.full != lv_color_black().full) {
                            assert(composite.full == background.full);
                            ++scene_gaps;
                        } else if (foreground.full != lv_color_black().full && background.full != lv_color_black().full) {
                            ++aa_overlaps;
                        }
                    }
                    ++checks;
                }
                assert(scene_gaps > 20);
                visible_gaps += scene_gaps;
            }
        }
    }
    assert(opaque_ink > 1000 && visible_gaps > 1000 && aa_overlaps > 20);
    std::printf("Native stick scenes behind glyphs: %u visible gap pixels, %u AA overlaps.\n", visible_gaps, aa_overlaps);
}

static std::vector<int> roamingTrace(const char *value, uint32_t seed) {
    Fixture scene(value, seed);
    scene.figure.setAutoplay(true);
    scene.figure.nextIdleAt = scene.now + 180000;
    std::vector<int> trace;
    std::set<int> columns;
    int top = Height, bottom = 0;
    unsigned arrivals = 0;
    for (unsigned elapsed = 0; elapsed < 120000; elapsed += 32) {
        const bool walking = scene.figure.roamHasTarget;
        scene.tick();
        assert(scene.figure.animationId() == ANIMATION_NONE);
        const int x = scene.figure.x / 256, y = scene.figure.y / 256;
        trace.push_back(scene.figure.x);
        trace.push_back(scene.figure.y);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
        columns.insert(std::min(2, x * 3 / Width));
        arrivals += walking && !scene.figure.roamHasTarget;
    }
    assert(top < 100 && bottom > 190 && columns.size() == 3 && arrivals >= 8);
    ++checks;
    return trace;
}

static void checkRoamingIgnoresPrice() {
    for (uint32_t seed : {1U, 37U, 359U}) {
        const auto empty = roamingTrace("", seed);
        assert(roamingTrace("$4,123.75", seed) == empty);
        assert(roamingTrace("$116,235.67", seed) == empty);
    }
}

static void checkIdleWalkingFinishes() {
    for (bool reverse : {false, true}) {
        Fixture scene;
        scene.startWalking(reverse ? 504 : 31, reverse ? 31 : 504);
        const int destination = scene.figure.roamTargetX;
        unsigned walking_ms = 0;
        while (scene.figure.roamHasTarget) {
            assert(walking_ms < 12000);
            scene.tick();
            assert(scene.figure.animationId() == ANIMATION_NONE);
            assert(scene.figure.roamTargetX == destination && scene.figure.roamTargetY == 220);
            walking_ms += 32;
        }
        assert(walking_ms > 6500); // Longer than the old maximum idle delay.
        assert(scene.figure.x == destination * 256 && scene.figure.y == 220 * 256);
        scene.tick();
        assert(scene.figure.animationId() != ANIMATION_NONE);
        assert(!scene.figure.manualScene);
        ++checks;
    }

    // Explicit user actions and numerical price changes still interrupt idle
    // walking immediately; only automatic idle performances wait for arrival.
    {
        Fixture scene;
        scene.startWalking();
        assert(scene.figure.playAnimation(ANIMATION_PHONE));
        scene.tick();
        assert(scene.figure.animationId() == ANIMATION_PHONE);
        assert(scene.figure.manualScene);
        assert(scene.figure.x != 504 * 256);
        ++checks;
    }
    for (int change : {-1, 1}) {
        Fixture scene;
        scene.startWalking();
        scene.figure.notifyPriceChange(change);
        scene.tick();
        assert(scene.figure.animationId() != ANIMATION_NONE);
        assert(animation_catalog_allowed(static_cast<animation_id_t>(scene.figure.animationId()),
               change > 0 ? ANIMATION_STATE_UP : ANIMATION_STATE_DOWN));
        assert(!scene.figure.manualScene && scene.figure.pendingPriceDirection == 0);
        assert(scene.figure.x != 504 * 256);
        ++checks;
    }
}

static void checkAutoplayRemainsLive() {
    std::set<int> all_scenes, starting_thirds;
    unsigned all_completions = 0;
    const int starts[] = {31, 268, 490};
    const uint32_t seeds[] = {1, 37, 359};
    for (unsigned run = 0; run < 3; ++run) {
        Fixture scene("$4,123.75", seeds[run]);
        scene.figure.setAutoplay(true);
        scene.figure.x = starts[run] * 256;
        scene.figure.y = 230 * 256;
        scene.figure.roamHasTarget = false;
        scene.figure.nextIdleAt = scene.now + 32;
        std::set<int> scenes;
        unsigned completions = 0;
        for (unsigned elapsed = 0; elapsed < 180000; elapsed += 32) {
            const int previous = scene.figure.animationId();
            const bool walking = scene.figure.roamHasTarget;
            const int destination_x = scene.figure.roamTargetX;
            const int destination_y = scene.figure.roamTargetY;
            const int previous_x = scene.figure.x / 256;
            scene.tick();
            if (!previous && walking && !scene.figure.roamHasTarget) {
                assert(scene.figure.animationId() == ANIMATION_NONE);
                assert(scene.figure.x == destination_x * 256 && scene.figure.y == destination_y * 256);
                ++completions;
            }
            if (!previous && scene.figure.animationId()) {
                assert(!walking);
                scenes.insert(scene.figure.animationId());
                starting_thirds.insert(std::min(2, previous_x * 3 / Width));
            }
        }
        assert(completions >= 5 && scenes.size() >= 5);
        all_completions += completions;
        all_scenes.insert(scenes.begin(), scenes.end());
        std::printf("Native autoplay seed %u: %u walking arrivals, %zu distinct scenes.\n",
                    seeds[run], completions, scenes.size());
    }
    assert(starting_thirds.size() == 3 && all_scenes.size() >= 8 && all_completions >= 15);
    ++checks;
}

int main() {
    lv_init();
    checkScreenAnchorsIgnorePrice();
    checkGlyphCompositingAndFallback();
    checkScenesVisibleThroughGlyphGaps();
    checkRoamingIgnoresPrice();
    checkIdleWalkingFinishes();
    checkAutoplayRemainsLive();
    std::printf("PASS %u native full-screen placement, glyph composition, complete idle walks and scheduling checks.\n", checks);
}
