#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "animation_catalog.h"
#include "odometer.h"
// Observe the actual fixed-point position, independently of the clipped draw
// rectangle. A selection must never reset the actor's screen position.
#define private public
#include "stick_figure.h"
#undef private

LV_FONT_DECLARE(bodoni_48_smallcaps);
static constexpr int WIDTH = 240, HEIGHT = 135;
static lv_color_t pixels[WIDTH * HEIGHT];
static lv_color_t draw_pixels[WIDTH * 16];
static uint32_t clock_ms;
static unsigned checks;
static const char *preview_directory;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < WIDTH && area->y2 < HEIGHT);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * WIDTH + x] = *colors++;
    lv_disp_flush_ready(driver);
}

static uint64_t frameHash() {
    uint64_t hash = 1469598103934665603ULL;
    for (auto pixel : pixels) hash = (hash ^ pixel.full) * 1099511628211ULL;
    return hash;
}

static unsigned coloredPixels() {
    unsigned count = 0;
    for (auto pixel : pixels) {
        lv_color32_t rgb;
        rgb.full = lv_color_to32(pixel);
        if (std::max({rgb.ch.red, rgb.ch.green, rgb.ch.blue}) -
            std::min({rgb.ch.red, rgb.ch.green, rgb.ch.blue}) > 24) ++count;
    }
    return count;
}

static void saveFrame(int id, int frame) {
    if (!preview_directory) return;
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/animation_%02d_%02d.ppm", preview_directory, id, frame);
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output << "P6\n" << WIDTH << ' ' << HEIGHT << "\n255\n";
    for (auto pixel : pixels) {
        lv_color32_t rgb;
        rgb.full = lv_color_to32(pixel);
        const char bytes[] = {static_cast<char>(rgb.ch.red), static_cast<char>(rgb.ch.green),
                              static_cast<char>(rgb.ch.blue)};
        output.write(bytes, sizeof(bytes));
    }
}

struct Scene {
    lv_obj_t *root;
    StickFigure figure;
    OdometerDisplay price;
    bool check_motion = true;

    explicit Scene(uint32_t seed = 1, float scale = 1.0f, bool padded = false,
                   const char *value = "$4,123.75") {
        root = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, WIDTH, HEIGHT);
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        if (padded) {
            lv_obj_set_pos(root, 11, 7);
            lv_obj_set_size(root, 216, 120);
            lv_obj_set_style_pad_left(root, 5, 0);
            lv_obj_set_style_pad_right(root, 3, 0);
            lv_obj_set_style_pad_top(root, 4, 0);
            lv_obj_set_style_pad_bottom(root, 2, 0);
        }
        price.begin(root, &bodoni_48_smallcaps);
        price.setValue(value);
        assert(price.valueChangeRevision() == 0); // Establishing the first price is idle.
        figure.begin(root, scale);
        figure.setRandomSeed(seed);
        tick();
    }
    ~Scene() {
        price.setAnimationEnabled(false);
        lv_obj_del(root);
        lv_area_t area;
        assert(!figure.bounds(&area));
        figure.update(clock_ms + 32, nullptr, nullptr, 0, false);
        ++checks;
    }
    void tick() {
        const int32_t old_x = figure.x, old_y = figure.y;
        const int old_scene = figure.animationId();
        const auto old_phase = figure.scenePhase;
        const uint32_t old_started = figure.sceneStarted;
        const uint32_t old_age = figure.poseTime - old_started;
        clock_ms += 32;
        lv_tick_inc(32);
        lv_timer_handler();
        lv_obj_update_layout(root);
        lv_area_t number, digit;
        int roll = 0;
        bool has_number = price.numberBounds(&number);
        bool has_digit = price.interactionTarget(&digit, &roll);
        lv_area_t reels[StickScenes::MaxReels];
        const size_t reel_count = price.digitBounds(reels, StickScenes::MaxReels);
        figure.update(clock_ms, has_number ? &number : nullptr,
                      has_digit ? &digit : nullptr, roll, price.isAnimating(),
                      price.valueChangeDirection(), reels, reel_count);
        if (figure.takeReelRequest() && !price.isAnimating()) price.replayAnimation();
        lv_obj_update_layout(root);
        if (check_motion) {
            int32_t limit = static_cast<int32_t>(8 * 256 * figure.scale());
            // Teleport is the only intentional discontinuity. It happens once,
            // immediately after the source body has completely faded away.
            const bool teleport_jump = old_scene == ANIMATION_TELEPORT &&
                old_phase == StickFigure::ScenePhase::Play &&
                figure.animationId() == old_scene && figure.sceneStarted == old_started &&
                old_age >= 3000 && old_age < 3400 && figure.poseTime - old_started >= 3400 &&
                (figure.x != old_x || figure.y != old_y);
            if (teleport_jump) {
                const auto destination = figure.teleportTarget;
                assert(old_x == figure.sceneAnchorX * 256 && old_y == figure.sceneAnchorY * 256);
                assert(figure.x == destination.x * 256 && figure.y == destination.y * 256);
            } else assert(std::abs(figure.x - old_x) <= limit);
            // The first initialization sets the ground coordinate once.
            if (old_y && !teleport_jump) assert(std::abs(figure.y - old_y) <= limit);
        }
        lv_area_t actor;
        assert(figure.bounds(&actor));
        assert(actor.x1 >= 0 && actor.y1 >= 0);
        assert(actor.x2 < lv_obj_get_content_width(root) && actor.y2 < lv_obj_get_content_height(root));
        assert(lv_obj_get_child_cnt(figure.actor) == 0);
        ++checks;
    }
    StickScenes::Detail::Point teleportDestination() const {
        StickScenes::Frame frame = {};
        frame.x = figure.sceneAnchorX;
        frame.y = figure.sceneAnchorY;
        frame.width = figure.width;
        frame.height = figure.height;
        frame.scaleQ8 = figure.scaleQ8;
        frame.seed = figure.sceneStarted;
        return StickScenes::Detail::teleportDestination(frame);
    }
    void step(unsigned ms) {
        for (unsigned elapsed = 0; elapsed < ms; elapsed += 32) tick();
    }
    void render() {
        lv_obj_invalidate(root);
        lv_refr_now(nullptr);
        assert(lv_mem_test() == LV_RES_OK);
    }
    void select(int id) {
        int32_t old_x = figure.x, old_y = figure.y;
        assert(figure.playAnimation(id));
        assert(figure.isSceneActive());
        assert(figure.x == old_x && figure.y == old_y);
        for (unsigned wait = 0; figure.animationId() != id && wait < 2500; wait += 32) tick();
        assert(figure.animationId() == id);
        for (unsigned wait = 0; figure.scenePhase != StickFigure::ScenePhase::Play && wait < 7000;
             wait += 32) tick();
        assert(figure.scenePhase == StickFigure::ScenePhase::Play);
        ++checks;
    }
    void setPrice(const char *text) {
        uint32_t before = price.valueChangeRevision();
        price.setValue(text);
        if (before != price.valueChangeRevision()) figure.notifyPriceChange(price.valueChangeDirection());
    }
};

static const unsigned expected_masks[ANIMATION_COUNT] = {
    1, 5, 5, 1, 2, 1, 2, 2, 1, 4, 1, 1, 1, 1, 1, 8,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 1, 4
};

static bool eligible(int id, int direction) {
    assert(id >= 1 && id <= ANIMATION_COUNT);
    unsigned mask = expected_masks[id - 1];
    return (mask & (direction == 0 ? 1 : direction > 0 ? 6 : 10)) != 0;
}

static void checkCatalog() {
    assert(!animation_catalog_lookup(ANIMATION_NONE));
    assert(!animation_catalog_lookup(static_cast<animation_id_t>(ANIMATION_COUNT + 1)));
    for (int id = 1; id <= ANIMATION_COUNT; ++id) {
        auto *info = animation_catalog_lookup(static_cast<animation_id_t>(id));
        assert(info && static_cast<int>(info->id) == id && info->name[0]);
        assert(info->allowed_states == expected_masks[id - 1]);
        for (int direction : {0, 1, -1}) {
            auto state = direction == 0 ? ANIMATION_STATE_IDLE :
                         direction > 0 ? ANIMATION_STATE_UP : ANIMATION_STATE_DOWN;
            assert(animation_catalog_allowed(static_cast<animation_id_t>(id), state) == eligible(id, direction));
            ++checks;
        }
    }
}

static void checkManualScenes() {
    Scene scene;
    scene.figure.setAutoplay(false);
    assert(!scene.figure.isSceneActive());
    assert(!scene.figure.playAnimation(0));
    assert(!scene.figure.playAnimation(-1));
    assert(!scene.figure.playAnimation(ANIMATION_COUNT + 1));
    assert(!scene.figure.isSceneActive());
    std::set<uint64_t> signatures;
    lv_mem_monitor_t before, after;
    scene.render();
    lv_mem_monitor(&before);
    unsigned child_count = lv_obj_get_child_cnt(scene.root);
    for (int id = 1; id <= ANIMATION_COUNT; ++id) {
        scene.select(id);
        std::set<uint64_t> frames;
        uint64_t signature = 1469598103934665603ULL;
        unsigned peak_ink = 0;
        const unsigned frame_ms = (animation_catalog[id - 1].duration_ms * 7 / 8 / 32 / 32) * 32;
        for (int frame = 0; frame < 32; ++frame) {
            scene.step(frame_ms);
            scene.render();
            peak_ink = std::max(peak_ink, coloredPixels());
            uint64_t hash = frameHash();
            frames.insert(hash);
            signature = (signature ^ hash) * 1099511628211ULL;
            saveFrame(id, frame);
            assert(scene.figure.animationId() == id);
            assert(lv_obj_get_child_cnt(scene.root) == child_count);
        }
        assert(peak_ink > 25);
        assert(frames.size() > 1); // Even a quiet newspaper scene must render motion.
        assert(signatures.insert(signature).second);
        std::printf("Animation %02d: %s, %zu distinct frames\n", id,
                    animation_catalog[id - 1].name, frames.size());
        ++checks;
    }
    for (unsigned wait = 0; scene.figure.isSceneActive() && wait < 20000; wait += 32) scene.tick();
    assert(!scene.figure.isSceneActive());
    scene.render();
    lv_mem_monitor(&after);
    assert(after.used_cnt <= before.used_cnt + 2);
    assert(after.free_size + 256 >= before.free_size);
    ++checks;
}

static std::vector<int> randomChoices() {
    std::vector<int> result;
    for (int direction : {0, 1, -1}) {
        std::set<int> choices;
        for (uint32_t seed = 1; seed <= 48; ++seed) {
            Scene scene(seed * 977);
            scene.figure.setAutoplay(true);
            if (direction) scene.figure.notifyPriceChange(direction);
            if (direction) scene.step(1600);
            else {
                // A due idle scene waits for the current walking leg to finish.
                for (unsigned wait = 0; !scene.figure.isSceneActive() && wait < 20000; wait += 32)
                    scene.tick();
            }
            int id = scene.figure.animationId();
            assert(scene.figure.isSceneActive());
            assert(eligible(id, direction));
            choices.insert(id);
            result.push_back(id);
            ++checks;
        }
        assert(choices.size() >= 2);
    }
    return result;
}

static void checkNotifications() {
    Scene scene(39);
    scene.figure.setAutoplay(true);
    scene.select(ANIMATION_NEWSPAPER);
    int id = scene.figure.animationId();
    auto phase = scene.figure.scenePhase;
    uint32_t revision = scene.price.valueChangeRevision();
    scene.figure.notifyPriceChange(0);
    scene.setPrice("$4,123.75");
    assert(scene.price.valueChangeRevision() == revision);
    scene.step(640);
    assert(scene.figure.animationId() == id && scene.figure.scenePhase == phase);
    scene.setPrice("$004123.750"); // Same numerical price with different formatting.
    assert(scene.price.valueChangeRevision() == revision);
    scene.step(640);
    assert(scene.figure.animationId() == id && scene.figure.scenePhase == phase);

    // A manual selection owns the stage, coalescing automatic price changes.
    scene.price.setAnimationEnabled(false);
    scene.setPrice("-$100.00");
    scene.step(640);
    assert(scene.figure.animationId() == id);
    scene.setPrice("-$99.00"); // Smaller magnitude, greater numerical value.
    scene.step(640);
    assert(scene.figure.animationId() == id);
    for (unsigned wait = 0; scene.figure.manualScene && wait < 20000; wait += 32) scene.tick();
    assert(!scene.figure.manualScene);
    scene.step(1600);
    assert(eligible(scene.figure.animationId(), 1));
    // Signed numerical changes are events even with all reel animation off.
    scene.setPrice("-$101.00");
    scene.step(1600);
    assert(eligible(scene.figure.animationId(), -1));

    // A same-magnitude sign flip has no roll but must choose an up scene.
    scene.price.setAnimationEnabled(true);
    scene.setPrice("$101.00");
    assert(!scene.price.isAnimating());
    scene.step(1600);
    assert(eligible(scene.figure.animationId(), 1));

    scene.select(ANIMATION_SLEEP);
    id = scene.figure.animationId();
    scene.step(640);
    assert(scene.figure.animationId() == id); // A stale last numeric direction is not an event.
    scene.setPrice("Service unavailable");
    scene.step(640);
    assert(scene.figure.animationId() == id);
    ++checks;
}

static void checkPartialFadeInterruption() {
    Scene scene;
    assert(scene.figure.playAnimation(ANIMATION_CLEAN));
    for (unsigned wait = 0; scene.figure.scenePhase != StickFigure::ScenePhase::Enter && wait < 7000;
         wait += 32) scene.tick();
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
    scene.step(128);
    uint32_t fade_age = scene.figure.poseTime - scene.figure.sceneSince;
    assert(fade_age < 450);
    int32_t old_x = scene.figure.x, old_y = scene.figure.y;
    assert(scene.figure.playAnimation(ANIMATION_MINE));
    assert(scene.figure.x == old_x && scene.figure.y == old_y);
    scene.tick();
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Exit);
    unsigned expected_opacity = (fade_age + 32) * 255 / 450;
    assert(scene.figure.exitStartOpacity == expected_opacity);
    assert(scene.figure.exitStartOpacity < LV_OPA_COVER);
    scene.render();
    scene.select(ANIMATION_MINE);
    ++checks;
}

static void checkAutomaticIdleNoChange() {
    Scene scene(123);
    scene.figure.setAutoplay(true);
    scene.price.setAnimationEnabled(false);
    scene.setPrice("$4,123.76");
    scene.step(1600);
    assert(eligible(scene.figure.animationId(), 1));
    for (unsigned wait = 0; scene.figure.isSceneActive() && wait < 20000; wait += 32) scene.tick();
    assert(!scene.figure.isSceneActive());
    // The idle deadline may expire before the current walking leg finishes.
    for (unsigned wait = 0; !scene.figure.isSceneActive() && wait < 20000; wait += 32) scene.tick();
    assert(scene.figure.isSceneActive());
    for (unsigned wait = 0; scene.figure.scenePhase != StickFigure::ScenePhase::Play && wait < 7000;
         wait += 32) scene.tick();
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
    int id = scene.figure.animationId();
    assert(eligible(id, 0) && !scene.figure.manualScene);
    uint32_t revision = scene.price.valueChangeRevision();
    assert(scene.price.valueChangeDirection() == 1); // Intentionally still cached.
    scene.setPrice("$4,123.76");
    scene.figure.notifyPriceChange(0);
    scene.step(640);
    assert(scene.price.valueChangeRevision() == revision);
    assert(scene.figure.animationId() == id && scene.figure.scenePhase == StickFigure::ScenePhase::Play);
    scene.setPrice("$004123.760");
    scene.step(640);
    assert(scene.price.valueChangeRevision() == revision);
    assert(scene.figure.animationId() == id && scene.figure.scenePhase == StickFigure::ScenePhase::Play);
    ++checks;
}

static void checkCleanupAndClockWrap() {
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    for (unsigned i = 0; i < 80; ++i) {
        Scene scene(i + 1);
        scene.figure.setAutoplay(true);
        scene.figure.playAnimation(i % ANIMATION_COUNT + 1);
        scene.step(320);
        scene.render();
    }
    lv_mem_monitor(&after);
    assert(after.used_cnt <= before.used_cnt + 1);
    assert(after.free_size + 128 >= before.free_size);
    Scene scene;
    scene.select(ANIMATION_FLY);
    clock_ms = UINT32_MAX - 96;
    scene.check_motion = false; // The deliberate timestamp discontinuity is not a frame.
    scene.tick();
    scene.check_motion = true;
    scene.step(320);
    assert(clock_ms < 1000);
    scene.render();
    ++checks;
}

static void checkReelReplay() {
    Scene scene;
    uint32_t revision = scene.price.valueChangeRevision();
    scene.price.replayAnimation();
    assert(scene.price.valueChangeRevision() == revision);
#if ODOMETER_ANIMATION_DURATION_MS > 0
    assert(scene.price.isAnimating());
    scene.step(ODOMETER_ANIMATION_DURATION_MS + 128);
    assert(!scene.price.isAnimating());
#else
    assert(!scene.price.isAnimating());
#endif
    scene.setPrice("$4,123.75");
    assert(scene.price.valueChangeRevision() == revision);
    scene.price.setAnimationEnabled(false);
    scene.price.replayAnimation();
    assert(!scene.price.isAnimating());
    assert(scene.price.valueChangeRevision() == revision);
    scene.price.setAnimationEnabled(true);
    scene.setPrice("Loading");
    scene.price.replayAnimation();
    assert(!scene.price.isAnimating());
    ++checks;
}

static void checkScaledScenes() {
    for (float scale : {0.5f, 2.0f}) {
        Scene scene(73, scale, scale == 0.5f);
        scene.render();
        unsigned children = lv_obj_get_child_cnt(scene.root);
        lv_mem_monitor_t before, after;
        lv_mem_monitor(&before);
        for (int id = 1; id <= ANIMATION_COUNT; ++id) {
            scene.select(id);
            unsigned peak = 0;
            for (unsigned frame = 0; frame < 4; ++frame) {
                scene.step(animation_catalog[id - 1].duration_ms / 6);
                scene.render();
                assert(scene.figure.animationId() == id);
                peak = std::max(peak, coloredPixels());
                assert(lv_obj_get_child_cnt(scene.root) == children);
            }
            assert(peak > 8);
            ++checks;
        }
        scene.price.setAnimationEnabled(false);
        for (unsigned wait = 0; scene.figure.isSceneActive() && wait < 20000; wait += 32) scene.tick();
        assert(!scene.figure.isSceneActive());
        scene.render();
        lv_mem_monitor(&after);
        assert(after.used_cnt <= before.used_cnt + 2);
        assert(after.free_size + 256 >= before.free_size);
        std::printf("Animation repertoire at %.1fx: all %d scenes rendered%s\n", scale, ANIMATION_COUNT,
                    scale == 0.5f ? " on a padded root" : "");
    }
}

static void setStandingPosition(Scene &scene, int point) {
    // Fractional positions catch a subtle snap that whole-pixel anchors hide.
    const int left = scene.figure.leftEdge(), right = scene.figure.rightEdge();
    const int top = scene.figure.topLane(), bottom = scene.figure.bottomLane();
    const int positions[][2] = {
        {left + 1, top + 2},
        {(left + right) / 2, (top + bottom) / 2},
        {right - 1, bottom - 1},
    };
    scene.figure.x = positions[point][0] * 256 + 91;
    scene.figure.y = positions[point][1] * 256 + 173;
    scene.figure.positionActor();
}

static void checkMovingScenesStartHere() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (int point = 0; point < 3; ++point) {
            for (int facing : {-1, 1}) {
                for (int id : {ANIMATION_DRIVE, ANIMATION_NARUTO, ANIMATION_FLY}) {
                    Scene scene(31, scale, scale == 0.5f, "");
                    setStandingPosition(scene, point);
                    scene.figure.facing = facing;
                    const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
                    assert(scene.figure.playAnimation(id));
                    scene.tick();
                    assert(scene.figure.animationId() == id);
                    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                    assert(scene.figure.x == start_x && scene.figure.y == start_y);
                    for (unsigned wait = 0; scene.figure.scenePhase == StickFigure::ScenePhase::Enter;
                         wait += 32) {
                        assert(wait < 512);
                        scene.tick();
                        assert(scene.figure.x == start_x && scene.figure.y == start_y);
                    }
                    std::set<int> directions;
                    bool moved = false;
                    for (unsigned elapsed = 0; elapsed < 6000; elapsed += 32) {
                        scene.tick();
                        assert(scene.figure.animationId() == id);
                        if (id != ANIMATION_FLY) assert(scene.figure.y == start_y);
                        assert(scene.figure.x >= scene.figure.leftEdge() * 256);
                        assert(scene.figure.x <= scene.figure.rightEdge() * 256);
                        moved |= scene.figure.x != start_x;
                        directions.insert(scene.figure.facing);
                    }
                    assert(moved);
                    if (id != ANIMATION_FLY) assert(directions.size() == 2);
                    scene.render();
                    ++checks;
                }
            }
        }
    }
}

static void checkStationaryScenesStartHere() {
    const int local_scenes[] = {ANIMATION_SHADOW_BOX, ANIMATION_COINS, ANIMATION_SMOKE,
                              ANIMATION_PHONE, ANIMATION_CELEBRATE};
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (int point = 0; point < 3; ++point) {
            for (int id : local_scenes) {
                Scene scene(67, scale, scale == 0.5f, "");
                setStandingPosition(scene, point);
                const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
                assert(scene.figure.playAnimation(id));
                scene.tick();
                assert(scene.figure.animationId() == id);
                assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.step(2048);
                assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.render();
                ++checks;
            }
        }
    }
}

static void checkLocalPropsUseAvailableRoom() {
    const int local_scenes[] = {
        ANIMATION_CLEAN, ANIMATION_MINE, ANIMATION_PAN, ANIMATION_BROOM,
        ANIMATION_DOG, ANIMATION_CAT, ANIMATION_SLEEP, ANIMATION_VIDEO_GAMES,
        ANIMATION_UPSET, ANIMATION_COFFEE, ANIMATION_COOK, ANIMATION_MUSIC,
        ANIMATION_DRAW, ANIMATION_PLANTS, ANIMATION_NEWSPAPER,
        ANIMATION_TREASURE,
    };
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (int facing : {-1, 1}) {
            for (int id : local_scenes) {
                Scene scene(73, scale, scale == 0.5f, "");
                // This off-center standing point has room for each prop;
                // an inward-facing pose can fit even when the initial one cannot.
                if (id == ANIMATION_DOG) {
                    // Fetch now uses a wide forward field. The native suite
                    // covers it at 1.4x; this old compact fixture must fit the
                    // field before asserting an entrance at the standing point.
                    const int width = lv_obj_get_content_width(scene.root);
                    if (width < scene.figure.scaled(163) + 2) continue;
                    scene.figure.x = (facing > 0 ? scene.figure.scaled(14) :
                        width - 1 - scene.figure.scaled(14)) * 256 + 91;
                } else scene.figure.x = scene.figure.scaled(42) * 256 + 91;
                scene.figure.y = scene.figure.scaled(55) * 256 + 173;
                scene.figure.facing = facing;
                scene.figure.positionActor();
                const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
                assert(scene.figure.playAnimation(id));
                scene.tick();
                assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.step(512);
                assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.render();
                ++checks;
            }
        }
    }
}

static void checkLocalSceneInterruptionAndResume() {
    Scene scene(91, 1.0f, false, "");
    scene.figure.setAutoplay(true);
    setStandingPosition(scene, 1);
    scene.select(ANIMATION_NARUTO);
    scene.step(1024);
    const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
    assert(scene.figure.playAnimation(ANIMATION_COINS));
    for (unsigned wait = 0; scene.figure.animationId() != ANIMATION_COINS; wait += 32) {
        assert(wait < 1024);
        scene.tick();
        assert(scene.figure.x == start_x && scene.figure.y == start_y);
    }
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
    scene.step(1024);
    assert(scene.figure.x == start_x && scene.figure.y == start_y);

    scene.figure.setEnabled(false);
    clock_ms += 10000;
    lv_tick_inc(10000);
    scene.figure.update(clock_ms, nullptr, nullptr, 0, false);
    assert(scene.figure.x == start_x && scene.figure.y == start_y);
    assert(!scene.figure.isSceneActive());
    scene.figure.setEnabled(true);
    assert(scene.figure.playAnimation(ANIMATION_COINS));
    scene.tick();
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
    assert(scene.figure.x == start_x && scene.figure.y == start_y);
    scene.render();
    ++checks;
}

static void checkTreasureScheduling() {
    const auto *info = animation_catalog_lookup(ANIMATION_TREASURE);
    assert(info && info->duration_ms == 12000);
    assert(info->allowed_states == ANIMATION_STATE_UP);
    bool selected_for_increase = false;
    for (int direction : {0, 1, -1}) {
        for (uint32_t seed = 1; seed <= 96; ++seed) {
            Scene scene(seed * 977);
            scene.figure.setAutoplay(true);
            if (direction) scene.figure.notifyPriceChange(direction);
            else scene.figure.nextIdleAt = clock_ms + 32;
            scene.tick();
            const int id = scene.figure.animationId();
            assert(id && eligible(id, direction));
            assert(!scene.figure.manualScene);
            if (id == ANIMATION_TREASURE) {
                assert(direction > 0);
                selected_for_increase = true;
            }
            ++checks;
        }
    }
    assert(selected_for_increase);
    ++checks;
}

static void checkTreasureLifecycle() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (int facing : {-1, 1}) {
                Scene scene(263, scale, padded);
                setStandingPosition(scene, 1);
                scene.figure.facing = facing;
                const unsigned children = lv_obj_get_child_cnt(scene.root);
                lv_mem_monitor_t before, after;
                scene.render();
                lv_mem_monitor(&before);
                scene.select(ANIMATION_TREASURE);
                const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
                std::set<uint64_t> frames;
                for (unsigned age : {800U, 2200U, 4000U, 6200U, 8600U, 10200U, 11600U}) {
                    while (scene.figure.poseTime - scene.figure.sceneStarted < age) scene.tick();
                    assert(scene.figure.animationId() == ANIMATION_TREASURE);
                    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
                    assert(scene.figure.x == start_x && scene.figure.y == start_y);
                    scene.render();
                    assert(coloredPixels() > 8);
                    assert(lv_obj_get_child_cnt(scene.root) == children);
                    frames.insert(frameHash());
                }
                assert(frames.size() >= 5);
                while (scene.figure.isSceneActive()) {
                    scene.tick();
                    assert(scene.figure.poseTime - scene.figure.sceneStarted < 13000);
                    assert(scene.figure.x == start_x && scene.figure.y == start_y);
                }
                assert(scene.figure.animationId() == ANIMATION_NONE);
                scene.render();
                lv_mem_monitor(&after);
                assert(lv_obj_get_child_cnt(scene.root) == children);
                assert(after.used_cnt <= before.used_cnt + 2);
                assert(after.free_size + 256 >= before.free_size);
                ++checks;
            }
        }
    }

    // Cancelling during entry, digging, or the reveal must remove every prop
    // and release the scene, including an immediate disable/re-enable cycle.
    for (unsigned age : {128U, 6200U, 10500U}) {
        for (bool disable : {false, true}) {
            // Exact in-place replacement is expected on an unrestricted canvas.
            Scene scene(269, 1.0f, false, "");
            scene.figure.setAutoplay(true);
            setStandingPosition(scene, 1);
            assert(scene.figure.playAnimation(ANIMATION_TREASURE));
            while (scene.figure.scenePhase != StickFigure::ScenePhase::Enter) scene.tick();
            scene.step(age);
            const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
            const unsigned children = lv_obj_get_child_cnt(scene.root);
            if (disable) {
                scene.figure.setEnabled(false);
                assert(!scene.figure.isSceneActive());
                assert(scene.figure.animationId() == ANIMATION_NONE);
                assert(!scene.figure.playAnimation(ANIMATION_TREASURE));
                clock_ms += 512;
                lv_tick_inc(512);
                scene.figure.update(clock_ms, nullptr, nullptr, 0, false);
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.render();
                scene.figure.setEnabled(true);
            }
            assert(scene.figure.playAnimation(ANIMATION_COINS));
            for (unsigned wait = 0; scene.figure.animationId() != ANIMATION_COINS; wait += 32) {
                assert(wait < 1024);
                scene.tick();
                assert(scene.figure.x == start_x && scene.figure.y == start_y);
                scene.render();
            }
            scene.step(512);
            assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
            assert(scene.figure.x == start_x && scene.figure.y == start_y);
            assert(lv_obj_get_child_cnt(scene.root) == children);
            scene.render();
            ++checks;
        }
    }
}

static void advanceTeleportTo(Scene &scene, uint32_t age) {
    assert(scene.figure.animationId() == ANIMATION_TELEPORT);
    while (scene.figure.poseTime - scene.figure.sceneStarted < age) scene.tick();
    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
}

static void checkOneWayTeleport() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (int facing : {-1, 1}) {
                Scene scene(131, scale, padded, "");
                scene.figure.setAutoplay(true);
                scene.select(ANIMATION_TELEPORT);
                scene.figure.facing = facing;
                const int32_t source_x = scene.figure.x, source_y = scene.figure.y;
                const auto destination = scene.teleportDestination();
                const int32_t destination_x = destination.x * 256;
                const int32_t destination_y = destination.y * 256;
                assert(destination_x != source_x);
                unsigned jumps = 0;
                while (scene.figure.isSceneActive()) {
                    const int32_t old_x = scene.figure.x, old_y = scene.figure.y;
                    scene.tick();
                    if (scene.figure.x != old_x || scene.figure.y != old_y) ++jumps;
                    const uint32_t age = scene.figure.poseTime - scene.figure.sceneStarted;
                    assert(age < 11000);
                    assert(scene.figure.x == (age < 3400 ? source_x : destination_x));
                    assert(scene.figure.y == (age < 3400 ? source_y : destination_y));
                    if (age % 1024 < 32) scene.render();
                }
                assert(jumps == 1);
                assert(scene.figure.x == destination_x && scene.figure.y == destination_y);
                // The actor's actual coordinates, rather than only its painted
                // silhouette, must continue naturally from the arrival portal.
                scene.tick();
                assert(std::abs(scene.figure.x - destination_x) <= scene.figure.scaled(8) * 256);
                assert(std::abs(scene.figure.y - destination_y) <= scene.figure.scaled(8) * 256);
                scene.step(1024);
                assert(!scene.figure.isSceneActive());
                assert(scene.figure.x != destination_x || scene.figure.y != destination_y);
                const int32_t standing_x = scene.figure.x, standing_y = scene.figure.y;
                assert(scene.figure.playAnimation(ANIMATION_COINS));
                scene.tick();
                assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                assert(scene.figure.x == standing_x && scene.figure.y == standing_y);
                scene.render();
                ++checks;
            }
        }
    }
}

static void checkTeleportInterruptionAndSwitches() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (int facing : {-1, 1}) {
                for (uint32_t age : {2400U, 3200U, 3392U, 3450U, 6500U, 9600U}) {
                    for (bool disable : {false, true}) {
                        Scene scene(197, scale, padded, "");
                        scene.select(ANIMATION_TELEPORT);
                        scene.figure.facing = facing;
                        advanceTeleportTo(scene, age);
                        const auto destination = scene.teleportDestination();
                        const uint32_t current_age = scene.figure.poseTime - scene.figure.sceneStarted;
                        int32_t expected_x = scene.figure.x, expected_y = scene.figure.y;
                        if (disable) {
                            // Disabling takes effect immediately, including
                            // while the source is invisible before departure.
                            scene.figure.setEnabled(false);
                            clock_ms += 10000;
                            lv_tick_inc(10000);
                            scene.figure.update(clock_ms, nullptr, nullptr, 0, false);
                            assert(scene.figure.x == expected_x && scene.figure.y == expected_y);
                            assert(!scene.figure.isSceneActive());
                            scene.figure.setEnabled(true);
                        } else if (current_age < 3400 && current_age + 32 >= 3400) {
                            // A queued replacement is consumed on the next
                            // update, after advancing to that frame's time.
                            expected_x = destination.x * 256;
                            expected_y = destination.y * 256;
                        }
                        assert(scene.figure.playAnimation(ANIMATION_COINS));
                        for (unsigned wait = 0; scene.figure.animationId() != ANIMATION_COINS;
                             wait += 32) {
                            assert(wait < 1024);
                            scene.tick();
                            assert(scene.figure.x == expected_x && scene.figure.y == expected_y);
                            scene.render();
                        }
                        assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                        scene.step(512);
                        assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
                        assert(scene.figure.x == expected_x && scene.figure.y == expected_y);
                        scene.render();
                        ++checks;
                    }
                }
            }
        }
    }
}

static void checkTeleportSkippedUpdates() {
    const uint32_t skips[][2] = {{128, 3500}, {128, 11000}, {2400, 6500}, {2400, 11000}};
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (const auto &skip : skips) {
                Scene scene(223, scale, padded, "");
                assert(scene.figure.playAnimation(ANIMATION_TELEPORT));
                while (scene.figure.scenePhase != StickFigure::ScenePhase::Enter) scene.tick();
                scene.step(skip[0]);
                const auto destination = scene.teleportDestination();
                const uint32_t arrival_time = scene.figure.sceneStarted + skip[1];
                // A stalled application can skip over both the invisible
                // interval and the scene deadline in one update. Time-limited
                // ordinary movement must not prevent committing the arrival.
                const uint32_t skipped_time = arrival_time - clock_ms - 32;
                clock_ms += skipped_time;
                lv_tick_inc(skipped_time);
                scene.check_motion = false; // This deliberate time gap is not an ordinary frame.
                scene.tick();
                scene.check_motion = true;
                assert(scene.figure.x == destination.x * 256);
                assert(scene.figure.y == destination.y * 256);
                scene.render();
                if (skip[1] > 10000) {
                    while (scene.figure.isSceneActive()) {
                        scene.tick();
                        assert(scene.figure.x == destination.x * 256);
                        assert(scene.figure.y == destination.y * 256);
                    }
                }
                ++checks;
            }
        }
    }
}

static std::vector<int32_t> roamingTrace(uint32_t seed, float scale, bool padded,
                                       const char *value = "") {
    Scene scene(seed, scale, padded, value);
    scene.figure.setAutoplay(true);
    // Isolate free movement from the repertoire's independent idle scheduler.
    scene.figure.nextIdleAt = clock_ms + 180000;
    const int32_t left = scene.figure.scaled(18) * 256;
    const int32_t right = (scene.figure.width - scene.figure.scaled(19)) * 256;
    const int32_t top = scene.figure.scaled(38) * 256;
    const int32_t bottom = scene.figure.bottomLane() * 256;
    int32_t min_x = right, max_x = left, min_y = bottom, max_y = top;
    unsigned interior_frames = 0, diagonal_frames = 0;
    std::set<int> cells;
    std::vector<int32_t> trace;
    for (unsigned elapsed = 0; elapsed < 120000; elapsed += 32) {
        const int32_t old_x = scene.figure.x, old_y = scene.figure.y;
        scene.tick();
        assert(!scene.figure.isSceneActive());
        const int32_t x = scene.figure.x, y = scene.figure.y;
        assert(x >= scene.figure.leftEdge() * 256 && x <= scene.figure.rightEdge() * 256);
        assert(y >= scene.figure.topLane() * 256 && y <= bottom);
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        if (x > left + (right - left) / 3 && x < right - (right - left) / 3 &&
            y > top + (bottom - top) / 3 && y < bottom - (bottom - top) / 3)
            ++interior_frames;
        if (x != old_x && y != old_y) ++diagonal_frames;
        cells.insert(std::clamp(static_cast<int>((x - left) * 3 / (right - left + 1)), 0, 2) +
                     std::clamp(static_cast<int>((y - top) * 3 / (bottom - top + 1)), 0, 2) * 3);
        if (elapsed % 256 == 0) {
            trace.push_back(x);
            trace.push_back(y);
        }
    }
    assert(max_x - min_x > (right - left) * 3 / 4);
    assert(max_y - min_y > (bottom - top) * 3 / 4);
    assert(interior_frames > 30 && diagonal_frames > 100);
    assert(cells.size() >= 8);
    scene.render();
    ++checks;
    return trace;
}

static void checkFreeRoaming() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        std::vector<int32_t> previous;
        for (uint32_t seed : {17U, 977U, 0x91e10da5U}) {
            const auto first = roamingTrace(seed, scale, scale == 0.5f);
            const auto second = roamingTrace(seed, scale, scale == 0.5f);
            assert(first == second);
            // Price geometry, including the status label's full-width box,
            // must not restrict wandering to lanes around the text.
            for (const char *value : {"$4,123.75", "$123456789.75", "Connecting..."})
                assert(first == roamingTrace(seed, scale, scale == 0.5f, value));
            assert(previous.empty() || first != previous);
            previous = first;
            ++checks;
        }
    }
}

// Compare actual LVGL pixels with the actor hidden at the very same clock
// tick. Ordinary scenes pass behind opaque glyphs and remain visible through
// their gaps, including rolling digits and rebuilt numeric or text labels.
static unsigned checkPriceGlyphs(Scene &scene, const lv_area_t *gaps_in = nullptr) {
    assert(lv_obj_get_child(scene.root, 0) == scene.figure.actor);
    lv_obj_add_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN);
    scene.render();
    const std::vector<lv_color_t> baseline(pixels, pixels + WIDTH * HEIGHT);
    unsigned glyph_pixels = 0;
    for (const auto pixel : baseline) glyph_pixels += pixel.full == lv_color_white().full;
    assert(glyph_pixels > 20);
    lv_obj_clear_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN);
    scene.render();
    unsigned colored_gaps = 0;
    lv_area_t number, content;
    if (scene.price.numberBounds(&number)) {
        lv_obj_get_content_coords(scene.root, &content);
        for (int y = std::max(0, content.y1 + number.y1);
             y <= std::min(HEIGHT - 1, content.y1 + number.y2); ++y) {
            for (int x = std::max(0, content.x1 + number.x1);
                 x <= std::min(WIDTH - 1, content.x1 + number.x2); ++x) {
                const unsigned i = y * WIDTH + x;
                if (baseline[i].full == lv_color_white().full)
                    assert(pixels[i].full == baseline[i].full);
                if (baseline[i].full == lv_color_black().full &&
                    (!gaps_in || (x >= gaps_in->x1 && x <= gaps_in->x2 &&
                                  y >= gaps_in->y1 && y <= gaps_in->y2))) {
                    lv_color32_t rgb;
                    rgb.full = lv_color_to32(pixels[i]);
                    colored_gaps += std::max({rgb.ch.red, rgb.ch.green, rgb.ch.blue}) -
                                    std::min({rgb.ch.red, rgb.ch.green, rgb.ch.blue}) > 24;
                }
            }
        }
    } else {
        for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
            if (baseline[i].full == lv_color_white().full)
                assert(pixels[i].full == baseline[i].full);
    }
    ++checks;
    return colored_gaps;
}

static lv_area_t localFootprint(const StickFigure &figure,
                                const StickFigure::SceneSpace &space,
                                int x, int y, int face);
static bool overlaps(const lv_area_t &one, const lv_area_t &two);

static void checkFlyingVisibleThroughPrice(Scene &scene) {
    lv_area_t number;
    assert(scene.price.numberBounds(&number));
    const int32_t old_x = scene.figure.x, old_y = scene.figure.y;
    // Put the flying pose inside the actual rebuilt price view to distinguish
    // glyph occlusion from a rectangular mask.
    scene.figure.x = (number.x1 + number.x2) / 2 * 256;
    scene.figure.y = std::clamp((number.y1 + number.y2) / 2 + scene.figure.scaled(12),
                               scene.figure.topLane(), scene.figure.bottomLane()) * 256;
    scene.figure.positionActor();
    assert(checkPriceGlyphs(scene) > 0);
    scene.figure.x = old_x; scene.figure.y = old_y;
    scene.figure.positionActor();
}

static unsigned checkOrdinaryPoseThroughPrice(Scene &scene) {
    lv_area_t number;
    assert(scene.price.numberBounds(&number));
    const int32_t old_x = scene.figure.x, old_y = scene.figure.y;
    const int32_t old_anchor_x = scene.figure.sceneAnchorX;
    const int32_t old_anchor_y = scene.figure.sceneAnchorY;
    unsigned gaps = 0;
    for (int point : {1, 2, 3}) {
        const int x = std::clamp(number.x1 + (number.x2 - number.x1) * point / 4,
                                 scene.figure.leftEdge(), scene.figure.rightEdge());
        const int y = std::clamp((number.y1 + number.y2) / 2 + scene.figure.scaled(12),
                                 scene.figure.topLane(), scene.figure.bottomLane());
        scene.figure.x = x * 256;
        scene.figure.y = y * 256;
        // Teleport draws its departure pose from the saved source anchor.
        scene.figure.sceneAnchorX = x;
        scene.figure.sceneAnchorY = y;
        scene.figure.positionActor();
        gaps += checkPriceGlyphs(scene);
    }
    scene.figure.x = old_x; scene.figure.y = old_y;
    scene.figure.sceneAnchorX = old_anchor_x;
    scene.figure.sceneAnchorY = old_anchor_y;
    scene.figure.positionActor();
    return gaps;
}

static void checkPunctuationBackgrounds() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            Scene scene(397, scale, padded);
            scene.select(ANIMATION_FLY);
            // Metadata children remain available when the cached renderer
            // hides them. Check each punctuation cell itself, rather than
            // allowing visible gaps under some other digit to hide a defect.
            lv_obj_t *layer = lv_obj_get_child(scene.root, -1);
            std::set<std::string> symbols;
            for (unsigned index = 0; index < lv_obj_get_child_cnt(layer); ++index) {
                lv_obj_t *label = lv_obj_get_child(layer, index);
                if (!lv_obj_check_type(label, &lv_label_class)) continue;
                const std::string symbol = lv_label_get_text(label);
                if (symbol != "$" && symbol != "," && symbol != ".") continue;
                symbols.insert(symbol);
                lv_area_t cell, content;
                lv_obj_get_coords(label, &cell);
                lv_obj_get_content_coords(scene.root, &content);
                unsigned gaps = 0;
                for (int side : {-1, 0, 1}) {
                    scene.figure.x = std::clamp((cell.x1 + cell.x2) / 2 - content.x1 +
                                               scene.figure.scaled(side * 3),
                                               scene.figure.leftEdge(), scene.figure.rightEdge()) * 256;
                    scene.figure.y = std::clamp((cell.y1 + cell.y2) / 2 - content.y1 +
                                               scene.figure.scaled(12),
                                               scene.figure.topLane(), scene.figure.bottomLane()) * 256;
                    scene.figure.positionActor();
                    gaps += checkPriceGlyphs(scene, &cell);
                }
                if (!gaps)
                    std::fprintf(stderr, "Opaque punctuation background: symbol=%s scale=%.1f padded=%d\n",
                                 symbol.c_str(), scale, padded);
                assert(gaps > 0);
                ++checks;
            }
            assert(symbols == std::set<std::string>({"$", ",", "."}));
        }
    }
}

static void checkFlyStartsInPlace() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (int point = 0; point < 3; ++point) {
                for (int face : {-1, 1}) {
                    Scene scene(379, scale, padded);
                    setStandingPosition(scene, point);
                    scene.figure.facing = face;
                    const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
                    assert(scene.figure.playAnimation(ANIMATION_FLY));
                    scene.tick();
                    assert(scene.figure.animationId() == ANIMATION_FLY);
                    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                    const unsigned entrance_gaps = checkPriceGlyphs(scene);
                    if (point == 1) assert(entrance_gaps > 0);
                    do {
                        assert(scene.figure.x == start_x && scene.figure.y == start_y);
                        scene.tick();
                    } while (scene.figure.scenePhase == StickFigure::ScenePhase::Enter);
                    assert(scene.figure.scenePhase == StickFigure::ScenePhase::Play);
                    assert(scene.figure.x == start_x && scene.figure.y == start_y);
                    checkPriceGlyphs(scene);
                    ++checks;
                }
            }
        }
    }
}

static std::vector<int32_t> flyingTrace(float scale, bool padded, const char *value) {
    Scene scene(383, scale, padded, value);
    setStandingPosition(scene, 1);
    const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
    scene.select(ANIMATION_FLY);
    assert(scene.figure.x == start_x && scene.figure.y == start_y);
    const bool has_price = value[0];
    unsigned colored_gaps = 0, crossings = 0;
    bool saw_exit = false;
    std::vector<int32_t> trace;
    for (unsigned elapsed = 0; scene.figure.isSceneActive(); elapsed += 32) {
        assert(elapsed < 11000);
        scene.tick();
        trace.push_back(scene.figure.x);
        trace.push_back(scene.figure.y);
        trace.push_back(static_cast<int>(scene.figure.scenePhase));
        if (!scene.figure.isSceneActive()) break;
        saw_exit |= scene.figure.scenePhase == StickFigure::ScenePhase::Exit;
        if (has_price) {
            lv_area_t number;
            assert(scene.price.numberBounds(&number));
            const auto area = localFootprint(scene.figure, StickFigure::sceneSpace(ANIMATION_FLY),
                                             scene.figure.x / 256, scene.figure.y / 256,
                                             scene.figure.facing);
            crossings += overlaps(area, number);
            if (elapsed % 256 == 0) colored_gaps += checkPriceGlyphs(scene);
        }
    }
    assert(scene.figure.animationId() == ANIMATION_NONE && saw_exit);
    if (has_price) assert(crossings && colored_gaps);
    ++checks;
    return trace;
}

static void checkFlyBehindNumbers() {
    checkFlyStartsInPlace();
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            // A price must neither divert the flight into a narrow corridor
            // nor change its start, movement, exit, or completion coordinates.
            const auto with_price = flyingTrace(scale, padded, "$4,123.75");
            const auto without_price = flyingTrace(scale, padded, "");
            assert(with_price == without_price);

            Scene scene(389, scale, padded);
            scene.select(ANIMATION_FLY);
            unsigned view = 0;
            for (const char *value : {"$9,876.50", "$4,123.75", "$123456789.75", "Connecting..."}) {
                scene.price.setAnimationEnabled(true);
                scene.setPrice(value);
                scene.tick();
                for (unsigned advance : {0U, 256U}) {
                    scene.step(advance);
                    assert(scene.figure.animationId() == ANIMATION_FLY);
                    checkFlyingVisibleThroughPrice(scene);
                }
                if (view++ == 0) {
                    assert(scene.price.isAnimating() == (ODOMETER_ANIMATION_DURATION_MS > 0));
                    scene.step(ODOMETER_ANIMATION_DURATION_MS + 64);
                    assert(!scene.price.isAnimating());
                    assert(scene.figure.animationId() == ANIMATION_FLY);
                    checkFlyingVisibleThroughPrice(scene);
                }
            }
            lv_area_t number;
            assert(scene.price.numberBounds(&number));
            scene.figure.x = (number.x1 + number.x2) / 2 * 256;
            scene.figure.y = std::clamp((number.y1 + number.y2) / 2 + scene.figure.scaled(12),
                                       scene.figure.topLane(), scene.figure.bottomLane()) * 256;
            scene.figure.positionActor();
            assert(scene.figure.playAnimation(ANIMATION_NARUTO));
            scene.tick();
            assert(scene.figure.animationId() == ANIMATION_FLY);
            assert(scene.figure.scenePhase == StickFigure::ScenePhase::Exit);
            assert(checkPriceGlyphs(scene) > 0);
            scene.step(128);
            assert(scene.figure.scenePhase == StickFigure::ScenePhase::Exit);
            assert(checkPriceGlyphs(scene) > 0);
            ++checks;
        }
    }
}

static lv_area_t localFootprint(const StickFigure &figure,
                                const StickFigure::SceneSpace &space,
                                int x, int y, int face) {
    return {static_cast<lv_coord_t>(x - figure.scaled(face > 0 ? space.back : space.front)),
            static_cast<lv_coord_t>(y - figure.scaled(space.above)),
            static_cast<lv_coord_t>(x + figure.scaled(face > 0 ? space.front : space.back)),
            static_cast<lv_coord_t>(y + figure.scaled(space.below))};
}

static bool overlaps(const lv_area_t &one, const lv_area_t &two) {
    return one.x1 <= two.x2 && one.x2 >= two.x1 &&
           one.y1 <= two.y2 && one.y2 >= two.y1;
}

static bool hasScreenStage(const Scene &scene, const StickFigure::SceneSpace &space) {
    // Check both orientations independently of the placement heuristic. Some
    // props cannot fit on this compact fixture at 2x; those still need bounded
    // actor movement and correct price layering rather than a fictional lane.
    const int top = std::max(scene.figure.topLane(), scene.figure.scaled(space.above));
    const int bottom = std::min(scene.figure.bottomLane(),
                                scene.figure.height - 1 - scene.figure.scaled(space.below));
    if (top > bottom) return false;
    for (int face : {-1, 1}) {
        const int left = std::max(scene.figure.leftEdge(),
                                 scene.figure.scaled(face > 0 ? space.back : space.front));
        const int right = std::min(scene.figure.rightEdge(), scene.figure.width - 1 -
                                  scene.figure.scaled(face > 0 ? space.front : space.back));
        if (left <= right) return true;
    }
    return false;
}

static std::vector<int32_t> localStagePosition(int id, float scale, bool padded,
                                              int initial_face, const char *value,
                                              unsigned &moved, unsigned &fits,
                                              unsigned &constrained) {
    Scene scene(331, scale, padded, value);
    setStandingPosition(scene, 1);
    scene.figure.facing = initial_face;
    scene.figure.positionActor();
    const int32_t start_x = scene.figure.x, start_y = scene.figure.y;
    const auto space = StickFigure::sceneSpace(id);
    const bool room = hasScreenStage(scene, space);
    scene.select(id); // Bounded frame movement also applies throughout approach.
    const auto area = localFootprint(scene.figure, space, scene.figure.x / 256,
                                     scene.figure.y / 256, scene.figure.facing);
    if (room) {
        assert(area.x1 >= 0 && area.y1 >= 0);
        assert(area.x2 < scene.figure.width && area.y2 < scene.figure.height);
        ++fits;
    } else ++constrained;
    moved += scene.figure.x != start_x || scene.figure.y != start_y;
    scene.render();
    if (value[0]) checkPriceGlyphs(scene);
    ++checks;
    return {scene.figure.x, scene.figure.y, scene.figure.facing};
}

static void checkLocalScenesUseFullScreen() {
    unsigned moved = 0, fits = 0, constrained = 0;
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (int initial_face : {-1, 1}) {
                for (int id = 1; id <= ANIMATION_COUNT; ++id) {
                    StickFigure::SceneSpace space;
                    if (!StickFigure::localSceneSpace(id, space)) continue;
                    const auto empty = localStagePosition(id, scale, padded, initial_face, "",
                                                          moved, fits, constrained);
                    for (const char *value : {"$4,123.75", "Connecting..."})
                        assert(empty == localStagePosition(id, scale, padded, initial_face, value,
                                                           moved, fits, constrained));
                }
            }
        }
    }
    assert(moved && fits && constrained);
}

static void checkRoamingBehindPrice() {
    unsigned crossings = 0, colored_gaps = 0;
    const StickFigure::SceneSpace walking = {13, 13, 30, 3};
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            for (uint32_t seed : {353U, 359U}) {
                Scene scene(seed, scale, padded);
                scene.figure.setAutoplay(true);
                scene.figure.nextIdleAt = clock_ms + 30000;
                lv_area_t number;
                assert(scene.price.numberBounds(&number));
                bool moved = false, late_movement = false;
                const int32_t initial_x = scene.figure.x, initial_y = scene.figure.y;
                for (unsigned age = 0; age < 10000; age += 32) {
                    const int32_t old_x = scene.figure.x, old_y = scene.figure.y;
                    scene.tick();
                    assert(!scene.figure.isSceneActive());
                    moved |= scene.figure.x != initial_x || scene.figure.y != initial_y;
                    if (age > 5000) late_movement |= scene.figure.x != old_x || scene.figure.y != old_y;
                    const auto area = localFootprint(scene.figure, walking, scene.figure.x / 256,
                                                     scene.figure.y / 256, scene.figure.facing);
                    crossings += overlaps(area, number);
                    if (age % 256 == 0) colored_gaps += checkPriceGlyphs(scene);
                }
                assert(moved && late_movement);
                ++checks;
            }
        }
    }
    assert(crossings && colored_gaps);
}

static void checkTeleportBehindPrice() {
    unsigned crossings = 0;
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (bool padded : {false, true}) {
            Scene scene(367, scale, padded);
            scene.select(ANIMATION_TELEPORT);
            const auto target = scene.figure.teleportTarget;
            const auto expected = scene.teleportDestination();
            assert(target.x == expected.x && target.y == expected.y);
            lv_area_t number;
            assert(scene.price.numberBounds(&number));
            const auto space = StickFigure::sceneSpace(ANIMATION_TELEPORT);
            const auto source = localFootprint(scene.figure, space, scene.figure.sceneAnchorX,
                                               scene.figure.sceneAnchorY, scene.figure.facing);
            const auto arrival = localFootprint(scene.figure, space, target.x, target.y,
                                                scene.figure.facing);
            if (hasScreenStage(scene, space)) {
                assert(source.x1 >= 0 && source.y1 >= 0);
                assert(source.x2 < scene.figure.width && source.y2 < scene.figure.height);
                assert(arrival.x1 >= 0 && arrival.y1 >= 0);
                assert(arrival.x2 < scene.figure.width && arrival.y2 < scene.figure.height);
            }
            crossings += overlaps(source, number) || overlaps(arrival, number);
            while (scene.figure.isSceneActive()) {
                scene.tick();
                if (scene.figure.poseTime - scene.figure.sceneStarted >= 3400)
                    assert(scene.figure.x == target.x * 256 && scene.figure.y == target.y * 256);
                assert(scene.figure.poseTime - scene.figure.sceneStarted < 11000);
                if ((scene.figure.poseTime - scene.figure.sceneStarted) % 2048 < 32)
                    checkPriceGlyphs(scene);
            }
            ++checks;
        }
    }
    assert(crossings);
}

static void checkIntentionalPriceInteractions() {
    for (int id : {ANIMATION_PUNCH, ANIMATION_CRANK, ANIMATION_SLOT_MACHINE, ANIMATION_CHILL}) {
        Scene scene(373);
        scene.select(id);
        assert(lv_obj_get_child(scene.root, 0) == scene.figure.actor);
        // Punching, cranking and the slot machine still trigger reel motion,
        // while every figure and prop respects the foreground glyph strokes.
        assert(scene.price.isAnimating() ==
               (id != ANIMATION_CHILL && ODOMETER_ANIMATION_DURATION_MS > 0));
        // Place the interacting figure across the number; protecting glyph
        // cores must not hide its gestures behind a rectangular backdrop.
        lv_area_t number;
        assert(scene.price.numberBounds(&number));
        scene.figure.x = (number.x1 + number.x2) / 2 * 256;
        scene.figure.y = number.y2 * 256;
        scene.figure.positionActor();
        assert(checkPriceGlyphs(scene) > 0);
        ++checks;
    }
}

// All scenes, including deliberate number interactions, travel behind glyph
// strokes and remain visible through their gaps.
static void checkPriceLayering() {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        Scene scene(311, scale, scale == 0.5f);
        for (int id = 1; id <= ANIMATION_COUNT; ++id) {
            scene.select(id);
            scene.step(512);
            checkPriceGlyphs(scene);
            const unsigned gaps = checkOrdinaryPoseThroughPrice(scene);
            if (!gaps)
                std::fprintf(stderr, "Scene clipped by price rectangle: id=%d scale=%.1f\n", id, scale);
            assert(gaps > 0);
            // Price labels and digit viewports are recreated both when a roll
            // begins and ends. Neither rebuild may put animations on top.
            scene.setPrice(id % 2 ? "$9,876.50" : "$4,123.75");
            scene.tick();
            checkPriceGlyphs(scene);
            assert(checkOrdinaryPoseThroughPrice(scene) > 0);
            scene.step(256);
            checkPriceGlyphs(scene);
            assert(checkOrdinaryPoseThroughPrice(scene) > 0);
        }
        // The same drawing order also protects the compact numeric fallback
        // and plain status messages when number geometry is unavailable.
        for (const char *value : {"$123456789.75", "Connecting..."}) {
            scene.price.setAnimationEnabled(false);
            scene.setPrice(value);
            scene.tick();
            checkPriceGlyphs(scene);
            assert(checkOrdinaryPoseThroughPrice(scene) > 0);
        }
    }
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
    lv_refr_now(nullptr);
    if (argc > 1 && std::string(argv[1]) == "--idle-only") {
        preview_directory = nullptr;
        checkAutomaticIdleNoChange();
        std::printf("Automatic idle/no-change: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--roaming-only") {
        preview_directory = nullptr;
        checkMovingScenesStartHere();
        checkStationaryScenesStartHere();
        checkLocalPropsUseAvailableRoom();
        checkLocalSceneInterruptionAndResume();
        checkFreeRoaming();
        std::printf("Animation placement and free roaming: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--teleport-only") {
        preview_directory = nullptr;
        checkOneWayTeleport();
        checkTeleportInterruptionAndSwitches();
        checkTeleportSkippedUpdates();
        std::printf("One-way teleport: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--price-only") {
        preview_directory = nullptr;
        checkRoamingBehindPrice();
        checkPriceLayering();
        checkPunctuationBackgrounds();
        checkLocalScenesUseFullScreen();
        checkTeleportBehindPrice();
        checkIntentionalPriceInteractions();
        checkFlyBehindNumbers();
        std::printf("Price legibility and animation placement: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--fly-only") {
        preview_directory = nullptr;
        checkFlyBehindNumbers();
        std::printf("Flying behind price glyphs: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--treasure-only") {
        preview_directory = nullptr;
        checkCatalog();
        checkTreasureScheduling();
        checkTreasureLifecycle();
        checkLocalPropsUseAvailableRoom();
        std::printf("Treasure animation: %u checks passed, roll duration %d ms\n",
                    checks, ODOMETER_ANIMATION_DURATION_MS);
        return 0;
    }
    checkPriceLayering();
    checkPunctuationBackgrounds();
    checkLocalScenesUseFullScreen();
    checkRoamingBehindPrice();
    checkTeleportBehindPrice();
    checkIntentionalPriceInteractions();
    checkFlyBehindNumbers();
    checkCatalog();
    checkTreasureScheduling();
    checkTreasureLifecycle();
    checkOneWayTeleport();
    checkTeleportInterruptionAndSwitches();
    checkTeleportSkippedUpdates();
    checkMovingScenesStartHere();
    checkStationaryScenesStartHere();
    checkLocalPropsUseAvailableRoom();
    checkLocalSceneInterruptionAndResume();
    checkFreeRoaming();
    checkManualScenes();
    auto first = randomChoices();
    auto second = randomChoices();
    assert(first == second);
    checkNotifications();
    checkAutomaticIdleNoChange();
    checkPartialFadeInterruption();
    checkReelReplay();
    checkCleanupAndClockWrap();
    checkScaledScenes();
    std::printf("Animation repertoire: %u checks passed, roll duration %d ms\n",
                checks, ODOMETER_ANIMATION_DURATION_MS);
}
