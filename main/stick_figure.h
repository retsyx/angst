#pragma once

#include <stdint.h>
#include <cmath>
#include <lvgl.h>
#include "animation_catalog.h"
#include "stick_scenes.h"

// A small procedural companion. All coordinates supplied to update(), and
// returned by bounds(), are relative to the root's content area. Only update()
// changes LVGL objects; the draw callback draws the current pose. Keep the
// instance on the same task as LVGL, and alive for as long as its actor exists.
class StickFigure {
public:
    enum class Activity { Patrol, Approach, Assist, Celebrate, Gesture, Sad };

    StickFigure() = default;
    StickFigure(const StickFigure &) = delete;
    StickFigure &operator=(const StickFigure &) = delete;
    StickFigure(StickFigure &&) = delete;
    StickFigure &operator=(StickFigure &&) = delete;

    ~StickFigure() {
        if (actor) lv_obj_del(actor);
    }

    // Normalize once to Q8: invalid values use 1x; finite positive values
    // clamp to 0.25..8x and round to the nearest 1/256 coefficient.
    void begin(lv_obj_t *screen, float figureScale = 1.0f) {
        if (actor) lv_obj_del(actor);
        if (!std::isfinite(figureScale) || figureScale <= 0.0f) figureScale = 1.0f;
        if (figureScale < 0.25f) figureScale = 0.25f;
        if (figureScale > 8.0f) figureScale = 8.0f;
        scaleQ8 = static_cast<int32_t>(figureScale * kUnit + 0.5f);
        drawX = scaleCeil(kDrawX);
        drawY = scaleCeil(kDrawY);
        drawWidth = drawX + scaleCeil(kDrawWidth - kDrawX);
        drawHeight = drawY + scaleCeil(kDrawHeight - kDrawY);
        root = screen;
        if (!root) return;
        actor = lv_obj_create(root);
        lv_obj_remove_style_all(actor);
        lv_obj_set_size(actor, static_cast<lv_coord_t>(drawWidth),
                        static_cast<lv_coord_t>(drawHeight));
        lv_obj_clear_flag(actor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(actor, onEvent, LV_EVENT_ALL, this);
        clockStarted = false;
        wasAnimating = false;
        hasTarget = false;
        currentActivity = Activity::Patrol;
        route = Route::Bottom;
        facing = 1;
        lastPriceChangeDirection = 1;
        climbing = false;
        poseTime = 0;
        x = scaled(22) * kUnit;
        y = 0;
        lv_obj_add_flag(actor, LV_OBJ_FLAG_HIDDEN);
        scenePhase = ScenePhase::None;
        sceneId = pendingScene = 0;
        pendingPriceDirection = 0;
        sceneCanvas = manualScene = reelRequest = false;
        sceneReelCount = 0;
        teleportTarget = {};
        teleportTransferred = false;
        nextIdleAt = 0;
        roamHasTarget = false;
        roamPauseUntil = 0;
    }

    // The firmware opts into the repertoire; legacy reusable-header clients
    // retain the original patrol and digit assistance until they opt in.
    void setAutoplay(bool enabled) {
        if (autoplay != enabled) {
            autoplay = enabled;
            roamHasTarget = false;
            roamPauseUntil = 0;
        }
    }
    void setRandomSeed(uint32_t seed) { randomState = seed ? seed : 1; }
    bool enabled() const { return actorEnabled; }
    void setEnabled(bool enabled) {
        if (actorEnabled == enabled) return;
        actorEnabled = enabled;
        resumeTiming = enabled;
        if (!enabled) {
            if (actor) lv_obj_add_flag(actor, LV_OBJ_FLAG_HIDDEN);
            sceneId = pendingScene = pendingPriceDirection = 0;
            scenePhase = ScenePhase::None;
            manualScene = reelRequest = false;
            hasTarget = hasSceneNumber = false;
            sceneReelCount = 0;
            teleportTarget = {};
            teleportTransferred = false;
            moving = climbing = wasAnimating = false;
            nextIdleAt = 0;
            resumePatrol();
        }
    }
    bool playAnimation(int id) {
        if (!actorEnabled || id < 1 || id > ANIMATION_COUNT) return false;
        pendingScene = id;
        return true;
    }
    int animationId() const { return sceneId; }
    bool isSceneActive() const { return sceneId != 0 || pendingScene != 0; }
    void notifyPriceChange(int numericalDirection) {
        if (actorEnabled && numericalDirection)
            pendingPriceDirection = numericalDirection < 0 ? -1 : 1;
    }
    bool takeReelRequest() {
        const bool result = reelRequest;
        reelRequest = false;
        return result;
    }

    // Supply signed numerical price direction for the finishing mood; zero
    // preserves the older behavior of using the reel's direction instead.
    void update(uint32_t now, const lv_area_t *numberBounds,
                const lv_area_t *activeDigitBounds, int rollDirection,
                bool animating, int priceChangeDirection = 0,
                const lv_area_t *reelBounds = nullptr, size_t reelCount = 0) {
        if (!root || !actor) return;
        if (!actorEnabled) {
            lastUpdate = now;
            lv_obj_add_flag(actor, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        const uint32_t elapsed = clockStarted && !resumeTiming ? now - lastUpdate : 32;
        if (clockStarted && elapsed < 32) return;
        lastUpdate = now;
        if (resumeTiming) {
            stateSince = lastGesture = now;
            resumeTiming = false;
        }
        width = lv_obj_get_content_width(root);
        height = lv_obj_get_content_height(root);
        // Hide on unusually small layouts instead of drawing clipped limbs.
        if (!fits()) {
            lv_obj_add_flag(actor, LV_OBJ_FLAG_HIDDEN);
            clockStarted = false;
            return;
        }
        if (!clockStarted) {
            y = bottomLane() * kUnit;
            x = clamp(scaled(22), leftEdge(), rightEdge()) * kUnit;
            stateSince = now;
            lastGesture = now;
            clockStarted = true;
        }
        x = clamp(x, leftEdge() * kUnit, rightEdge() * kUnit);
        y = clamp(y, topLane() * kUnit, bottomLane() * kUnit);
        lv_obj_clear_flag(actor, LV_OBJ_FLAG_HIDDEN);
        lv_area_t number;
        hasTarget = clipBounds(numberBounds, number) &&
                    clipBounds(activeDigitBounds, target);
        if (hasTarget) {
            target.x1 = static_cast<lv_coord_t>(maximum(target.x1, number.x1));
            target.x2 = static_cast<lv_coord_t>(minimum(target.x2, number.x2));
            target.y1 = static_cast<lv_coord_t>(maximum(target.y1, number.y1));
            target.y2 = static_cast<lv_coord_t>(minimum(target.y2, number.y2));
            hasTarget = target.x1 <= target.x2 && target.y1 <= target.y2;
        }
        direction = rollDirection < 0 ? -1 : 1;
        // Numerical direction can differ from reel direction for negative
        // prices. Remember replacements too, even when a roll stays active.
        if (animating || wasAnimating) {
            const int change = priceChangeDirection ? priceChangeDirection : rollDirection;
            if (change) lastPriceChangeDirection = change < 0 ? -1 : 1;
        }
        poseTime = now;
        const uint32_t dt = elapsed > 96 ? 96 : elapsed;

        hasSceneNumber = clipBounds(numberBounds, sceneNumber);
        sceneReelCount = 0;
        if (hasSceneNumber && reelBounds) {
            for (size_t i = 0; i < reelCount && i < StickScenes::MaxReels; ++i) {
                lv_area_t bounds;
                if (clipBounds(&reelBounds[i], bounds)) sceneReels[sceneReelCount++] = bounds;
            }
        }
        if (autoplay || isSceneActive()) {
            updateScenes(now, dt, animating);
            wasAnimating = animating;
            positionActor();
            return;
        }

        if (animating && hasTarget &&
            (!wasAnimating || currentActivity == Activity::Patrol ||
             currentActivity == Activity::Gesture)) startApproach(now);
        if (wasAnimating && !animating &&
            (currentActivity == Activity::Approach ||
             currentActivity == Activity::Assist)) {
            currentActivity = lastPriceChangeDirection < 0 ? Activity::Sad : Activity::Celebrate;
            stateSince = now;
            lastGesture = now;
        }
        wasAnimating = animating;
        if (!hasTarget && (currentActivity == Activity::Approach ||
                           currentActivity == Activity::Assist ||
                           currentActivity == Activity::Gesture)) {
            resumePatrol();
        }

        moving = false;
        climbing = false;
        switch (currentActivity) {
        case Activity::Patrol:
            patrol(dt);
            if (hasTarget && now - lastGesture >= 11000) {
                lastGesture = now;
                startApproach(now);
            }
            break;
        case Activity::Approach:
            approach(now, dt, animating);
            break;
        case Activity::Assist:
            // A newer update can select another rolling digit before the
            // previous roll finishes. Follow it without retaining its object.
            if (distance(x, assistX() * kUnit) > scaled(3) * kUnit ||
                distance(y, assistY() * kUnit) > scaled(3) * kUnit) {
                startApproach(now);
            }
            facing = targetCenter() >= x / kUnit ? 1 : -1;
            break;
        case Activity::Celebrate:
            if (now - stateSince >= 1200) resumePatrol();
            break;
        case Activity::Sad:
            if (now - stateSince >= 2100) resumePatrol();
            break;
        case Activity::Gesture:
            if (animating) startApproach(now);
            else if (now - stateSince >= 1900) resumePatrol();
            break;
        }

        positionActor();
    }

    Activity activity() const { return currentActivity; }
    float scale() const { return static_cast<float>(scaleQ8) / kUnit; }

    // The draw box includes room for reaching arms and a jump; transparent
    // parts do not cover the digits. Return its visible portion on the root.
    bool bounds(lv_area_t *result) const {
        if (!result || !actorEnabled || !actor || !clockStarted || !fits())
            return false;
        if (sceneCanvas) {
            *result = {0, 0, static_cast<lv_coord_t>(width - 1),
                       static_cast<lv_coord_t>(height - 1)};
            return true;
        }
        result->x1 = static_cast<lv_coord_t>(maximum(0, x / kUnit - drawX));
        result->y1 = static_cast<lv_coord_t>(maximum(0, y / kUnit - drawY));
        result->x2 = static_cast<lv_coord_t>(minimum(width - 1,
                              x / kUnit - drawX + drawWidth - 1));
        result->y2 = static_cast<lv_coord_t>(minimum(height - 1,
                              y / kUnit - drawY + drawHeight - 1));
        return true;
    }

private:
    enum class Route { Bottom, Right, Top, Left };
    enum class ApproachLeg { Edge, Down, Digit };
    static constexpr int32_t kUnit = 256;
    static constexpr lv_coord_t kDrawWidth = 46;
    static constexpr lv_coord_t kDrawHeight = 48;
    static constexpr int32_t kDrawX = 23;
    static constexpr int32_t kDrawY = 42;

    lv_obj_t *root = nullptr;
    lv_obj_t *actor = nullptr;
    lv_area_t target = {};
    int32_t scaleQ8 = kUnit;
    int32_t drawX = kDrawX, drawY = kDrawY;
    int32_t drawWidth = kDrawWidth, drawHeight = kDrawHeight;
    int32_t width = 0, height = 0;
    int32_t x = 0, y = 0;
    int32_t approachEdge = 0;
    uint32_t lastUpdate = 0, stateSince = 0, lastGesture = 0, poseTime = 0;
    Activity currentActivity = Activity::Patrol;
    Route route = Route::Bottom;
    ApproachLeg approachLeg = ApproachLeg::Digit;
    int facing = 1, direction = 1, lastPriceChangeDirection = 1;
    bool clockStarted = false, wasAnimating = false, hasTarget = false;
    bool moving = false, climbing = false;
    bool actorEnabled = true, resumeTiming = false;

    enum class ScenePhase { None, Approach, Enter, Play, Exit };
    ScenePhase scenePhase = ScenePhase::None;
    int sceneId = 0, pendingScene = 0, pendingPriceDirection = 0;
    uint32_t sceneSince = 0, sceneStarted = 0, nextIdleAt = 0;
    uint32_t randomState = 0x91e10da5;
    int lastIdleScene = 0;
    bool autoplay = false, manualScene = false, sceneCanvas = false;
    bool reelRequest = false, hasSceneNumber = false;
    lv_opa_t exitStartOpacity = LV_OPA_COVER;
    lv_area_t sceneNumber = {};
    lv_area_t sceneReels[StickScenes::MaxReels] = {};
    size_t sceneReelCount = 0;
    int32_t sceneAnchorX = 0, sceneAnchorY = 0;
    lv_point_t teleportTarget = {};
    bool teleportTransferred = false;
    lv_area_t gymnasticsCorridor = {};
    unsigned gymnasticsSegment = UINT32_MAX;
    int32_t gymnasticsFromX = 0, gymnasticsToX = 0;
    int sceneFacing = 0;
    int32_t roamTargetX = 0, roamTargetY = 0;
    uint32_t roamPauseUntil = 0, roamSpeed = 40;
    bool roamHasTarget = false;
    static constexpr uint32_t kSceneFade = 450;

    uint32_t random() {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        return randomState;
    }

    int pickScene(animation_state_t state) {
        // Reservoir sampling avoids a temporary candidate list on the ESP32.
        unsigned count = 0;
        int selected = 0;
        for (const auto &info : animation_catalog) {
            if (state == ANIMATION_STATE_IDLE && info.id == lastIdleScene) continue;
            if (animation_catalog_allowed(info.id, state) && random() % ++count == 0)
                selected = info.id;
        }
        if (state == ANIMATION_STATE_IDLE) lastIdleScene = selected;
        return selected;
    }

    static bool startsInPlace(int id) {
        switch (id) {
        case ANIMATION_DRIVE: case ANIMATION_NARUTO: case ANIMATION_SHADOW_BOX:
        case ANIMATION_COINS: case ANIMATION_SMOKE: case ANIMATION_FLY:
        case ANIMATION_PHONE: case ANIMATION_CELEBRATE:
            return true;
        default: return false;
        }
    }

    struct SceneSpace { int back, front, above, below; };
    static bool localSceneSpace(int id, SceneSpace &space) {
        // Design-pixel clearance for local props. These scenes need room,
        // but do not need to gather at the bottom-center of the display.
        switch (id) {
        case ANIMATION_CLEAN: space = {16, 36, 38, 3}; break;
        case ANIMATION_MINE: space = {13, 44, 42, 3}; break;
        case ANIMATION_PAN: space = {13, 45, 35, 4}; break;
        case ANIMATION_BROOM: space = {23, 40, 43, 3}; break;
        case ANIMATION_DOG: space = {13, 150, 37, 4}; break;
        case ANIMATION_CAT: space = {13, 50, 37, 4}; break;
        case ANIMATION_SLEEP: space = {18, 29, 46, 3}; break;
        case ANIMATION_VIDEO_GAMES: space = {13, 51, 39, 3}; break;
        case ANIMATION_UPSET: space = {13, 28, 39, 3}; break;
        case ANIMATION_COFFEE: space = {13, 45, 42, 3}; break;
        case ANIMATION_COOK: space = {13, 46, 43, 3}; break;
        case ANIMATION_MUSIC: space = {24, 46, 49, 3}; break;
        case ANIMATION_DRAW: space = {13, 55, 43, 3}; break;
        case ANIMATION_PLANTS: space = {13, 59, 37, 3}; break;
        case ANIMATION_NEWSPAPER: space = {13, 32, 35, 3}; break;
        case ANIMATION_TREASURE: space = {22, 65, 44, 6}; break;
        default: return false;
        }
        return true;
    }

    static SceneSpace sceneSpace(int id) {
        SceneSpace space;
        if (localSceneSpace(id, space)) return space;
        switch (id) {
        case ANIMATION_DRIVE: return {30, 30, 36, 3};
        case ANIMATION_NARUTO: return {42, 42, 35, 3};
        case ANIMATION_SHADOW_BOX: return {13, 26, 34, 3};
        case ANIMATION_COINS: return {16, 16, 37, 3}; // Rain remains screen-wide.
        case ANIMATION_SMOKE: return {14, 40, 44, 3};
        case ANIMATION_FLY: return {48, 30, 35, 3};
        case ANIMATION_PHONE: return {13, 31, 39, 3};
        case ANIMATION_CELEBRATE: return {18, 18, 38, 3}; // Confetti remains screen-wide.
        case ANIMATION_GYMNASTICS: return {22, 22, 37, 3};
        case ANIMATION_SWORD_FIGHT: return {13, 65, 44, 3};
        case ANIMATION_ROMANCE: return {20, 44, 40, 3}; // Include her at the meeting point.
        case ANIMATION_PARTY: return {53, 65, 44, 3};
        case ANIMATION_TELEPORT: return {19, 19, 42, 6};
        default: return {13, 13, 30, 3};
        }
    }

    static bool numberScene(int id) {
        return id == ANIMATION_PUNCH || id == ANIMATION_CRANK ||
               id == ANIMATION_SLOT_MACHINE || id == ANIMATION_CHILL;
    }

    struct SceneAnchor { int x, y, face; };
    SceneAnchor chooseSceneAnchor(const SceneSpace &space, int wantedX, int wantedY,
                                  int wantedFace, int awayX = -1) const {
        SceneAnchor best = {wantedX, wantedY, wantedFace};
        int32_t bestDistance = INT32_MAX;
        for (int turn = 0; turn < 2; ++turn) {
            const int face = turn ? -wantedFace : wantedFace;
            const int back = scaled(face > 0 ? space.back : space.front);
            const int front = scaled(face > 0 ? space.front : space.back);
            const int lowX = minimum(rightEdge(), maximum(leftEdge(), back));
            const int highX = maximum(lowX, minimum(rightEdge(), width - 1 - front));
            const int lowY = minimum(bottomLane(), maximum(topLane(), scaled(space.above)));
            const int highY = maximum(lowY, minimum(bottomLane(), height - 1 - scaled(space.below)));
            const int xs[] = {wantedX, lowX, highX, awayX - scaled(38), awayX + scaled(38)};
            const int py = clamp(wantedY, lowY, highY);
            // Keep props on screen. Transparent price glyphs are foreground
            // objects, so their rectangular viewports do not limit placement.
            const int count = awayX >= 0 ? 5 : 1;
            for (int i = 0; i < count; ++i) {
                const int px = clamp(xs[i], lowX, highX);
                if (awayX >= 0 && distance(px, awayX) < scaled(38)) continue;
                const int travel = distance(px, wantedX) + distance(py, wantedY);
                if (travel < bestDistance) {
                    best = {px, py, face}; bestDistance = travel;
                }
            }
        }
        if (bestDistance == INT32_MAX && awayX >= 0)
            return chooseSceneAnchor(space, wantedX, wantedY, wantedFace);
        return best;
    }

    lv_area_t motionArea(const SceneSpace &space) const {
        const int side = scaled(maximum(space.back, space.front));
        const int left = minimum(rightEdge(), maximum(leftEdge(), side));
        const int top = minimum(bottomLane(), maximum(topLane(), scaled(space.above)));
        return {static_cast<lv_coord_t>(left), static_cast<lv_coord_t>(top),
            static_cast<lv_coord_t>(maximum(left, minimum(rightEdge(), width - 1 - side))),
            static_cast<lv_coord_t>(maximum(top, minimum(bottomLane(), height - 1 - scaled(space.below))))};
    }

    void updateGymnastics(uint32_t age, uint32_t dt) {
        const auto motion = StickScenes::Detail::gymnasticsMotion(age,
            animation_catalog_lookup(ANIMATION_GYMNASTICS)->duration_ms);
        if (motion.segment != gymnasticsSegment) {
            gymnasticsSegment = motion.segment;
            gymnasticsFromX = gymnasticsToX = x;
            if (motion.travel) {
                const int32_t low = gymnasticsCorridor.x1 * kUnit;
                const int32_t high = gymnasticsCorridor.x2 * kUnit;
                int direction = motion.travel > 0 ? facing : -facing;
                const int32_t requested = (motion.travel > 0 ? motion.travel : -motion.travel) * scaleQ8;
                int32_t available = maximum(0, direction > 0 ? high - x : x - low);
                const int32_t other = maximum(0, direction > 0 ? x - low : high - x);
                if (available < requested && other > available) {
                    direction = -direction;
                    available = other;
                }
                // Poses are neutral at these boundaries, so turning here is
                // continuous. A handspring travels backwards without mirroring.
                facing = motion.travel > 0 ? direction : -direction;
                gymnasticsToX += direction * minimum(requested, available);
            }
        }
        const int32_t desired = gymnasticsFromX + static_cast<int32_t>(
            static_cast<int64_t>(gymnasticsToX - gymnasticsFromX) * motion.progress / 1024);
        const int32_t step = minimum(scaled(8) * kUnit,
            maximum(1, static_cast<int32_t>(static_cast<uint64_t>(200) * dt * scaleQ8 / 1000)));
        const int32_t delta = clamp(desired - x, -step, step);
        moving = delta != 0;
        x += delta;
    }

    int32_t localAnchorX(const SceneSpace &space, int face) const {
        const int32_t low = maximum(leftEdge(), scaled(face > 0 ? space.back : space.front));
        const int32_t high = minimum(rightEdge(), width - 1 - scaled(face > 0 ? space.front : space.back));
        return low <= high ? clamp(x / kUnit, low, high) :
               clamp((low + high) / 2, leftEdge(), rightEdge());
    }

    void enterScene(uint32_t now) {
        moving = climbing = false;
        gymnasticsSegment = UINT32_MAX;
        if (sceneId == ANIMATION_PUNCH || sceneId == ANIMATION_CRANK ||
            sceneId == ANIMATION_SLOT_MACHINE) facing = 1;
        else if (sceneFacing) facing = sceneFacing;
        scenePhase = ScenePhase::Enter;
        sceneSince = sceneStarted = now;
        teleportTransferred = false;
        if (sceneId == ANIMATION_TELEPORT) {
            StickScenes::Frame frame = {};
            frame.x = sceneAnchorX;
            frame.y = sceneAnchorY;
            frame.width = width;
            frame.height = height;
            frame.scaleQ8 = scaleQ8;
            frame.seed = sceneStarted;
            const auto target = StickScenes::Detail::teleportDestination(frame);
            const auto arrival = chooseSceneAnchor(sceneSpace(sceneId),
                target.x, target.y, facing, sceneAnchorX);
            teleportTarget = {static_cast<lv_coord_t>(arrival.x), static_cast<lv_coord_t>(arrival.y)};
        }
        // Manual reel scenes demonstrate motion even at an unchanged price.
        reelRequest = sceneId == ANIMATION_PUNCH || sceneId == ANIMATION_CRANK ||
                      sceneId == ANIMATION_SLOT_MACHINE;
    }

    void beginScene(int id, bool manual, uint32_t now) {
        sceneId = id;
        manualScene = manual;
        scenePhase = ScenePhase::Approach;
        sceneSince = now;
        currentActivity = Activity::Approach;
        sceneFacing = 0;
        if (id == ANIMATION_GYMNASTICS) {
            gymnasticsCorridor = motionArea(sceneSpace(id));
            sceneAnchorX = clamp(x / kUnit, gymnasticsCorridor.x1, gymnasticsCorridor.x2);
            sceneAnchorY = clamp(y / kUnit, gymnasticsCorridor.y1, gymnasticsCorridor.y2);
            sceneFacing = facing;
            approachLeg = ApproachLeg::Digit;
            if (sceneAnchorX == x / kUnit && sceneAnchorY == y / kUnit) enterScene(now);
            return;
        }
        if (startsInPlace(id)) {
            sceneAnchorX = x / kUnit;
            sceneAnchorY = y / kUnit;
            // Face inward if an extended pose would otherwise run directly
            // into the edge. Keep the exact standing coordinates, including
            // their fractional pixels, throughout the entrance fade.
            if (id != ANIMATION_COINS && id != ANIMATION_CELEBRATE) {
                const int room = facing > 0 ? width - 1 - sceneAnchorX : sceneAnchorX;
                const int otherRoom = facing > 0 ? sceneAnchorX : width - 1 - sceneAnchorX;
                if (room < scaled(30) && otherRoom > room) facing = -facing;
            }
            sceneFacing = facing;
            if (sceneAnchorX == x / kUnit && sceneAnchorY == y / kUnit) {
                enterScene(now);
                return;
            }
            approachLeg = ApproachLeg::Digit;
            return;
        }
        // Groups need a shared stage; portals and digit interactions retain
        // their own anchors. Other props use the nearest suitable space.
        sceneAnchorX = width / 2;
        sceneAnchorY = bottomLane();
        SceneSpace space;
        const bool local = localSceneSpace(id, space);
        if (local) {
            const int32_t forward = localAnchorX(space, facing);
            const int32_t reverse = localAnchorX(space, -facing);
            sceneFacing = distance(reverse * kUnit, x) < distance(forward * kUnit, x) ? -facing : facing;
            sceneAnchorX = sceneFacing == facing ? forward : reverse;
            const int32_t top = maximum(topLane(), scaled(space.above));
            const int32_t bottom = minimum(bottomLane(), height - 1 - scaled(space.below));
            sceneAnchorY = top <= bottom ? clamp(y / kUnit, top, bottom) : bottomLane();
        }
        if (id == ANIMATION_TELEPORT) {
            StickScenes::Frame frame = {};
            frame.width = width;
            frame.height = height;
            frame.scaleQ8 = scaleQ8;
            frame.seed = random();
            const auto source = StickScenes::Detail::teleportSource(frame);
            sceneAnchorX = source.x;
            sceneAnchorY = source.y;
        }
        if (hasSceneNumber && (id == ANIMATION_PUNCH || id == ANIMATION_CRANK ||
                              id == ANIMATION_SLOT_MACHINE || id == ANIMATION_CHILL)) {
            sceneAnchorX = hasTarget ? targetCenter() : (sceneNumber.x1 + sceneNumber.x2) / 2;
            if (id == ANIMATION_PUNCH || id == ANIMATION_CRANK) sceneAnchorX -= scaled(20);
            if (id == ANIMATION_SLOT_MACHINE) {
                StickScenes::Frame frame = {};
                frame.number = sceneNumber;
                frame.width = width;
                frame.height = height;
                sceneAnchorX = StickScenes::Detail::slotGeometry(frame).knob.x - scaled(14);
            }
            sceneAnchorY = id == ANIMATION_CHILL ? sceneNumber.y1 + scaled(13)
                : (hasTarget ? target.y2 + scaled(24) : sceneNumber.y2 + scaled(24));
            if (id == ANIMATION_SLOT_MACHINE) sceneAnchorY = sceneNumber.y2 + scaled(33);
        }
        if (!numberScene(id)) {
            const auto anchor = chooseSceneAnchor(sceneSpace(id), sceneAnchorX,
                                                  sceneAnchorY, sceneFacing ? sceneFacing : facing);
            sceneAnchorX = anchor.x; sceneAnchorY = anchor.y; sceneFacing = anchor.face;
        }
        sceneAnchorX = clamp(sceneAnchorX, leftEdge(), rightEdge());
        sceneAnchorY = clamp(sceneAnchorY, topLane(), bottomLane());
        if (sceneAnchorX == x / kUnit && sceneAnchorY == y / kUnit) {
            enterScene(now);
            return;
        }
        approachEdge = x / kUnit < width / 2 ? leftEdge() : rightEdge();
        // Only number interactions need the old route around the digits.
        const bool digits = id == ANIMATION_PUNCH || id == ANIMATION_CRANK || id == ANIMATION_SLOT_MACHINE;
        approachLeg = digits && hasSceneNumber && y / kUnit < sceneNumber.y2 + scaled(22)
                      ? ApproachLeg::Edge : ApproachLeg::Digit;
    }

    void exitScene(uint32_t now) {
        if (scenePhase == ScenePhase::Approach) {
            // Walking has no scene props yet, so another destination is safe.
            sceneId = 0;
            scenePhase = ScenePhase::None;
            manualScene = false;
            resumePatrol();
        } else if (scenePhase != ScenePhase::Exit) {
            exitStartOpacity = scenePhase == ScenePhase::Enter ?
                static_cast<lv_opa_t>(minimum(255, (now - sceneSince) * 255 / kSceneFade)) :
                static_cast<lv_opa_t>(LV_OPA_COVER);
            scenePhase = ScenePhase::Exit;
            sceneSince = now;
        }
    }

    void syncTeleport(uint32_t now) {
        if (sceneId != ANIMATION_TELEPORT || teleportTransferred ||
            (scenePhase != ScenePhase::Enter && scenePhase != ScenePhase::Play)) return;
        const uint32_t duration = animation_catalog_lookup(ANIMATION_TELEPORT)->duration_ms;
        if (StickScenes::Detail::teleportPhase(now - sceneStarted, duration) <
            StickScenes::Detail::TeleportArrivalPhase) return;
        // Commit while invisible so exit fades, roaming and later scenes all
        // inherit the arrival point instead of returning to the old source.
        x = teleportTarget.x * kUnit;
        y = teleportTarget.y * kUnit;
        teleportTransferred = true;
    }

    void updateScenes(uint32_t now, uint32_t dt, bool animating) {
        moving = climbing = false;
        syncTeleport(now);
        if (pendingScene && sceneId) exitScene(now);
        // A manual performance owns the stage until it finishes. Automatic
        // changes received during it are coalesced and performed afterwards.
        if (autoplay && pendingPriceDirection && sceneId && !manualScene) exitScene(now);
        if (!sceneId) {
            if (pendingScene) {
                const int id = pendingScene;
                pendingScene = 0;
                beginScene(id, true, now);
            } else if (autoplay && pendingPriceDirection) {
                const int change = pendingPriceDirection;
                pendingPriceDirection = 0;
                beginScene(pickScene(change > 0 ? ANIMATION_STATE_UP : ANIMATION_STATE_DOWN), false, now);
            } else if (autoplay && !animating) {
                if (!nextIdleAt) nextIdleAt = now + 2500 + random() % 4000;
                // Automatic scenes wait for the current walking leg to finish.
                // Explicit scene requests and price changes still take priority.
                if (static_cast<int32_t>(now - nextIdleAt) >= 0 && !roamHasTarget)
                    beginScene(pickScene(ANIMATION_STATE_IDLE), false, now);
            }
        }
        if (!sceneId) {
            currentActivity = Activity::Patrol;
            roam(now, dt);
            return;
        }
        if (scenePhase == ScenePhase::Approach) {
            bool arrived = false;
            if (approachLeg == ApproachLeg::Edge) {
                if (moveTo(approachEdge, y / kUnit, dt, 100)) approachLeg = ApproachLeg::Down;
            } else if (approachLeg == ApproachLeg::Down) {
                climbing = true;
                if (moveTo(approachEdge, sceneAnchorY, dt, 100)) approachLeg = ApproachLeg::Digit;
            } else arrived = moveTo(sceneAnchorX, sceneAnchorY, dt, 100);
            if (arrived) enterScene(now);
        } else if (scenePhase == ScenePhase::Enter) {
            if (now - sceneSince >= kSceneFade) scenePhase = ScenePhase::Play;
        } else if (scenePhase == ScenePhase::Play) {
            const uint32_t age = now - sceneStarted;
            if (sceneId == ANIMATION_DRIVE || sceneId == ANIMATION_NARUTO) {
                const auto corridor = motionArea(sceneSpace(sceneId));
                const int32_t left = corridor.x1, right = corridor.x2;
                if ((facing > 0 && x >= right * kUnit) || (facing < 0 && x <= left * kUnit))
                    facing = -facing;
                if (moveToFixed((facing > 0 ? right : left) * kUnit, y, dt,
                                sceneId == ANIMATION_NARUTO ? 130 : 75)) facing = -facing;
            } else if (sceneId == ANIMATION_GYMNASTICS) {
                updateGymnastics(age, dt);
            } else if (sceneId == ANIMATION_FLY) {
                // Fly through the full stage behind the digits, including
                // their open counters and spacing, without a rectangular cutoff.
                const auto corridor = motionArea(sceneSpace(sceneId));
                const int cx = (corridor.x1 + corridor.x2) / 2;
                const int cy = (corridor.y1 + corridor.y2) / 2;
                const int rx = (corridor.x2 - corridor.x1) / 2;
                const int ry = (corridor.y2 - corridor.y1) / 2;
                moveTo(cx + sine(age, 4800, rx), cy + sine(age + 1200, 4800, ry), dt, 130);
            }
            if (age >= animation_catalog_lookup(static_cast<animation_id_t>(sceneId))->duration_ms)
                exitScene(now);
        } else if (scenePhase == ScenePhase::Exit && now - sceneSince >= kSceneFade) {
            scenePhase = ScenePhase::None;
            sceneId = 0;
            manualScene = false;
            nextIdleAt = now + 2500 + random() % 4000;
            resumePatrol();
        }
    }

    void positionActor() {
        const bool wide = sceneId && scenePhase != ScenePhase::Approach;
        // Price glyphs cover the figure's strokes; their transparent backgrounds
        // leave its body and props visible through counters, spacing and margins.
        if (lv_obj_get_child(root, 0) != actor) lv_obj_move_background(actor);
        const int32_t actorWidth = wide ? width : drawWidth;
        const int32_t actorHeight = wide ? height : drawHeight;
        if (wide != sceneCanvas || lv_obj_get_width(actor) != actorWidth ||
                                  lv_obj_get_height(actor) != actorHeight) {
            sceneCanvas = wide;
            lv_obj_set_size(actor, static_cast<lv_coord_t>(actorWidth),
                            static_cast<lv_coord_t>(actorHeight));
        }
        lv_obj_set_pos(actor, static_cast<lv_coord_t>(wide ? 0 : x / kUnit - drawX),
                       static_cast<lv_coord_t>(wide ? 0 : y / kUnit - drawY));
        lv_obj_invalidate(actor);
    }

    static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
    static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }
    static int32_t clamp(int32_t v, int32_t low, int32_t high) {
        return minimum(maximum(v, low), high);
    }
    static int32_t distance(int32_t a, int32_t b) {
        return a > b ? a - b : b - a;
    }
    // Poses use original design pixels; root and digit geometry stays in real
    // pixels. Rectangles round outward, points round to the nearest pixel.
    int32_t scaled(int32_t design) const {
        const int64_t value = static_cast<int64_t>(design) * scaleQ8;
        return static_cast<int32_t>(value >= 0 ? (value + kUnit / 2) / kUnit
                                                : -((-value + kUnit / 2) / kUnit));
    }
    int32_t unscaled(int32_t pixels) const {
        const int64_t value = static_cast<int64_t>(pixels) * kUnit;
        return static_cast<int32_t>(value >= 0 ? (value + scaleQ8 / 2) / scaleQ8
                                                : -((-value + scaleQ8 / 2) / scaleQ8));
    }
    int32_t scaleFloor(int32_t design) const {
        const int64_t value = static_cast<int64_t>(design) * scaleQ8;
        return static_cast<int32_t>(value >= 0 ? value / kUnit
                                                : -((-value + kUnit - 1) / kUnit));
    }
    int32_t scaleCeil(int32_t design) const {
        return -scaleFloor(-design);
    }
    bool fits() const { return width >= scaleCeil(36) && height >= scaleCeil(36); }
    int32_t leftEdge() const { return scaled(12); }
    int32_t rightEdge() const { return width - scaled(13); }
    int32_t topLane() const { return minimum(scaled(30), height - scaled(7)); }
    int32_t bottomLane() const { return height - scaled(7); }
    int32_t targetCenter() const {
        return (static_cast<int32_t>(target.x1) + target.x2) / 2;
    }
    int32_t assistX() const {
        return clamp(targetCenter() - direction * scaled(10), leftEdge(), rightEdge());
    }
    int32_t assistY() const {
        return clamp(static_cast<int32_t>(target.y2) + scaled(29), topLane(), bottomLane());
    }

    bool clipBounds(const lv_area_t *source, lv_area_t &result) const {
        if (!source || source->x1 > source->x2 || source->y1 > source->y2 ||
            source->x2 < 0 || source->y2 < 0 || source->x1 >= width ||
            source->y1 >= height) return false;
        result.x1 = static_cast<lv_coord_t>(maximum(source->x1, 0));
        result.y1 = static_cast<lv_coord_t>(maximum(source->y1, 0));
        result.x2 = static_cast<lv_coord_t>(minimum(source->x2, width - 1));
        result.y2 = static_cast<lv_coord_t>(minimum(source->y2, height - 1));
        return true;
    }

    bool moveTo(int32_t toX, int32_t toY, uint32_t dt, uint32_t speed) {
        return moveToFixed(toX * kUnit, toY * kUnit, dt, speed);
    }

    bool moveToFixed(int32_t toX, int32_t toY, uint32_t dt, uint32_t speed) {
        const int32_t dx = toX - x;
        const int32_t dy = toY - y;
        const int32_t extent = maximum(distance(dx, 0), distance(dy, 0));
        const int32_t step = maximum(1, static_cast<int32_t>(
                            static_cast<uint64_t>(speed) * dt * scaleQ8 / 1000));
        if (!extent) return true;
        moving = true;
        if (dx) facing = dx > 0 ? 1 : -1;
        if (extent <= step) {
            x = toX;
            y = toY;
            return true;
        }
        x += static_cast<int32_t>(static_cast<int64_t>(dx) * step / extent);
        y += static_cast<int32_t>(static_cast<int64_t>(dy) * step / extent);
        return false;
    }

    void roam(uint32_t now, uint32_t dt) {
        const int32_t left = minimum(rightEdge(), scaled(18));
        const int32_t right = maximum(left, width - scaled(19));
        const int32_t top = minimum(bottomLane(), scaled(38));
        const int32_t bottom = bottomLane();
        if (roamHasTarget && (roamTargetX < left || roamTargetX > right ||
                              roamTargetY < top || roamTargetY > bottom)) roamHasTarget = false;
        if (!roamHasTarget) {
            if (roamPauseUntil && static_cast<int32_t>(now - roamPauseUntil) < 0) return;
            const int32_t minimumTrip = maximum(1, maximum(right - left, bottom - top) / 4);
            for (int attempt = 0; attempt < 8; ++attempt) {
                roamTargetX = left + random() % (right - left + 1);
                roamTargetY = top + random() % (bottom - top + 1);
                if (maximum(distance(roamTargetX * kUnit, x), distance(roamTargetY * kUnit, y)) >=
                    minimumTrip * kUnit) break;
            }
            roamSpeed = 36 + random() % 23;
            roamHasTarget = true;
            roamPauseUntil = 0;
        }
        if (moveTo(roamTargetX, roamTargetY, dt, roamSpeed)) {
            moving = false;
            roamHasTarget = false;
            roamPauseUntil = now + 150 + random() % 650;
        }
    }

    void patrol(uint32_t dt) {
        switch (route) {
        case Route::Bottom:
            if (y != bottomLane() * kUnit) {
                moveTo(x / kUnit, bottomLane(), dt, 35);
            } else if (moveTo(rightEdge(), bottomLane(), dt, 33)) {
                route = Route::Right;
            }
            break;
        case Route::Right:
            climbing = true;
            facing = 1;
            if (moveTo(rightEdge(), topLane(), dt, 36)) route = Route::Top;
            break;
        case Route::Top:
            if (moveTo(leftEdge(), topLane(), dt, 33)) route = Route::Left;
            break;
        case Route::Left:
            climbing = true;
            facing = -1;
            if (moveTo(leftEdge(), bottomLane(), dt, 36)) route = Route::Bottom;
            break;
        }
    }

    void startApproach(uint32_t now) {
        currentActivity = Activity::Approach;
        stateSince = now;
        approachEdge = x / kUnit < width / 2 ? leftEdge() : rightEdge();
        // Reach the bottom of the number by the nearest edge, so a walk from
        // the top lane does not cross the price itself.
        approachLeg = y / kUnit < static_cast<int32_t>(target.y2) + scaled(25)
                          ? ApproachLeg::Edge : ApproachLeg::Digit;
    }

    void approach(uint32_t now, uint32_t dt, bool animating) {
        switch (approachLeg) {
        case ApproachLeg::Edge:
            if (moveTo(approachEdge, y / kUnit, dt, 86))
                approachLeg = ApproachLeg::Down;
            break;
        case ApproachLeg::Down:
            climbing = true;
            if (moveTo(approachEdge, assistY(), dt, 86))
                approachLeg = ApproachLeg::Digit;
            break;
        case ApproachLeg::Digit:
            if (moveTo(assistX(), assistY(), dt, 86)) {
                currentActivity = animating ? Activity::Assist : Activity::Gesture;
                stateSince = now;
                moving = false;
                facing = targetCenter() >= x / kUnit ? 1 : -1;
            }
            break;
        }
    }

    void resumePatrol() {
        currentActivity = Activity::Patrol;
        route = Route::Bottom;
        roamHasTarget = false;
        roamPauseUntil = 0;
    }

    static int sine(uint32_t now, uint32_t period, int amplitude) {
        const uint16_t angle = static_cast<uint16_t>((now % period) * 360 / period);
        return static_cast<int32_t>(lv_trigo_sin(angle)) * amplitude / LV_TRIGO_SIN_MAX;
    }

    static void onEvent(lv_event_t *event) {
        auto *self = static_cast<StickFigure *>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_DELETE) {
            self->actor = nullptr;
            self->root = nullptr;
            self->clockStarted = false;
            self->sceneId = self->pendingScene = 0;
            self->scenePhase = ScenePhase::None;
            self->teleportTarget = {};
            self->teleportTransferred = false;
        } else if (lv_event_get_code(event) == LV_EVENT_DRAW_MAIN) {
            self->draw(lv_event_get_draw_ctx(event));
        }
    }

    void line(lv_draw_ctx_t *context, const lv_draw_line_dsc_t &style,
              int32_t ox, int32_t oy, int ax, int ay, int bx, int by) const {
        const lv_point_t a = {static_cast<lv_coord_t>(ox + scaled(ax)),
                              static_cast<lv_coord_t>(oy + scaled(ay))};
        const lv_point_t b = {static_cast<lv_coord_t>(ox + scaled(bx)),
                              static_cast<lv_coord_t>(oy + scaled(by))};
        lv_draw_line(context, &style, &a, &b);
    }

    void rectangle(lv_draw_ctx_t *context, const lv_draw_rect_dsc_t &style,
                   int32_t ox, int32_t oy, int x1, int y1, int x2, int y2) const {
        const lv_area_t area = {static_cast<lv_coord_t>(ox + scaleFloor(x1)),
                               static_cast<lv_coord_t>(oy + scaleFloor(y1)),
                               static_cast<lv_coord_t>(ox + scaleCeil(x2 + 1) - 1),
                               static_cast<lv_coord_t>(oy + scaleCeil(y2 + 1) - 1)};
        lv_draw_rect(context, &style, &area);
    }

    void draw(lv_draw_ctx_t *context) const {
        if (!actorEnabled) return;
        lv_area_t box;
        lv_obj_get_coords(actor, &box);
        if (sceneCanvas) {
            const uint32_t age = poseTime - sceneSince;
            lv_opa_t opacity = LV_OPA_COVER;
            if (scenePhase == ScenePhase::Enter)
                opacity = static_cast<lv_opa_t>(minimum(255, age * 255 / kSceneFade));
            else if (scenePhase == ScenePhase::Exit)
                opacity = static_cast<lv_opa_t>(exitStartOpacity *
                          (255 - minimum(255, age * 255 / kSceneFade)) / 255);
            if (opacity != LV_OPA_COVER)
                drawLegacy(context, box.x1 + x / kUnit, box.y1 + y / kUnit,
                           static_cast<lv_opa_t>(255 - opacity));
            StickScenes::Frame frame = {};
            frame.id = sceneId;
            frame.elapsed = poseTime - sceneStarted;
            frame.duration = animation_catalog_lookup(static_cast<animation_id_t>(sceneId))->duration_ms;
            frame.x = x / kUnit;
            frame.y = y / kUnit;
            frame.facing = facing;
            frame.scaleQ8 = scaleQ8;
            frame.width = width;
            frame.height = height;
            frame.number = sceneNumber;
            frame.hasNumber = hasSceneNumber;
            frame.opacity = frame.propOpacity = opacity;
            frame.reels = sceneReels;
            frame.reelCount = sceneReelCount;
            frame.seed = sceneStarted;
            if (sceneId == ANIMATION_TELEPORT) {
                frame.x = sceneAnchorX;
                frame.y = sceneAnchorY;
                frame.teleportTarget = teleportTarget;
                frame.hasTeleportTarget = true;
                // An interrupted fade must not cross the jump threshold after
                // the actor has already resumed at its last committed point.
                if (scenePhase == ScenePhase::Exit)
                    frame.elapsed = sceneSince - sceneStarted;
            }
            StickScenes::draw(context, box, frame);
        } else {
            drawLegacy(context, static_cast<int32_t>(box.x1) + drawX,
                       static_cast<int32_t>(box.y1) + drawY, LV_OPA_COVER);
        }
    }

    void drawLegacy(lv_draw_ctx_t *context, int32_t ox, int32_t ground,
                    lv_opa_t opacity) const {
        if (!opacity) return;
        const uint32_t stateAge = poseTime - stateSince;
        int jump = 0;
        if (currentActivity == Activity::Celebrate) {
            const uint32_t phase = stateAge % 600;
            if (phase < 450) jump = static_cast<int>(phase * (450 - phase) * 8 / 50625);
        } else if (currentActivity == Activity::Gesture && stateAge > 1300 && stateAge < 1750) {
            const uint32_t phase = stateAge - 1300;
            jump = static_cast<int>(phase * (450 - phase) * 5 / 50625);
        }
        jump = minimum(jump, maximum(0, unscaled(y / kUnit - scaled(33))));
        const int32_t oy = ground - scaled(jump);
        const int gait = moving ? sine(poseTime, climbing ? 680 : 560, 5) : 0;
        const int bob = moving && !climbing ? distance(gait, 0) / 4 : 0;
        const bool assisting = currentActivity == Activity::Assist;
        const bool gesturing = currentActivity == Activity::Gesture;
        const bool cheering = currentActivity == Activity::Celebrate;
        const bool sad = currentActivity == Activity::Sad;
        const int lean = assisting || sad ? facing * 3 : moving ? facing : 0;
        const int shoulderX = lean;
        const int shoulderY = sad ? -14 : -17 - bob;
        const int hipY = sad ? -9 : -10 - bob;
        const int headX = lean + (sad ? facing * 2 : 0);
        const int headY = sad ? -20 : -24 - bob;

        lv_draw_rect_dsc_t shape;
        lv_draw_rect_dsc_init(&shape);
        shape.radius = LV_RADIUS_CIRCLE;
        shape.bg_color = lv_color_hex(0x55432a);
        shape.bg_opa = static_cast<lv_opa_t>(LV_OPA_60 * opacity / 255);
        rectangle(context, shape, ox, ground, -7, 1, 7, 2);

        lv_draw_line_dsc_t stroke;
        lv_draw_line_dsc_init(&stroke);
        stroke.color = lv_color_hex(0xf2c87b);
        stroke.opa = opacity;
        stroke.width = static_cast<lv_coord_t>(maximum(1, scaled(2)));
        stroke.round_start = true;
        stroke.round_end = true;

        // Two-segment legs. During a roll the wide planted feet make the
        // pushing/pulling pose readable even at this display's small size.
        const int backFoot = assisting ? -7 : -4 - gait;
        const int frontFoot = assisting ? 7 : 4 + gait;
        const int backLift = moving ? maximum(0, -gait) / 2 : 0;
        const int frontLift = moving ? maximum(0, gait) / 2 : 0;
        line(context, stroke, ox, oy, 0, hipY, -3 - gait / 2, -5);
        line(context, stroke, ox, oy, -3 - gait / 2, -5, backFoot, -backLift);
        line(context, stroke, ox, oy, 0, hipY, 3 + gait / 2, -5);
        line(context, stroke, ox, oy, 3 + gait / 2, -5, frontFoot, -frontLift);
        line(context, stroke, ox, oy, 0, hipY, shoulderX, shoulderY);
        line(context, stroke, ox, oy, shoulderX, shoulderY, headX, headY + 4);

        if (assisting || (gesturing && stateAge >= 700 && stateAge < 1300)) {
            const int handX = clamp(unscaled(targetCenter() - x / kUnit), -16, 16);
            int handY = clamp(unscaled(static_cast<int32_t>(target.y2) + scaled(1) - y / kUnit),
                              -29, -9);
            if (assisting) {
                // The odometer rolls increases downward and decreases upward.
                // Catch/pull downward or brace/push upward with its motion.
                handY += direction < 0 ? -maximum(0, sine(poseTime, 740, 2))
                                       : maximum(0, sine(poseTime, 740, 3));
            }
            line(context, stroke, ox, oy, shoulderX, shoulderY,
                 facing * 8, -20);
            line(context, stroke, ox, oy, facing * 8, -20, handX, handY);
            line(context, stroke, ox, oy, shoulderX, shoulderY,
                 facing * 4 - 2, -21);
            line(context, stroke, ox, oy, facing * 4 - 2, -21,
                 clamp(handX - 3, -16, 16), handY + 2);
        } else if (sad) {
            // Drop both shoulders and the head; briefly wipe a little tear.
            line(context, stroke, ox, oy, shoulderX, shoulderY,
                 -facing * 3, -9);
            line(context, stroke, ox, oy, -facing * 3, -9, -facing * 4, -4);
            if (stateAge >= 400 && stateAge < 1100) {
                line(context, stroke, ox, oy, shoulderX, shoulderY, facing * 8, -12);
                line(context, stroke, ox, oy, facing * 8, -12,
                     headX + facing * 2, headY + 1);
            } else {
                line(context, stroke, ox, oy, shoulderX, shoulderY, facing * 7, -9);
                line(context, stroke, ox, oy, facing * 7, -9, facing * 6, -4);
            }
        } else if (cheering) {
            line(context, stroke, ox, oy, shoulderX, shoulderY, -7, -23);
            line(context, stroke, ox, oy, -7, -23, -10, -29);
            line(context, stroke, ox, oy, shoulderX, shoulderY, 7, -23);
            line(context, stroke, ox, oy, 7, -23, 10, -29);
        } else if (gesturing || climbing) {
            const int wave = sine(poseTime, climbing ? 680 : 380, 3);
            line(context, stroke, ox, oy, shoulderX, shoulderY, facing * 7, -23);
            line(context, stroke, ox, oy, facing * 7, -23,
                 facing * (8 + wave), -29);
            line(context, stroke, ox, oy, shoulderX, shoulderY, -facing * 5, -14);
            line(context, stroke, ox, oy, -facing * 5, -14,
                 -facing * 7, climbing ? -21 - wave : -10);
        } else {
            line(context, stroke, ox, oy, shoulderX, shoulderY, -5 - gait / 2, -13);
            line(context, stroke, ox, oy, -5 - gait / 2, -13, -5 - gait, -9);
            line(context, stroke, ox, oy, shoulderX, shoulderY, 5 + gait / 2, -13);
            line(context, stroke, ox, oy, 5 + gait / 2, -13, 5 + gait, -9);
        }

        shape.bg_color = lv_color_black();
        shape.bg_opa = opacity;
        shape.border_color = stroke.color;
        shape.border_width = stroke.width;
        shape.border_opa = opacity;
        rectangle(context, shape, ox, oy, headX - 3, headY - 3, headX + 3, headY + 3);
        shape.bg_color = stroke.color;
        shape.border_width = 0;
        rectangle(context, shape, ox, oy, headX + facing, headY + (sad ? 1 : 0),
                  headX + facing, headY + (sad ? 1 : 0));
        if (sad) {
            shape.bg_color = lv_color_hex(0x81c9dd);
            const int drop = 3 + static_cast<int>((stateAge % 700) / 100);
            rectangle(context, shape, ox, oy, headX + facing * 4, headY + drop,
                      headX + facing * 4, headY + drop + 1);
        }
    }
};
