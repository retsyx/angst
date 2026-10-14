#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Observe scene phases and coordinates, and ensure disabling retains the
// actual price rather than just producing a similarly shaped rendered image.
#define private public
#include "odometer.h"
#include "stick_figure.h"
#undef private

LV_FONT_DECLARE(bodoni_48_smallcaps);
static constexpr int WIDTH = 240, HEIGHT = 135;
static lv_color_t pixels[WIDTH * HEIGHT];
static lv_color_t draw_pixels[WIDTH * 16];
static uint32_t clock_ms;
static unsigned checks;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < WIDTH && area->y2 < HEIGHT);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * WIDTH + x] = *colors++;
    lv_disp_flush_ready(driver);
}

static std::vector<uint16_t> frame() {
    std::vector<uint16_t> result;
    result.reserve(WIDTH * HEIGHT);
    for (auto pixel : pixels) result.push_back(pixel.full);
    return result;
}

struct Scene {
    lv_obj_t *root;
    StickFigure figure;
    OdometerDisplay price;

    explicit Scene(bool autoplay = true) {
        root = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, WIDTH, HEIGHT);
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        price.begin(root, &bodoni_48_smallcaps);
        price.setValue("$4,123.75");
        figure.begin(root);
        figure.setRandomSeed(39);
        figure.setAutoplay(autoplay);
        tick();
        assert(figure.enabled());
    }

    ~Scene() {
        price.setAnimationEnabled(false);
        lv_obj_del(root);
        figure.setEnabled(false);
        figure.update(clock_ms + 32, nullptr, nullptr, 0, false);
        lv_area_t bounds;
        assert(!figure.bounds(&bounds));
    }

    void tick() {
        clock_ms += 32;
        lv_tick_inc(32);
        lv_timer_handler();
        lv_obj_update_layout(root);
        lv_area_t number, digit;
        int direction = 0;
        bool has_number = price.numberBounds(&number);
        bool has_digit = price.interactionTarget(&digit, &direction);
        lv_area_t reels[StickScenes::MaxReels];
        const size_t reel_count = price.digitBounds(reels, StickScenes::MaxReels);
        figure.update(clock_ms, has_number ? &number : nullptr,
                      has_digit ? &digit : nullptr, direction, price.isAnimating(),
                      price.valueChangeDirection(), reels, reel_count);
        lv_obj_update_layout(root);
        assert(lv_mem_test() == LV_RES_OK);
    }

    void step(unsigned ms) {
        for (unsigned elapsed = 0; elapsed < ms; elapsed += 32) tick();
    }

    void render() {
        lv_obj_invalidate(root);
        lv_refr_now(nullptr);
        assert(lv_mem_test() == LV_RES_OK);
    }

    std::vector<uint16_t> priceOnlyFrame() {
        bool hidden = lv_obj_has_flag(figure.actor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(figure.actor, LV_OBJ_FLAG_HIDDEN);
        render();
        auto expected = frame();
        if (!hidden) lv_obj_clear_flag(figure.actor, LV_OBJ_FLAG_HIDDEN);
        render();
        return expected;
    }

    void setPrice(const char *text) {
        auto revision = price.valueChangeRevision();
        price.setValue(text);
        if (revision != price.valueChangeRevision())
            figure.notifyPriceChange(price.valueChangeDirection());
    }

    void awaitPhase(StickFigure::ScenePhase phase) {
        for (unsigned wait = 0; figure.scenePhase != phase && wait < 20000; wait += 32) tick();
        assert(figure.scenePhase == phase);
    }

    void applySwitches(bool animation, bool stickFigure) {
        // Match the GUI loop's effective state from a single settings snapshot.
        price.setAnimationEnabled(animation);
        figure.setEnabled(animation && stickFigure);
    }
};

static void assertDisabled(Scene &scene) {
    auto &figure = scene.figure;
    assert(!figure.enabled());
    assert(lv_obj_has_flag(figure.actor, LV_OBJ_FLAG_HIDDEN));
    lv_area_t bounds;
    assert(!figure.bounds(&bounds));
    assert(!figure.isSceneActive());
    assert(figure.animationId() == 0);
    assert(!figure.takeReelRequest());
    assert(!figure.playAnimation(ANIMATION_CLEAN));
    ++checks;
}

static void checkScenePhase(StickFigure::ScenePhase phase) {
    Scene scene;
    scene.price.setAnimationEnabled(false);
    // Cleaning can now start in place. Use a staged group scene when the
    // switch test specifically needs to interrupt an approach walk.
    assert(scene.figure.playAnimation(phase == StickFigure::ScenePhase::Approach
                                         ? ANIMATION_SWORD_FIGHT : ANIMATION_CLEAN));
    scene.awaitPhase(phase);
    // Enter and exit must be tested with visible props partway through fading.
    if (phase == StickFigure::ScenePhase::Enter || phase == StickFigure::ScenePhase::Exit)
        scene.step(128);
    assert(scene.figure.scenePhase == phase);
    scene.render();
    auto visible = frame();
    auto expected = scene.priceOnlyFrame();
    assert(visible != expected);
    if (phase != StickFigure::ScenePhase::Approach) assert(scene.figure.sceneCanvas);
    int32_t x = scene.figure.x, y = scene.figure.y;
    auto revision = scene.price.valueChangeRevision();
    unsigned children = lv_obj_get_child_cnt(scene.root);

    // Leave both a manual replacement and an automatic price reaction pending.
    assert(scene.figure.playAnimation(ANIMATION_MINE));
    scene.figure.notifyPriceChange(-1);
    scene.figure.setEnabled(false);
    assertDisabled(scene);
    scene.render();
    assert(frame() == expected); // Includes every pixel formerly occupied by props.
    assert(scene.figure.x == x && scene.figure.y == y);
    assert(scene.figure.autoplay); // Visibility does not discard the autoplay preference.
    assert(scene.price.currentText == "$4,123.75");
    assert(scene.price.valueChangeRevision() == revision);
    assert(lv_obj_get_child_cnt(scene.root) == children);
    assert(lv_obj_get_child_cnt(scene.figure.actor) == 0);

    scene.figure.notifyPriceChange(1); // Events received while off must also be ignored.
    scene.step(10000);
    assertDisabled(scene);
    scene.render();
    assert(frame() == expected);
    assert(scene.figure.x == x && scene.figure.y == y);
    scene.figure.setEnabled(true);
    assert(scene.figure.enabled());
    assert(scene.figure.x == x && scene.figure.y == y);
    scene.tick();
    assert(!scene.figure.isSceneActive());
    assert(!scene.figure.takeReelRequest());
    assert(scene.figure.activity() == StickFigure::Activity::Patrol);
    assert(std::abs(scene.figure.x - x) <= 8 * 256);
    assert(std::abs(scene.figure.y - y) <= 8 * 256);
    assert(!lv_obj_has_flag(scene.figure.actor, LV_OBJ_FLAG_HIDDEN));
    scene.step(2304);
    assert(!scene.figure.isSceneActive()); // Fresh idle delay after the long disabled gap.
    for (unsigned wait = 0; !scene.figure.isSceneActive() && wait < 7000; wait += 32) scene.tick();
    assert(scene.figure.isSceneActive());
    assert(!scene.figure.manualScene);
    assert(animation_catalog_allowed(static_cast<animation_id_t>(scene.figure.animationId()),
                                     ANIMATION_STATE_IDLE));
    ++checks;
}

static void checkPendingAndReelRequests() {
    {
        Scene scene;
        assert(scene.figure.playAnimation(ANIMATION_SLOT_MACHINE));
        assert(scene.figure.isSceneActive());
        scene.figure.setEnabled(false); // Cancel selection before its first GUI update.
        assertDisabled(scene);
        scene.figure.setEnabled(true);
        scene.tick();
        assert(!scene.figure.isSceneActive());
        ++checks;
    }
    {
        Scene scene;
        assert(scene.figure.playAnimation(ANIMATION_PUNCH));
        scene.awaitPhase(StickFigure::ScenePhase::Enter);
        assert(scene.figure.reelRequest); // Do not consume it before toggling off.
        scene.figure.setEnabled(false);
        assertDisabled(scene);
        scene.figure.setEnabled(true);
        scene.tick();
        assert(!scene.figure.takeReelRequest());
        assert(!scene.figure.isSceneActive());
        assert(!scene.price.isAnimating());
        ++checks;
    }
}

static void checkLegacyPatrol() {
    Scene scene(false);
    assert(scene.figure.activity() == StickFigure::Activity::Patrol);
    scene.step(8000); // Disable away from the initial position, on an edge lane.
    int32_t x = scene.figure.x, y = scene.figure.y;
    auto expected = scene.priceOnlyFrame();
    scene.figure.setEnabled(false);
    assertDisabled(scene);
    scene.step(16000);
    scene.render();
    assert(frame() == expected);
    assert(scene.figure.x == x && scene.figure.y == y);
    scene.figure.setEnabled(true);
    scene.tick();
    assert(!scene.figure.autoplay);
    assert(std::abs(scene.figure.x - x) <= 8 * 256);
    assert(std::abs(scene.figure.y - y) <= 8 * 256);
    scene.step(1600);
    assert(!scene.figure.isSceneActive());
    lv_area_t bounds;
    assert(scene.figure.bounds(&bounds));
    ++checks;
}

static void checkRuntimeCombinations() {
    for (bool animation : {false, true}) {
        for (bool stickFigure : {false, true}) {
            Scene scene;
            scene.applySwitches(animation, stickFigure);
            scene.setPrice("$4,123.85");
            scene.tick();
#if ODOMETER_ANIMATION_DURATION_MS > 0
            assert(scene.price.isAnimating() == animation);
#else
            assert(!scene.price.isAnimating());
#endif
            assert(scene.figure.enabled() == (animation && stickFigure));
            if (animation && stickFigure) assert(scene.figure.isSceneActive());
            else assertDisabled(scene);
            scene.step(ODOMETER_ANIMATION_DURATION_MS + 128);
            assert(!scene.price.isAnimating());
            assert(scene.price.currentText == "$4,123.85");
            if (!animation || !stickFigure) {
                auto expected = scene.priceOnlyFrame();
                scene.render();
                assert(frame() == expected);
            }
            ++checks;
        }
    }

    Scene scene;
    scene.setPrice("$4,123.85");
    scene.step(128);
#if ODOMETER_ANIMATION_DURATION_MS > 0
    assert(scene.price.isAnimating());
#endif
    scene.applySwitches(true, false); // Figure switch leaves the active digit roll running.
    assertDisabled(scene);
#if ODOMETER_ANIMATION_DURATION_MS > 0
    assert(scene.price.isAnimating());
#endif
    scene.applySwitches(false, true); // Master switch immediately settles an active roll.
    assertDisabled(scene);
    assert(!scene.price.isAnimating());
    auto expected = scene.priceOnlyFrame();
    scene.render();
    assert(frame() == expected);
    scene.setPrice("$4,124.85");
    assert(!scene.price.isAnimating());
    assert(scene.price.currentText == "$4,124.85");
    scene.applySwitches(true, false);
    scene.setPrice("$4,124.95");
#if ODOMETER_ANIMATION_DURATION_MS > 0
    assert(scene.price.isAnimating());
#else
    assert(!scene.price.isAnimating());
#endif
    assertDisabled(scene);
    ++checks;
}

int main() {
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
    for (auto phase : {StickFigure::ScenePhase::Approach, StickFigure::ScenePhase::Enter,
                       StickFigure::ScenePhase::Play, StickFigure::ScenePhase::Exit})
        checkScenePhase(phase);
    checkPendingAndReelRequests();
    checkLegacyPatrol();
    checkRuntimeCombinations();
    std::printf("Animation switches: %u checks passed, roll duration %d ms\n",
                checks, ODOMETER_ANIMATION_DURATION_MS);
}
