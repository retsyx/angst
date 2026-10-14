#pragma once

#include <stdint.h>
#include <lvgl.h>
#include "animation_catalog.h"

// Procedural scenery for the companion. No objects, bitmaps, heap allocations,
// or floating point are needed while painting a frame. Geometry is expressed
// in the same 256ths scale and original design pixels as StickFigure.
namespace StickScenes {
constexpr size_t MaxReels = 64;

struct Frame {
    int id;
    uint32_t elapsed;
    uint32_t duration;
    int32_t x, y;
    int facing;
    int32_t scaleQ8;
    int width, height;
    lv_area_t number;
    bool hasNumber;
    lv_opa_t opacity;
    lv_opa_t propOpacity;
    const lv_area_t *reels;
    size_t reelCount;
    uint32_t seed;
    lv_point_t teleportTarget;
    bool hasTeleportTarget;
};

namespace Detail {
constexpr uint32_t Gold = 0xf2c87b, Amber = 0xdca14b, Wood = 0x88623d;
constexpr uint32_t Muted = 0x726b68, Blue = 0x81c9dd, Green = 0x76bb78;
constexpr uint32_t Red = 0xf16d87, White = 0xd6e5e8, Purple = 0xba9bde;
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline int clamp(int v, int lo, int hi) { return min(max(v, lo), hi); }
inline int abs(int v) { return v < 0 ? -v : v; }
inline lv_opa_t opacity(lv_opa_t a, int b) {
    return static_cast<lv_opa_t>(static_cast<unsigned>(a) * clamp(b, 0, 255) / 255);
}
inline int sine(uint32_t t, uint32_t period, int amplitude) {
    return static_cast<int32_t>(lv_trigo_sin((t % period) * 360 / period)) *
           amplitude / LV_TRIGO_SIN_MAX;
}
inline int cosine(uint32_t t, uint32_t period, int amplitude) {
    return sine(t + period / 4, period, amplitude);
}
inline int roundedSine(uint32_t t, uint32_t period, int amplitude) {
    // Hold small movements at their extremes instead of a single-frame twitch.
    const int value = sine(t, period, amplitude * 256);
    return value < 0 ? -((-value + 128) / 256) : (value + 128) / 256;
}
inline int ramp(uint32_t t, uint32_t period, int amplitude) {
    const uint32_t phase = t % period;
    return phase < period / 2 ? phase * amplitude * 2 / period
                             : (period - phase) * amplitude * 2 / period;
}
inline uint32_t mixBits(uint32_t value) {
    // Stateless randomness keeps a particle's path stable across redraws.
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

struct Point { int x, y; };
struct SlotGeometry {
    lv_area_t cabinet;
    Point pivot, knob;
    int radius;
};

// Machine geometry follows the actual price, independently of the actor's
// position, direction or scale. Keep the whole lever in the right-side gutter.
inline SlotGeometry slotGeometry(const Frame &f) {
    const int room = max(0, f.width - 1 - f.number.x2);
    const int radius = clamp((room - 4) / 3, 1, 3);
    const int side = clamp(room - 2 * radius - 3, 1, 3);
    SlotGeometry g = {};
    g.cabinet = {static_cast<lv_coord_t>(max(1, f.number.x1 - 3)),
                 static_cast<lv_coord_t>(max(1, f.number.y1 - 5)),
                 static_cast<lv_coord_t>(min(f.width - 2, f.number.x2 + side)),
                 static_cast<lv_coord_t>(min(f.height - 3, f.number.y2 + 5))};
    g.radius = radius;
    g.pivot = {min(f.width - radius - 2, g.cabinet.x2 + radius + 1),
               min(f.height - 9, f.number.y2 + 12)};
    const int pull = (127 - cosine(f.elapsed, 1800, 127)) * 15 / 254;
    g.knob = {min(f.width - radius - 2, g.pivot.x + 2), g.pivot.y - 22 + pull};
    return g;
}
struct Pose {
    Point hip = {0, -10}, shoulder = {0, -18}, head = {0, -25};
    Point elbow1 = {-5, -13}, hand1 = {-7, -8};
    Point elbow2 = {5, -13}, hand2 = {7, -8};
    Point knee1 = {-3, -5}, foot1 = {-5, 0};
    Point knee2 = {3, -5}, foot2 = {5, 0};
    bool sad = false;
    bool closedEyes = false;
};

struct Painter {
    lv_draw_ctx_t *ctx;
    int32_t ox, oy, scale;
    int facing;
    lv_opa_t props;
    int horizontalScale = 256;

    int s(int design) const {
        const int64_t n = static_cast<int64_t>(design) * scale;
        return static_cast<int>(n >= 0 ? (n + 128) / 256 : -((-n + 128) / 256));
    }
    int unscale(int real) const {
        const int64_t n = static_cast<int64_t>(real) * 256;
        return static_cast<int>(n >= 0 ? (n + scale / 2) / scale : -((-n + scale / 2) / scale));
    }
    int wx(int x) const { return ox + facing * s(x) * horizontalScale / 256; }
    int wy(int y) const { return oy + s(y); }
    int localX(int rootX, const lv_area_t &canvas) const {
        return facing * unscale(rootX + canvas.x1 - ox);
    }
    int localY(int rootY, const lv_area_t &canvas) const {
        return unscale(rootY + canvas.y1 - oy);
    }
    Painter offset(int x, int y, int face = 1) const {
        return {ctx, wx(x), wy(y), scale, facing * face, props, horizontalScale};
    }
    void realLine(int ax, int ay, int bx, int by, uint32_t color,
                  lv_opa_t opa, int width = 1) const {
        if (opa == 0) return;
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = lv_color_hex(color);
        d.opa = opa;
        d.width = static_cast<lv_coord_t>(max(1, s(width)));
        d.round_start = d.round_end = true;
        const lv_point_t a = {static_cast<lv_coord_t>(ax), static_cast<lv_coord_t>(ay)};
        const lv_point_t b = {static_cast<lv_coord_t>(bx), static_cast<lv_coord_t>(by)};
        lv_draw_line(ctx, &d, &a, &b);
    }
    void line(int ax, int ay, int bx, int by, uint32_t color = Wood,
              int width = 1, int opa = -1) const {
        realLine(wx(ax), wy(ay), wx(bx), wy(by), color,
                 opa < 0 ? props : static_cast<lv_opa_t>(opa), width);
    }
    void line(Point a, Point b, uint32_t color, int width, lv_opa_t opa) const {
        line(a.x, a.y, b.x, b.y, color, width, opa);
    }
    void realRect(int x1, int y1, int x2, int y2, uint32_t color,
                  bool filled, lv_opa_t opa, int radius = 0, int border = 1) const {
        if (opa == 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = radius == LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : s(radius);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = filled ? opa : static_cast<lv_opa_t>(LV_OPA_TRANSP);
        d.border_color = lv_color_hex(color);
        d.border_width = filled ? 0 : max(1, s(border));
        d.border_opa = opa;
        const lv_area_t area = {static_cast<lv_coord_t>(min(x1, x2)),
                                static_cast<lv_coord_t>(min(y1, y2)),
                                static_cast<lv_coord_t>(max(x1, x2)),
                                static_cast<lv_coord_t>(max(y1, y2))};
        lv_draw_rect(ctx, &d, &area);
    }
    void rect(int x1, int y1, int x2, int y2, uint32_t color = Wood,
              bool filled = false, int radius = 0, int opa = -1, int border = 1) const {
        realRect(wx(x1), wy(y1), wx(x2), wy(y2), color, filled,
                 opa < 0 ? props : static_cast<lv_opa_t>(opa), radius, border);
    }
    void circle(int x, int y, int radius, uint32_t color = Wood,
                bool filled = false, int opa = -1, int border = 1) const {
        rect(x - radius, y - radius, x + radius, y + radius, color,
             filled, LV_RADIUS_CIRCLE, opa, border);
    }
    void triangle(Point a, Point b, Point c, uint32_t color, int opa = -1) const {
        const lv_opa_t alpha = opa < 0 ? props : static_cast<lv_opa_t>(opa);
        if (!alpha) return;
        lv_draw_rect_dsc_t style;
        lv_draw_rect_dsc_init(&style);
        style.bg_color = lv_color_hex(color);
        style.bg_opa = alpha;
        style.border_width = 0;
        const lv_point_t points[] = {
            {static_cast<lv_coord_t>(wx(a.x)), static_cast<lv_coord_t>(wy(a.y))},
            {static_cast<lv_coord_t>(wx(b.x)), static_cast<lv_coord_t>(wy(b.y))},
            {static_cast<lv_coord_t>(wx(c.x)), static_cast<lv_coord_t>(wy(c.y))}};
        lv_draw_triangle(ctx, &style, points);
    }
    void spark(int x, int y, int radius = 2, uint32_t color = Gold, int opa = -1) const {
        line(x - radius, y, x + radius, y, color, 1, opa);
        line(x, y - radius, x, y + radius, color, 1, opa);
    }
    void heart(int x, int y, int r, int opa = -1) const {
        // Separate lobes, a two-row cleft and a tapered point. Filled pixel
        // intervals stay joined at every scale without rounded line caps
        // closing the cleft, or overlapping strokes compounding the opacity.
        const int width = max(5, s(2 * r + 1));
        const int height = max(5, s(2 * r));
        const int left = wx(x) - width / 2, top = wy(y) - height / 2;
        const lv_opa_t alpha = opa < 0 ? props : static_cast<lv_opa_t>(opa);
        const auto row = [&](int yy, int first, int last) {
            const int x1 = left + first * width / 11;
            const int x2 = left + (last + 1) * width / 11;
            const int y1 = top + yy * height / 10;
            const int y2 = top + (yy + 1) * height / 10;
            if (x2 > x1 && y2 > y1)
                realRect(x1, y1, x2 - 1, y2 - 1, Red, true, alpha);
        };
        row(0, 2, 3); row(0, 7, 8);
        row(1, 1, 4); row(1, 6, 9);
        for (int yy = 2; yy <= 4; ++yy) row(yy, 0, 10);
        for (int yy = 5; yy < 10; ++yy) row(yy, yy - 4, 14 - yy);
    }
    void note(int x, int y, int opa = -1) const {
        circle(x, y, 1, Blue, true, opa);
        line(x + 1, y, x + 1, y - 7, Blue, 1, opa);
        line(x + 1, y - 7, x + 4, y - 5, Blue, 1, opa);
    }
    void z(int x, int y, int size, int opa) const {
        line(x, y, x + size, y, Blue, 1, opa);
        line(x + size, y, x, y + size, Blue, 1, opa);
        line(x, y + size, x + size, y + size, Blue, 1, opa);
    }
    void body(const Pose &p, lv_opa_t opa, uint32_t color = Gold,
              bool articulatedNeck = false) const {
        if (opa == 0) return;
        line(p.hip, p.knee1, color, 2, opa);
        line(p.knee1, p.foot1, color, 2, opa);
        line(p.hip, p.knee2, color, 2, opa);
        line(p.knee2, p.foot2, color, 2, opa);
        line(p.hip, p.shoulder, color, 2, opa);
        Point neck = {p.head.x, p.head.y + 4};
        if (articulatedNeck) {
            const int dx = p.shoulder.x - p.head.x, dy = p.shoulder.y - p.head.y;
            const int extent = max(abs(dx), abs(dy));
            if (extent) neck = {p.head.x + dx * 4 / extent, p.head.y + dy * 4 / extent};
        }
        line(p.shoulder, neck, color, 2, opa);
        line(p.shoulder, p.elbow1, color, 2, opa);
        line(p.elbow1, p.hand1, color, 2, opa);
        line(p.shoulder, p.elbow2, color, 2, opa);
        line(p.elbow2, p.hand2, color, 2, opa);
        circle(p.head.x, p.head.y, 3, color, false, opa, 2);
        if (p.closedEyes) line(p.head.x, p.head.y, p.head.x + 2, p.head.y, color, 1, opa);
        else rect(p.head.x + 1, p.head.y + (p.sad ? 1 : 0),
                  p.head.x + 1, p.head.y + (p.sad ? 1 : 0), color, true, 0, opa);
    }
};

constexpr int TeleportArrivalPhase = 3400;
inline int teleportPhase(uint32_t elapsed, uint32_t duration) {
    const uint64_t phase = static_cast<uint64_t>(elapsed) * 10000 /
                           (duration ? duration : 10000);
    return phase < 10000 ? static_cast<int>(phase) : 10000;
}

inline lv_area_t teleportBounds(const Frame &f) {
    const int side = (19 * f.scaleQ8 + 128) / 256;
    const int top = (42 * f.scaleQ8 + 128) / 256;
    const int bottom = (7 * f.scaleQ8 + 128) / 256;
    const int right = max(side, f.width - 1 - side);
    const int ground = max(0, f.height - bottom);
    return {static_cast<lv_coord_t>(min(side, right)),
            static_cast<lv_coord_t>(min(top, ground)),
            static_cast<lv_coord_t>(right), static_cast<lv_coord_t>(ground)};
}
inline Point teleportSource(const Frame &f) {
    const lv_area_t area = teleportBounds(f);
    const int span = area.x2 - area.x1;
    const int gap = min(span, (38 * f.scaleQ8 + 128) / 256);
    const uint32_t bits = mixBits(f.seed ^ 0x13b79a65U);
    int x = area.x1 + bits % (span + 1);
    // On a narrow layout avoid a central source that would leave too little
    // space for a second portal on either side.
    if (span < 2 * gap) {
        const int count = span - gap + 1;
        const int pick = bits % (2 * count);
        x = pick < count ? area.x1 + pick : area.x2 - (pick - count);
    }
    return {x, area.y1 + static_cast<int>(mixBits(bits) % (area.y2 - area.y1 + 1))};
}
inline Point teleportDestination(const Frame &f) {
    const lv_area_t area = teleportBounds(f);
    const int gap = min(area.x2 - area.x1, (38 * f.scaleQ8 + 128) / 256);
    const int leftCount = max(0, f.x - gap - area.x1 + 1);
    const int rightCount = max(0, area.x2 - f.x - gap + 1);
    const uint32_t bits = mixBits(f.seed ^ 0xb4c85d29U);
    int x;
    if (leftCount + rightCount) {
        const int pick = bits % (leftCount + rightCount);
        x = pick < leftCount ? area.x1 + pick : f.x + gap + pick - leftCount;
    } else {
        // A caller with an arbitrary source still gets the farthest endpoint.
        x = f.x - area.x1 > area.x2 - f.x ? area.x1 : area.x2;
    }
    return {x, area.y1 + static_cast<int>(mixBits(bits) % (area.y2 - area.y1 + 1))};
}

inline void lean(Pose &p, int amount, int bob = 0) {
    p.hip = {amount / 2, -10 - bob};
    p.shoulder = {amount, -18 - bob};
    p.head = {amount + amount / 3, -25 - bob};
}
inline void run(Pose &p, int gait, bool naruto = false) {
    lean(p, naruto ? 8 : 2, abs(gait) / 3);
    p.knee1 = {-3 - gait / 2, -6}; p.foot1 = {-5 - gait, -max(0, -gait)};
    p.knee2 = {3 + gait / 2, -6}; p.foot2 = {5 + gait, -max(0, gait)};
    if (naruto) {
        p.elbow1 = {-3, -18}; p.hand1 = {-12, -22};
        p.elbow2 = {-1, -16}; p.hand2 = {-10, -18};
    } else {
        p.elbow1 = {-5 - gait / 2, -14}; p.hand1 = {-6 - gait, -10};
        p.elbow2 = {5 + gait / 2, -14}; p.hand2 = {6 + gait, -10};
    }
}
inline void sit(Pose &p, bool sad = false) {
    p.hip = {0, -13}; p.shoulder = {sad ? 4 : 0, sad ? -20 : -22};
    p.head = {sad ? 7 : 1, sad ? -25 : -29};
    p.knee1 = {10, -12}; p.foot1 = {12, 0};
    p.knee2 = {7, -11}; p.foot2 = {9, 0};
    p.elbow1 = {6, -18}; p.hand1 = {9, -12};
    p.elbow2 = {8, -18}; p.hand2 = {12, -13};
    p.sad = sad;
}
inline Pose musician(int instrument, int beat) {
    Pose p;
    if (instrument == 0) {
        sit(p);
        p.elbow1 = {7, -24}; p.hand1 = {14, -26 + beat};
        p.elbow2 = {10, -20}; p.hand2 = {22, -23 - beat};
    } else if (instrument == 1) {
        p.elbow1 = {6, -14}; p.hand1 = {10, -13 + beat / 3};
        p.elbow2 = {9, -21}; p.hand2 = {20, -23};
    } else {
        p.elbow1 = {6, -20}; p.hand1 = {9, -20};
        p.elbow2 = {9, -15}; p.hand2 = {13, -13};
    }
    return p;
}
inline void blendPose(Pose &to, const Pose &from, int progress) {
    Point *dest[] = {&to.hip, &to.shoulder, &to.head, &to.elbow1, &to.hand1,
                    &to.elbow2, &to.hand2, &to.knee1, &to.foot1, &to.knee2, &to.foot2};
    const Point *source[] = {&from.hip, &from.shoulder, &from.head, &from.elbow1,
                            &from.hand1, &from.elbow2, &from.hand2, &from.knee1,
                            &from.foot1, &from.knee2, &from.foot2};
    progress = clamp(progress, 0, 255);
    for (int i = 0; i < 11; ++i) {
        dest[i]->x = source[i]->x + (dest[i]->x - source[i]->x) * progress / 255;
        dest[i]->y = source[i]->y + (dest[i]->y - source[i]->y) * progress / 255;
    }
}

enum class GymnasticsAction { Ready, Cartwheel, Jump, Somersault, Handspring, Finish };

struct GymnasticsMotion {
    GymnasticsAction action;
    unsigned segment;
    uint16_t progress; // 0..1024 within this segment.
    int travel;        // Total design pixels relative to the actor's facing.
};

// The renderer and actor movement share one timeline. Every segment starts
// and finishes standing at the actor's landing baseline; the framework can
// choose a safe travel direction there without mirroring an airborne pose.
inline GymnasticsMotion gymnasticsMotion(uint32_t elapsed, uint32_t duration) {
    struct Segment { GymnasticsAction action; uint16_t duration; int16_t travel; };
    static constexpr Segment segments[] = {
        {GymnasticsAction::Ready, 600, 0},
        {GymnasticsAction::Cartwheel, 1800, 200},
        {GymnasticsAction::Jump, 1200, 100},
        {GymnasticsAction::Somersault, 1800, 160},
        {GymnasticsAction::Ready, 400, 0},
        {GymnasticsAction::Handspring, 2000, -160},
        {GymnasticsAction::Cartwheel, 1800, 200},
        {GymnasticsAction::Finish, 1400, 0}
    };
    const uint64_t normalized = static_cast<uint64_t>(elapsed) * 11000 /
                                (duration ? duration : 11000);
    const uint32_t time = normalized > 11000 ? 11000 : static_cast<uint32_t>(normalized);
    uint32_t start = 0;
    for (unsigned i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i) {
        const Segment &segment = segments[i];
        if (time < start + segment.duration || i == 7)
            return {segment.action, i,
                    static_cast<uint16_t>((time - start) * 1024 / segment.duration), segment.travel};
        start += segment.duration;
    }
    return {GymnasticsAction::Finish, 7, 1024, 0};
}

inline int gymnasticsEase(int progress) {
    const int p = clamp(progress, 0, 1024);
    return p * p * (3072 - 2 * p) / (1024 * 1024);
}

struct GymnasticsKeyframe { uint16_t progress; Pose pose; };

inline Pose gymnasticsInterpolate(const GymnasticsKeyframe *keys, size_t count, int progress) {
    if (progress <= keys[0].progress) return keys[0].pose;
    for (size_t i = 1; i < count; ++i) {
        if (progress > keys[i].progress) continue;
        Pose result = keys[i].pose;
        const int phase = gymnasticsEase((progress - keys[i - 1].progress) * 1024 /
                                        (keys[i].progress - keys[i - 1].progress));
        Point *to[] = {&result.hip, &result.shoulder, &result.head, &result.elbow1,
                       &result.hand1, &result.elbow2, &result.hand2, &result.knee1,
                       &result.foot1, &result.knee2, &result.foot2};
        const Pose &previous = keys[i - 1].pose;
        const Point *from[] = {&previous.hip, &previous.shoulder, &previous.head,
                               &previous.elbow1, &previous.hand1, &previous.elbow2,
                               &previous.hand2, &previous.knee1, &previous.foot1,
                               &previous.knee2, &previous.foot2};
        for (unsigned joint = 0; joint < 11; ++joint) {
            to[joint]->x = from[joint]->x + (to[joint]->x - from[joint]->x) * phase / 1024;
            to[joint]->y = from[joint]->y + (to[joint]->y - from[joint]->y) * phase / 1024;
        }
        return result;
    }
    return keys[count - 1].pose;
}

// Joint centers remain inside x=-19..19, y=-35..0. Including the head and
// rounded strokes, {22,22,37,3} contains every maneuver and its landing shadow.
// The jump compresses the torso and tucks the legs to fit below a tall price.
inline Pose gymnasticsPose(const GymnasticsMotion &motion) {
    static constexpr Pose standing = {};
    static constexpr Pose crouched = {
        {-2,-7}, {0,-14}, {2,-21}, {-8,-12}, {-10,-18}, {8,-12}, {10,-18},
        {-7,-5}, {-9,0}, {6,-4}, {9,0}
    };
    static constexpr GymnasticsKeyframe cartwheel[] = {
        {0, standing},
        {128, {{1,-10}, {3,-18}, {5,-25}, {-8,-22}, {-11,-29}, {9,-20}, {13,-24},
               {-6,-5}, {-10,0}, {7,-5}, {11,0}}},
        // Plant the leading hand before the second hand takes the weight.
        {256, {{-1,-14}, {5,-10}, {10,-8}, {8,-5}, {12,0}, {-3,-13}, {-8,-15},
               {-8,-23}, {-13,-31}, {-6,-18}, {-14,-21}}},
        {384, {{0,-20}, {0,-11}, {0,-5}, {-5,-5}, {-10,0}, {6,-6}, {12,0},
               {-7,-27}, {-12,-34}, {8,-27}, {15,-32}}},
        // Split legs above both hands, then transfer to the trailing hand.
        {512, {{0,-21}, {0,-12}, {0,-6}, {-5,-6}, {-10,0}, {5,-6}, {10,0},
               {-8,-29}, {-17,-34}, {8,-29}, {17,-34}}},
        {640, {{2,-18}, {-3,-10}, {-8,-8}, {-5,-5}, {-9,0}, {5,-15}, {10,-21},
               {7,-24}, {15,-30}, {10,-17}, {19,-18}}},
        {768, {{1,-11}, {-3,-19}, {-7,-25}, {-8,-13}, {-11,-5}, {4,-23}, {7,-29},
               {-4,-6}, {-8,0}, {6,-5}, {11,0}}},
        {896, crouched}, {1024, standing}
    };
    static constexpr GymnasticsKeyframe jump[] = {
        {0, standing}, {128, crouched},
        {256, {{0,-16}, {0,-23}, {0,-29}, {-7,-24}, {-10,-30}, {7,-24}, {10,-30},
               {-5,-11}, {-8,-5}, {5,-11}, {8,-5}}},
        // Raised hands and folded lower legs distinguish the airborne tuck.
        {512, {{0,-22}, {0,-28}, {0,-32}, {-7,-31}, {-11,-35}, {7,-31}, {11,-35},
               {-8,-19}, {-10,-25}, {8,-19}, {10,-25}}},
        {768, {{0,-16}, {0,-23}, {0,-28}, {-7,-24}, {-10,-30}, {7,-24}, {10,-30},
               {-5,-11}, {-8,-5}, {5,-11}, {8,-5}}},
        {896, crouched}, {1024, standing}
    };
    static constexpr GymnasticsKeyframe somersault[] = {
        {0, standing}, {128, crouched},
        {256, {{-2,-18}, {2,-25}, {7,-31}, {6,-23}, {10,-27}, {10,-22}, {15,-26},
               {7,-13}, {9,-8}, {-5,-13}, {-9,-12}}},
        // Fold into a small ball, turn head-first, then open before landing.
        {384, {{-4,-27}, {2,-29}, {7,-33}, {-1,-23}, {1,-20}, {7,-24}, {3,-20},
               {6,-23}, {0,-19}, {7,-21}, {0,-17}}},
        {512, {{0,-31}, {4,-26}, {7,-20}, {-2,-26}, {-4,-30}, {8,-27}, {4,-31},
               {-5,-29}, {-8,-34}, {-4,-27}, {-7,-33}}},
        {640, {{-1,-27}, {-5,-30}, {-10,-33}, {-8,-26}, {-2,-22}, {-2,-26}, {4,-24},
               {6,-25}, {8,-31}, {4,-22}, {8,-26}}},
        {768, {{0,-16}, {-2,-23}, {-5,-29}, {-8,-22}, {-10,-16}, {5,-22}, {9,-16},
               {-4,-10}, {-7,-2}, {4,-10}, {7,-2}}},
        {896, crouched}, {1024, standing}
    };
    static constexpr GymnasticsKeyframe handspring[] = {
        {0, standing}, {128, crouched},
        // Sit back, throw the arms behind, and arch into the hand support.
        {256, {{-4,-16}, {-9,-22}, {-13,-27}, {-12,-24}, {-18,-28}, {-6,-24}, {-15,-30},
               {-2,-10}, {3,-2}, {5,-9}, {11,-2}}},
        {384, {{0,-24}, {-8,-20}, {-13,-15}, {-13,-13}, {-18,-6}, {-9,-12}, {-13,-5},
               {7,-26}, {12,-33}, {9,-23}, {16,-28}}},
        {512, {{-2,-18}, {-4,-10}, {-3,-5}, {-9,-5}, {-13,0}, {1,-5}, {5,0},
               {-8,-28}, {-12,-35}, {8,-27}, {13,-33}}},
        // Push off both hands and snap the bent legs down beneath the body.
        {640, {{-1,-18}, {-6,-22}, {-10,-29}, {-12,-14}, {-15,-9}, {-4,-14}, {0,-9},
               {9,-10}, {15,-2}, {5,-7}, {11,0}}},
        {768, crouched}, {1024, standing}
    };
    static constexpr GymnasticsKeyframe finish[] = {
        {0, standing},
        {256, {{0,-10}, {0,-18}, {0,-25}, {-7,-24}, {-9,-32}, {7,-24}, {9,-32},
               {-3,-5}, {-5,0}, {3,-5}, {5,0}}},
        {768, {{0,-10}, {0,-18}, {0,-25}, {-7,-24}, {-9,-32}, {7,-24}, {9,-32},
               {-3,-5}, {-5,0}, {3,-5}, {5,0}}},
        {1024, standing}
    };
    const GymnasticsKeyframe *keys = nullptr;
    size_t count = 0;
    switch (motion.action) {
    case GymnasticsAction::Cartwheel: keys = cartwheel; count = sizeof(cartwheel) / sizeof(cartwheel[0]); break;
    case GymnasticsAction::Jump: keys = jump; count = sizeof(jump) / sizeof(jump[0]); break;
    case GymnasticsAction::Somersault: keys = somersault; count = sizeof(somersault) / sizeof(somersault[0]); break;
    case GymnasticsAction::Handspring: keys = handspring; count = sizeof(handspring) / sizeof(handspring[0]); break;
    case GymnasticsAction::Finish: keys = finish; count = sizeof(finish) / sizeof(finish[0]); break;
    case GymnasticsAction::Ready: return standing;
    }
    return gymnasticsInterpolate(keys, count, motion.progress);
}
inline void chair(const Painter &p, uint32_t color = Wood) {
    p.line(-4, -26, -4, -12, color, 2);
    p.line(-4, -12, 9, -12, color, 2);
    p.line(-3, -12, -7, 0, color);
    p.line(7, -12, 11, 0, color);
}
inline void broom(const Painter &p, int x, int y, int tilt) {
    p.line(x - tilt, y - 29, x, y - 5, Wood, 2);
    p.line(x - 5, y, x + 5, y, Amber, 2);
    for (int i = -4; i <= 4; i += 2) p.line(x, y - 6, x + i, y, Amber);
}
inline void plant(const Painter &p, int x, int growth, bool droop = false) {
    p.rect(x - 5, -6, x + 5, 0, Wood, false, 2);
    p.line(x, -6, x, -6 - growth, Green, 2);
    const int stem = -6 - growth / 2;
    p.line(x, stem, x - 5, stem - (droop ? -1 : 4), Green, 2);
    p.line(x, stem - 2, x + 5, stem - (droop ? 0 : 6), Green, 2);
    p.circle(x - 5, stem - (droop ? -1 : 4), 2, Green, true);
    p.circle(x + 5, stem - (droop ? 0 : 6), 2, Green, true);
    if (growth > 12) p.circle(x, -6 - growth, 2, Red, true);
}
// The pets have different silhouettes, not just different ears on one skeleton.
// Single-pixel outlines leave room for faces and separated paws at native size.
enum class DogFetchPhase { Windup, Throw, Chase, Pickup, Turn, Return, Handoff, Reset };

struct DogFetchFrame {
    DogFetchPhase phase;
    Point ball, hand;
    int dogX, bow, facingQ8, gait;
    bool carrying;
};

inline int dogFetchEase(int progress) {
    const int q = clamp(progress, 0, 1024);
    return q * q * (3072 - 2 * q) / (1024 * 1024);
}

inline Point dogFetchLerp(Point from, Point to, int progress) {
    const int q = dogFetchEase(progress);
    return {from.x + (to.x - from.x) * q / 1024,
            from.y + (to.y - from.y) * q / 1024};
}

// Two complete throws fit the twelve-second scene. All phase joins use the
// same ball/hand/muzzle positions; turning carries the ball around the snout
// instead of switching its screen coordinates when the dog changes direction.
inline DogFetchFrame dogFetch(uint32_t elapsed) {
    const int t = elapsed % 6000;
    DogFetchFrame result = {DogFetchPhase::Windup, {14,-9}, {14,-9}, 38, 0, 256, 0, false};
    if (t < 500) {
        result.hand = result.ball = dogFetchLerp({14,-9}, {12,-26}, t * 1024 / 500);
    } else if (t < 2200) {
        const int running = t - 500;
        result.dogX = 38 + 86 * dogFetchEase(running * 1024 / 1700) / 1024;
        const int gaitOpacity = min(255, min(running, 1700 - running) * 255 / 160);
        result.gait = sine(running, 440, 5) * gaitOpacity / 255;
        if (t < 1600) {
            result.phase = DogFetchPhase::Throw;
            const int q = (t - 500) * 1024 / 1100;
            result.ball = {12 + 128 * q / 1024,
                           -26 + 24 * q / 1024 - 64 * q * (1024 - q) / (1024 * 1024)};
            result.hand = dogFetchLerp({12,-26}, {14,-9}, q);
        } else {
            result.phase = DogFetchPhase::Chase;
            result.ball = {140,-2};
        }
    } else if (t < 2700) {
        result.phase = DogFetchPhase::Pickup;
        result.dogX = 124;
        result.bow = 10 * dogFetchEase((t - 2200) * 1024 / 500) / 1024;
        result.ball = {140,-2};
    } else if (t < 3100) {
        result.phase = DogFetchPhase::Turn;
        const int q = dogFetchEase((t - 2700) * 1024 / 400);
        result.dogX = 124;
        result.facingQ8 = 256 - 512 * q / 1024;
        result.bow = 10 - 10 * q / 1024;
        result.carrying = true;
    } else if (t < 4700) {
        result.phase = DogFetchPhase::Return;
        const int running = t - 3100;
        result.dogX = 124 - 86 * dogFetchEase(running * 1024 / 1600) / 1024;
        const int gaitOpacity = min(255, min(running, 1600 - running) * 255 / 160);
        result.gait = sine(running, 440, 5) * gaitOpacity / 255;
        result.facingQ8 = -256;
        result.carrying = true;
    } else if (t < 5200) {
        result.phase = DogFetchPhase::Handoff;
        result.facingQ8 = -256;
        if (t < 4950) {
            result.hand = dogFetchLerp({14,-9}, {22,-12}, (t - 4700) * 1024 / 250);
            result.carrying = true;
        } else {
            result.hand = result.ball = dogFetchLerp({22,-12}, {14,-9}, (t - 4950) * 1024 / 250);
        }
    } else {
        result.phase = DogFetchPhase::Reset;
        result.facingQ8 = -256 + 512 * dogFetchEase((t - 5200) * 1024 / 400) / 1024;
    }
    if (result.carrying)
        result.ball = {result.dogX + 16 * result.facingQ8 / 256, -12 + result.bow};
    return result;
}

inline void dog(const Painter &p, uint32_t t, int bow, int gait = 0) {
    const int chest = bow / 2;
    const lv_opa_t far = opacity(p.props, 145);
    // Opposite near/far paws alternate while running; idle paws stay planted.
    const Point backFar = {-5 + gait, -1 - max(0, -gait) / 2};
    const Point frontFar = {7 - gait, -1 - max(0, gait) / 2};
    p.line(-6, -6, backFar.x, backFar.y, Amber, 1, far);
    p.line(backFar.x, backFar.y, backFar.x + 2, backFar.y, Amber, 1, far);
    p.line(3, -7 + chest, frontFar.x, frontFar.y, Amber, 1, far);
    p.line(frontFar.x, frontFar.y, frontFar.x + 2, frontFar.y, Amber, 1, far);
    const int wag = sine(t, 360, 3);
    p.line(-12, -10, -16, -13 + wag / 2, Amber);
    p.line(-16, -13 + wag / 2, -18, -18 + wag, Amber);
    // Long back, rounded haunch, belly and sloping chest.
    p.line(-12, -10, -9, -13, Amber);
    p.line(-9, -13, -3, -13, Amber);
    p.line(-3, -13, 3, -12 + chest, Amber);
    p.line(-12, -10, -12, -7, Amber);
    p.line(-12, -7, -9, -5, Amber);
    p.line(-9, -5, 1, -5 + chest / 2, Amber);
    p.line(1, -5 + chest / 2, 5, -9 + chest, Amber);
    const Point backNear = {-10 - gait, -max(0, gait) / 2};
    const Point frontNear = {3 + chest + gait, -max(0, -gait) / 2};
    p.line(-9, -6, backNear.x, backNear.y, Amber);
    p.line(backNear.x, backNear.y, backNear.x + 3, backNear.y, Amber);
    p.line(2, -7 + chest, frontNear.x, frontNear.y, Amber);
    p.line(frontNear.x, frontNear.y, frontNear.x + 3, frontNear.y, Amber);

    const Painter head = p.offset(0, bow);
    // A forehead, projecting muzzle and nose read as a dog even without color.
    head.line(4, -18, 7, -20, Amber);
    head.line(7, -20, 10, -18, Amber);
    head.line(10, -18, 11, -15, Amber);
    head.line(11, -15, 15, -14, Amber);
    head.line(15, -14, 15, -11, Amber);
    head.line(15, -11, 7, -11, Amber);
    head.line(7, -11, 4, -13, Amber);
    head.rect(15, -14, 16, -13, White, true);
    head.rect(9, -17, 9, -17, White, true);
    // A broad hanging ear stays visibly separate from the snout.
    head.line(4, -18, 1, -17, Amber);
    head.line(1, -17, 0, -11, Amber);
    head.line(0, -11, 2, -9, Amber);
    head.line(2, -9, 4, -11, Amber);
    head.line(4, -11, 5, -17, Amber);
    head.line(6, -11, 8, -10, Red); // Collar below the ear.
}

inline void cat(const Painter &p, uint32_t t, int swipe) {
    const int curl = roundedSine(t, 1800, 2);
    // The long, upright hooked tail and seated haunch distinguish it from the dog.
    p.line(-5, -2, -10, -3, Purple);
    p.line(-10, -3, -12, -6, Purple);
    p.line(-12, -6, -12, -10, Purple);
    p.line(-12, -10, -11, -14, Purple);
    p.line(-11, -14, -12 + curl, -18, Purple);
    p.line(-12 + curl, -18, -14 + curl, -20, Purple);
    p.line(-14 + curl, -20, -16 + curl, -19, Purple);
    p.line(-3, -13, -6, -8, Purple);
    p.line(-6, -8, -7, -3, Purple);
    p.line(-7, -3, -5, 0, Purple);
    p.line(-5, 0, 5, 0, Purple);
    p.line(3, -13, 5, -8, Purple);
    p.line(-4, -6, -2, -4, Purple);
    p.line(-2, -4, -2, 0, Purple);
    p.line(0, -10, 0, 0, Purple);
    p.line(0, 0, 2, 0, Purple);
    // One forepaw reaches toward the lure; the other stays planted.
    const int pawX = 6 + swipe * 4 / 255;
    const int pawY = -1 - swipe * 16 / 255;
    p.line(3, -12, 6, -7 - swipe * 5 / 255, Purple);
    p.line(6, -7 - swipe * 5 / 255, pawX, pawY, Purple);
    p.line(pawX, pawY, pawX + 2, pawY, Purple);

    // Broad triangular ears are part of the head outline, with no circle
    // crossing their bases. A front-facing face keeps both ears/eyes readable.
    const Painter head = p.offset(0, max(0, roundedSine(t, 2400, 1)));
    const Point outline[] = {{-5, -15}, {-6, -18}, {-6, -24}, {-2, -21},
                             {2, -21}, {6, -24}, {6, -18}, {5, -15},
                             {2, -13}, {-2, -13}, {-5, -15}};
    for (unsigned i = 1; i < sizeof(outline) / sizeof(outline[0]); ++i)
        head.line(outline[i - 1], outline[i], Purple, 1, p.props);
    head.rect(-2, -18, -2, -18, White, true);
    head.rect(2, -18, 2, -18, White, true);
    head.rect(0, -16, 0, -16, Red, true);
    head.line(0, -15, -1, -14, Purple);
    head.line(0, -15, 1, -14, Purple);
    head.line(-4, -16, -9, -17, Purple);
    head.line(-4, -15, -9, -14, Purple);
    head.line(4, -16, 9, -17, Purple);
    head.line(4, -15, 9, -14, Purple);
}
} // namespace Detail

inline void draw(lv_draw_ctx_t *ctx, const lv_area_t &canvas, const Frame &f) {
    using namespace Detail;
    if (!ctx || f.id < 1 || f.id > ANIMATION_COUNT || f.scaleQ8 <= 0 ||
        (f.opacity == 0 && f.propOpacity == 0)) return;
    lv_area_t clip = canvas;
    const lv_area_t *oldClip = ctx->clip_area;
    if (oldClip) {
        clip.x1 = max(clip.x1, oldClip->x1); clip.y1 = max(clip.y1, oldClip->y1);
        clip.x2 = min(clip.x2, oldClip->x2); clip.y2 = min(clip.y2, oldClip->y2);
    }
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
    ctx->clip_area = &clip;
    Painter p = {ctx, canvas.x1 + f.x, canvas.y1 + f.y,
                 f.scaleQ8, f.facing < 0 ? -1 : 1, f.propOpacity};
    Pose body;
    lv_opa_t bodyOpacity = f.opacity;
    const uint32_t t = f.elapsed;
    const int wave = sine(t, 1200, 4);
    const int fast = sine(t, 600, 5);
    const int reachRootX = f.hasNumber ? clamp(f.x + p.facing * p.s(18),
                                              f.number.x1, f.number.x2) : f.x;
    const int reachX = f.hasNumber ? p.localX(reachRootX, canvas) : 18;
    const int reachY = f.hasNumber ? p.localY(f.number.y2, canvas) : -22;
    // A very subtle contact shadow gives the props and figure a common ground.
    if (f.id != 23)
        p.rect(-7, 1, 7, 2, Wood, true, LV_RADIUS_CIRCLE, opacity(f.opacity, 75));

    switch (f.id) {
    case 1: { // Clean: wipe with the rag, then sweep the loosened dust.
        lean(body, 4);
        const int scrub = sine(t, 650, 5);
        body.elbow2 = {9, -20}; body.hand2 = {15, -23 + scrub};
        body.elbow1 = {-4, -12}; body.hand1 = {-3, -5};
        p.rect(13, -26 + scrub, 18, -21 + scrub, White, true, 1);
        for (int i = 0; i < 4; ++i) {
            const int x = 19 + i * 4;
            const int a = 100 + sine(t + i * 180, 900, 70);
            p.line(x, -22 + scrub, x + 3, -22 + scrub, Blue, 1, opacity(f.propOpacity, a));
        }
        broom(p, -9, 0, 4);
        for (int i = 0; i < 3; ++i)
            p.circle(12 + i * 5 + sine(t + i * 80, 1000, 3), 0, 1, Muted, true);
        break;
    }
    case 2: { // Pickaxe repeatedly strikes a gold-bearing rock.
        lean(body, 3, max(0, fast) / 3);
        const int lift = ramp(t, 1200, 20);
        body.elbow1 = {5, -20 - lift / 4}; body.hand1 = {11, -12 - lift};
        body.elbow2 = {8, -18 - lift / 3}; body.hand2 = {13, -13 - lift};
        p.line(10, -18 - lift, 25, -9 - lift, Wood, 2);
        p.line(21, -13 - lift, 29, -6 - lift, White, 2);
        p.rect(20, -8, 37, 0, Muted, true, 5);
        p.line(23, -5, 27, -3, Gold, 2); p.line(31, -6, 34, -4, Gold, 2);
        const int flash = max(0, 255 - static_cast<int>(t % 1200) * 2);
        p.spark(22, -9, 4, Gold, opacity(f.propOpacity, flash));
        for (int i = 0; i < 3; ++i) {
            const int age = (t + i * 140) % 1200;
            p.circle(25 + i * 4 + age / 200, -8 - max(0, 8 - age / 90), 1,
                     Gold, true, opacity(f.propOpacity, max(0, 180 - age / 4)));
        }
        break;
    }
    case 3: { // A shallow pan swirls over a blue stream; gold remains inside.
        lean(body, 5);
        body.elbow1 = {8, -15}; body.hand1 = {14, -11 + wave / 2};
        body.elbow2 = {10, -19}; body.hand2 = {22, -11 + wave / 2};
        p.rect(9, -4, 43, 2, Blue, false, 5, opacity(f.propOpacity, 160));
        for (int i = 0; i < 3; ++i) p.line(12 + i * 10, -2, 17 + i * 10, -2, Blue);
        p.rect(13, -12 + wave / 2, 29, -7 + wave / 2, White, false, LV_RADIUS_CIRCLE);
        p.circle(21 + sine(t, 900, 3), -10 + wave / 2, 1, Gold, true);
        p.circle(24, -9 + wave / 2, 1, Gold, true);
        break;
    }
    case 4: { // Mime driving with empty hands, seated hips and running feet.
        sit(body);
        body.knee1 = {8 - fast / 2, -8}; body.foot1 = {6 - fast, -max(0, fast)};
        body.knee2 = {12 + fast / 2, -9}; body.foot2 = {14 + fast, -max(0, -fast)};
        body.elbow1 = {8, -25}; body.hand1 = {15, -25 + wave / 2};
        body.elbow2 = {8, -17}; body.hand2 = {16, -14 + wave / 2};
        for (int i = 0; i < 3; ++i) p.line(-17 - i * 5, -4 - i * 4, -11 - i * 5, -4 - i * 4, Muted);
        break;
    }
    case 5: { // Alternate a reaching punch and a high kick against the digits.
        lean(body, 4);
        const int extend = max(0, sine(t, 1100, 13));
        const bool kick = (t / 1100) % 2 != 0;
        const int tx = clamp(reachX, 12, 35), ty = clamp(reachY, -33, -12);
        body.elbow1 = {3, -22}; body.hand1 = {7, -27};
        body.foot1 = {-8, 0}; body.foot2 = {9, 0};
        if (kick) {
            body.knee2 = {3 + extend / 3, -5 - extend};
            body.foot2 = {9 + extend, ty * extend / 13};
            body.elbow2 = {8 - extend, -20 + extend / 3};
            body.hand2 = {8 - extend, ty + 14 * extend / 13};
        } else {
            body.elbow2 = {8, -20}; body.hand2 = {8 + extend, ty};
        }
        const int impact = max(0, extend - 9) * 55;
        p.spark(tx, ty, 4, White, opacity(f.propOpacity, impact));
        p.line(tx + 3, ty - 5, tx + 7, ty - 8, Gold, 1, opacity(f.propOpacity, impact));
        p.line(tx + 4, ty + 4, tx + 9, ty + 6, Gold, 1, opacity(f.propOpacity, impact));
        break;
    }
    case 6: {
        run(body, sine(t, 400, 7), true);
        p.line(body.head.x - 3, body.head.y - 2, body.head.x + 3, body.head.y - 2, Blue, 2);
        p.line(body.head.x - 3, body.head.y - 2, body.head.x - 10, body.head.y - 1 + fast / 3, Blue);
        for (int i = 0; i < 4; ++i) p.line(-22 - i * 4, -8 - i * 5, -13 - i * 4, -8 - i * 5, Muted);
        break;
    }
    case 7: { // Handle is mechanically connected to the actual odometer edge.
        const int cx = 18, cy = -19;
        const int hx = cx + cosine(t, 1200, 6), hy = cy + sine(t, 1200, 6);
        lean(body, 3); body.elbow2 = {9, -18}; body.hand2 = {hx, hy};
        body.elbow1 = {4, -15}; body.hand1 = {10, -13};
        p.circle(cx, cy, 7, Amber); p.circle(cx, cy, 2, White, true);
        p.line(cx, cy, hx, hy, White, 2); p.circle(hx, hy, 2, Wood, true);
        const int connectX = f.hasNumber ? reachX : 30;
        p.line(cx, cy, connectX, cy, Wood, 2);
        if (f.hasNumber) p.line(connectX, cy, connectX, reachY, Wood, 2);
        p.line(16, -12, 16, 0, Wood); p.line(11, 0, 24, 0, Wood);
        break;
    }
    case 8: { // Individual live reel windows and a completely exterior lever.
        if (!f.hasNumber) break;
        const SlotGeometry g = slotGeometry(f);
        // Cabinet decoration stays sized to the font, even if the figure is scaled.
        const Painter machine = {ctx, canvas.x1, canvas.y1, 256, 1, f.propOpacity};
        machine.rect(g.cabinet.x1, g.cabinet.y1, g.cabinet.x2, g.cabinet.y2,
                     Amber, false, 3);
        for (size_t i = 0; f.reels && i < f.reelCount; ++i) {
            const lv_area_t &reel = f.reels[i];
            // An inset leaves a dark gutter between neighboring numeric reels.
            machine.rect(reel.x1 + 1, reel.y1 - 1, reel.x2 - 1, reel.y2 + 1,
                         Gold, false, 2);
        }
        const int bulbs = min(12, max(2, (g.cabinet.x2 - g.cabinet.x1) / 16));
        for (int i = 0; i <= bulbs; ++i) {
            const int x = g.cabinet.x1 + 3 + (g.cabinet.x2 - g.cabinet.x1 - 6) * i / bulbs;
            const lv_opa_t glow = opacity(f.propOpacity, 145 + sine(t + i * 130, 1000, 85));
            machine.rect(x, g.cabinet.y1, x + 1, g.cabinet.y1 + 1,
                         Gold, true, 0, glow);
            machine.rect(x, g.cabinet.y2 - 1, x + 1, g.cabinet.y2,
                         Gold, true, 0, glow);
        }
        // The mount, shaft and knob never enter a reel or cross a digit.
        machine.line(g.cabinet.x2, g.cabinet.y2 - 2, g.pivot.x, g.pivot.y, Wood);
        machine.circle(g.pivot.x, g.pivot.y, g.radius, Amber, true);
        machine.line(g.pivot.x, g.pivot.y, g.knob.x, g.knob.y, White,
                     g.radius > 1 ? 2 : 1);
        machine.circle(g.knob.x, g.knob.y, g.radius, Red, true);
        body.hand2 = {p.localX(g.knob.x, canvas), p.localY(g.knob.y, canvas)};
        body.elbow2 = {body.hand2.x / 2, (body.hand2.y - 18) / 2};
        body.elbow1 = {-4, -13}; body.hand1 = {-5, -8};
        break;
    }
    case 9: { // Guard, jab, cross and bouncing footwork.
        const int jab = max(0, sine(t, 900, 12));
        lean(body, 2, max(0, fast) / 3);
        body.elbow1 = {-5, -19}; body.hand1 = {0, -26};
        body.elbow2 = {7, -22}; body.hand2 = {10, -24};
        // Each punch returns to guard before the other arm takes its turn.
        if ((t / 900) % 2) {
            body.elbow1 = {-5 + jab, -19 - jab / 4};
            body.hand1 = {22 * jab / 12, -26 + jab / 6};
        } else {
            body.hand2.x += jab;
        }
        body.foot1 = {-8, 0}; body.foot2 = {9, -max(0, -fast) / 2};
        p.circle(body.hand1.x, body.hand1.y, 2, Red, false);
        p.circle(body.hand2.x, body.hand2.y, 2, Red, false);
        const lv_opa_t trail = opacity(f.propOpacity, jab * 16);
        p.line(13, -29, 19, -29, Muted, 1, trail);
        p.line(14, -20, 20, -20, Muted, 1, trail);
        break;
    }
    case 10: { // Independent, tumbling drops shower the entire content area.
        const int hop = max(0, sine(t, 900, 4));
        lean(body, wave / 2, hop);
        body.elbow1 = {-7, -23}; body.hand1 = {-11, -31 + wave / 2};
        body.elbow2 = {7, -23}; body.hand2 = {11, -31 - wave / 2};
        Painter rain = {ctx, canvas.x1, canvas.y1, 256, 1, f.propOpacity};
        for (int i = 0; i < 24; ++i) {
            const uint32_t stream = mixBits(f.seed ^ (i + 1) * 0x9e3779b9U);
            const uint32_t period = 3200 + stream % 1500;
            const uint64_t clock = static_cast<uint64_t>(t) + mixBits(stream) % period;
            const uint32_t age = clock % period;
            uint32_t bits = mixBits(stream ^ static_cast<uint32_t>(clock / period) * 0x85ebca6bU);
            const auto next = [&]() { bits = mixBits(bits + 0x9e3779b9U); return bits; };
            const int radius = clamp(p.s(2 + next() % 3), 1, 6);
            const uint32_t flight = period * (68 + next() % 26) / 100;
            if (age >= flight) continue;

            // Broad bands ensure coverage; fresh random positions within
            // each band avoid fixed columns. A drop changes its parameters
            // only after it has passed completely below the screen.
            const int band = i % 6;
            const int left = f.width * band / 6;
            const int right = f.width * (band + 1) / 6;
            const int startX = left + next() % max(1, right - left);
            const int driftRange = max(2, f.width / 14);
            const int drift = static_cast<int>(next() % (2 * driftRange + 1)) - driftRange;
            const int wobble = 2 + next() % 4;
            const uint32_t swayPeriod = 1300 + next() % 1500;
            const uint32_t swayPhase = next() % swayPeriod;
            const uint32_t spinPeriod = 550 + next() % 1150;
            const uint32_t spinPhase = next() % spinPeriod;
            const uint32_t colors[] = {Gold, 0xffd879, 0xeec15a};
            const uint32_t color = colors[next() % 3];
            const int u = age * 1024 / flight;
            const int fall = u * (3072 + u) / 4096; // A little gravity.
            const int xx = startX + drift * u / 1024 +
                           sine(age + swayPhase, swayPeriod, wobble) -
                           sine(swayPhase, swayPeriod, wobble);
            const int yy = -radius - 2 + (f.height + 2 * radius + 4) * fall / 1024;
            const int halfWidth = abs(cosine(age + spinPhase, spinPeriod, radius));
            if (halfWidth == 0)
                rain.line(xx, yy - radius, xx, yy + radius, color);
            else
                rain.rect(xx - halfWidth, yy - radius, xx + halfWidth, yy + radius,
                          color, false, LV_RADIUS_CIRCLE);
            if (halfWidth >= 2)
                rain.line(xx, yy - max(1, radius / 2), xx, yy + max(1, radius / 2), Amber);
        }
        break;
    }
    case 11: {
        lean(body, wave, max(0, fast) / 3);
        body.elbow1 = {7, -20}; body.hand1 = {14 + wave, -21};
        body.elbow2 = {8, -13}; body.hand2 = {15 + wave, -12};
        body.foot1 = {-6 - wave, 0}; body.foot2 = {7 + wave, -max(0, fast)};
        broom(p, 19 + wave, 0, 7 + wave);
        p.note(-16, -26 - wave); p.note(29, -35 + wave);
        break;
    }
    case 12: { // Throw, chase, collect and return the ball for another throw.
        const DogFetchFrame fetch = dogFetch(t);
        lean(body, 5);
        body.elbow2 = {9, -13 + min(0, fetch.hand.y + 14) / 2};
        body.hand2 = fetch.hand;
        body.elbow1 = {-3, -13}; body.hand1 = {-4, -7};
        Painter puppy = p.offset(fetch.dogX, 0);
        puppy.horizontalScale = fetch.facingQ8;
        dog(puppy, t, fetch.bow, fetch.gait);
        const int ballX = fetch.carrying ? puppy.wx(16) : p.wx(fetch.ball.x);
        const int ballY = p.wy(fetch.ball.y);
        const int radius = max(1, p.s(2));
        p.realRect(ballX - radius, ballY - radius, ballX + radius, ballY + radius,
                   Red, true, f.propOpacity, LV_RADIUS_CIRCLE);
        p.realLine(ballX - radius / 2, ballY - radius / 2,
                   ballX + radius / 2, ballY + radius / 2,
                   White, opacity(f.propOpacity, 170));
        break;
    }
    case 13: { // The seated cat watches a fishing-wand toy and bats it with a paw.
        const int sway = sine(t, 2400, 3);
        const int swipe = max(0, sine(t, 2400, 255));
        lean(body, 3); body.elbow2 = {8, -20}; body.hand2 = {14, -25 + sway / 2};
        p.line(body.hand2.x, body.hand2.y, 23, -32 + sway / 2, Wood);
        p.line(23, -32 + sway / 2, 26 - sway, -19, White);
        p.circle(26 - sway, -18, 2, Red, true);
        cat(p.offset(36, 0, -1), t, swipe);
        break;
    }
    case 14: { // Reclining, breathing pose and drifting Zs above the pillow.
        p.rect(-15, -11, 26, -4, Wood, false, 2, -1, 2);
        p.line(-15, -17, -15, 1, Wood, 2); p.line(26, -8, 26, 1, Wood, 2);
        p.rect(-13, -16, -2, -10, White, false, 2);
        const int breath = sine(t, 1800, 1);
        body.head = {-8, -15}; body.shoulder = {0, -12 + breath}; body.hip = {11, -10};
        body.elbow1 = {2, -8}; body.hand1 = {7, -10};
        body.elbow2 = {1, -16}; body.hand2 = {6, -14};
        body.knee1 = {17, -8}; body.foot1 = {23, -8};
        body.knee2 = {17, -11}; body.foot2 = {23, -10}; body.closedEyes = true;
        p.rect(3, -13 + breath, 24, -5, Blue, false, 2);
        for (int i = 0; i < 3; ++i) {
            const int age = (t + i * 750) % 2400;
            const int a = min(255, min(age, 2400 - age) / 3);
            p.z(-10 + age / 220, -23 - age / 120, 3 + i,
                opacity(f.propOpacity, a));
        }
        break;
    }
    case 15: {
        sit(body); chair(p);
        body.elbow1 = {6, -21}; body.hand1 = {12, -20 + fast / 5};
        body.elbow2 = {10, -23}; body.hand2 = {18, -20 - fast / 5};
        p.rect(27, -36, 48, -19, White, false, 2, -1, 2);
        p.line(37, -19, 37, -12, Wood); p.line(31, -12, 43, -12, Wood);
        p.rect(30 + ramp(t, 1600, 11), -25, 34 + ramp(t, 1600, 11), -22, Green, true);
        p.rect(29, -21, 46, -20, Blue, true);
        p.rect(11, -22, 20, -18, Purple, false, 2);
        p.line(12, -20, 15, -20, White); p.line(13, -21, 13, -19, White);
        p.circle(18, -20, 1, Red, true);
        p.line(20, -19, 26, -19, Muted); p.line(26, -19, 27, -26, Muted);
        break;
    }
    case 16: {
        sit(body, true);
        p.line(-10, -28, -1, -13, Blue, 2); p.line(-1, -13, 13, -13, Blue, 2);
        p.line(-9, -27, 10, 0, Wood); p.line(11, -13, -8, 0, Wood);
        p.line(-4, -20, 14, -20, Wood); p.line(14, -20, 16, -13, Wood);
        body.head.y += max(0, sine(t, 2400, 2));
        body.elbow2 = {9, -20}; body.hand2 = {body.head.x + 1, body.head.y + 4};
        const int age = t % 1500;
        p.circle(12, -24 + age / 130, 1, Blue, true,
                 opacity(f.propOpacity, 190 - age / 8));
        p.line(21, -37, 25, -37, Muted, 1, opacity(f.propOpacity, 120));
        break;
    }
    case 17: {
        lean(body, 1);
        const int lift = max(0, sine(t, 2600, 11));
        body.elbow2 = {7, -14 - lift / 2}; body.hand2 = {7, -12 - lift};
        p.line(7, -12 - lift, 13, -12 - lift, White);
        p.rect(13, -12 - lift, 14, -12 - lift, Red, true);
        for (int i = 0; i < 5; ++i) {
            const int age = (t + i * 520) % 2600;
            const int xx = 13 + sine(age + i * 100, 2000, 3);
            const int yy = -15 - lift - age / 150;
            p.circle(xx, yy, 1 + age / 1100, Muted, false,
                     opacity(f.propOpacity, 170 - age / 18));
        }
        break;
    }
    case 18: { // Coffee machine fills the mug before it moves toward the mouth.
        p.rect(23, -25, 39, -9, Wood, false, 2, -1, 2);
        p.rect(26, -22, 35, -17, White, false, 1);
        p.line(26, -14, 34, -14, Amber); p.line(27, -9, 27, -5, Wood);
        p.line(38, -9, 38, -5, Wood); p.line(22, -5, 42, -5, Wood);
        const int lift = max(0, sine(t + 3200, 6400, 15));
        const int mugX = 27 - lift * 20 / 15, mugY = -9 - lift;
        lean(body, lift ? 1 : 5);
        body.elbow2 = {8, -14 - lift / 3}; body.hand2 = {mugX - 1, mugY};
        body.elbow1 = {6, -17}; body.hand1 = {11, -17};
        p.rect(mugX, mugY - 3, mugX + 6, mugY + 3, White, false, 1);
        p.circle(mugX + 7, mugY, 2, White);
        if (lift < 2) p.line(30, -14, 30, -9, Amber, 1,
                            opacity(f.propOpacity, 110 + sine(t, 300, 100)));
        for (int i = 0; i < 2; ++i)
            p.line(mugX + 1 + i * 3, mugY - 6, mugX + 2 + i * 3 + wave / 3,
                   mugY - 10, Muted, 1, opacity(f.propOpacity, 120));
        break;
    }
    case 19: {
        lean(body, 3); body.elbow2 = {9, -16}; body.hand2 = {17, -16 + wave / 2};
        body.elbow1 = {6, -22}; body.hand1 = {20, -24 + fast / 2};
        p.rect(21, -11, 43, -1, Wood, false, 1, -1, 2);
        p.circle(27, -5, 1, Red, true); p.circle(36, -5, 1, White, true);
        p.line(22, -13, 42, -13, Red); p.line(25, -14, 28, -18, Amber);
        p.line(35, -14, 37, -18, Amber);
        p.line(16, -16 + wave / 2, 23, -16, White, 2);
        p.rect(23, -19, 40, -15, White, false, LV_RADIUS_CIRCLE);
        p.circle(30, -20 - max(0, fast), 2, Green, true);
        p.circle(35, -21 - max(0, -fast), 1, Red, true);
        p.line(20, -24 + fast / 2, 30, -18, Wood);
        for (int i = 0; i < 3; ++i) p.line(26 + i * 5, -26 + wave / 2, 28 + i * 5, -30 + wave / 2, Muted);
        break;
    }
    case 20: { // A cool-colored opponent parries with an independently timed sword.
        lean(body, 3); body.foot1 = {-8, 0}; body.foot2 = {9, 0};
        body.elbow2 = {9, -20}; body.hand2 = {14, -22 + wave};
        body.elbow1 = {-5, -17}; body.hand1 = {-7, -22};
        p.line(14, -22 + wave, 31, -33 - wave, White, 2);
        p.line(12, -25 + wave, 17, -19 + wave, Amber, 2);
        Painter opponent = p.offset(42, 0, -1);
        Pose other; lean(other, 3); other.foot1 = {-7, 0}; other.foot2 = {9, 0};
        other.elbow2 = {8, -20}; other.hand2 = {14, -23 - wave};
        other.elbow1 = {-5, -17}; other.hand1 = {-7, -21};
        opponent.body(other, f.propOpacity, Blue);
        opponent.line(14, -23 - wave, 28, -33 + wave, White, 2);
        opponent.line(12, -26 - wave, 17, -20 - wave, Purple, 2);
        p.spark(25, -32, 3, Gold, opacity(f.propOpacity, max(0, 120 - abs(wave) * 30)));
        break;
    }
    case 21: { // Enter, meet, cross the screen, then blow one last heart back.
        const int phase = min(12000, static_cast<int>(static_cast<uint64_t>(t) * 12000 /
                              (f.duration ? f.duration : 12000)));
        const auto progress = [&](int begin, int end) {
            return clamp((phase - begin) * 255 / (end - begin), 0, 255);
        };
        const auto travel = [&](int from, int to, int begin, int end) {
            const int u = progress(begin, end);
            const int eased = u * u * (765 - 2 * u) / 65025;
            return from + (to - from) * eased / 255;
        };
        // These are root pixels: both entrances are genuinely beyond the
        // content edge, including scaled figures and roots with padding.
        const int start = p.facing > 0 ? f.width + p.s(18) : -p.s(18);
        const int finish = p.facing > 0 ? -p.s(18) : f.width + p.s(18);
        const int meet = f.x + p.facing * p.s(26);
        const int exitRoom = p.facing > 0 ? f.x : f.width - 1 - f.x;
        const int farewell = f.x - p.facing * min(p.s(72), max(0, exitRoom - p.s(22)));
        const int girlX = phase < 2800 ? travel(start, meet, 0, 2800) :
                          phase < 5200 ? meet :
                          phase < 8400 ? travel(meet, farewell, 5200, 8400) :
                          phase < 9400 ? farewell : travel(farewell, finish, 9400, 11500);
        int walking = 0;
        if (phase < 2800) walking = min(progress(0, 350), 255 - progress(2450, 2800));
        else if (phase >= 5200 && phase < 8400)
            walking = min(progress(5200, 5550), 255 - progress(8050, 8400));
        else if (phase >= 9400)
            walking = min(progress(9400, 9750), 255 - progress(11150, 11500));

        Pose girl;
        const int stride = roundedSine(phase, 800, 6);
        lean(girl, 1, abs(stride) / 4);
        girl.knee1 = {stride / 2, -5}; girl.foot1 = {stride, -max(0, roundedSine(phase, 800, 2))};
        girl.knee2 = {-stride / 2, -5}; girl.foot2 = {-stride, -max(0, -roundedSine(phase, 800, 2))};
        girl.elbow1 = {-3 - stride / 2, -14}; girl.hand1 = {-2 - stride, -9};
        girl.elbow2 = {3 + stride / 2, -14}; girl.hand2 = {2 + stride, -9};
        blendPose(girl, Pose{}, walking);

        const int holding = min(progress(2400, 2800), 255 - progress(5200, 5500));
        Pose joined = girl;
        joined.elbow2 = {8, -17}; joined.hand2 = {13, -13};
        blendPose(joined, girl, holding); girl = joined;
        Pose greeting = body;
        greeting.elbow2 = {8, -17}; greeting.hand2 = {13, -13};
        blendPose(greeting, body, holding); body = greeting;

        const bool lookingBack = phase >= 8400 && phase < 9400;
        Painter partner = p;
        partner.ox = canvas.x1 + girlX;
        partner.facing *= lookingBack ? 1 : -1;
        // Pass behind him in a slightly higher walking lane, returning to
        // the shared ground before the farewell pose.
        const int crossing = min(progress(5300, 5800), 255 - progress(7500, 8200));
        partner.oy -= p.s(2) * crossing / 255;
        if (lookingBack) {
            Pose kiss = girl;
            const int extend = progress(8800, 9200);
            kiss.elbow2 = {6, -19};
            kiss.hand2 = {4 + extend * 7 / 255, -24 + extend * 2 / 255};
            blendPose(kiss, girl, progress(8400, 8650)); girl = kiss;
        }
        partner.body(girl, f.propOpacity, Purple);
        partner.line(girl.head.x - 3, girl.head.y - 2,
                     girl.head.x - 5, girl.head.y + 6, Purple, 2);
        partner.line(girl.head.x - 4, girl.head.y - 1,
                     girl.head.x - 7, girl.head.y + 3, Purple, 2);
        partner.line(girl.shoulder.x, girl.shoulder.y,
                     girl.hip.x - 5, girl.hip.y + 2, Purple);
        partner.line(girl.shoulder.x, girl.shoulder.y,
                     girl.hip.x + 5, girl.hip.y + 2, Purple);
        partner.line(girl.hip.x - 5, girl.hip.y + 2,
                     girl.hip.x + 5, girl.hip.y + 2, Purple);

        if (phase >= 2900 && phase < 5100) {
            const int age = phase - 2900;
            const int alpha = min(255, min(age, 2200 - age) * 255 / 350);
            p.heart(13, -20 - age * 9 / 2200, 5, opacity(f.propOpacity, alpha));
        }
        if (phase >= 9000 && phase < 11800) {
            // A fixed launch point lets the heart keep flying toward him
            // while she resumes walking off the opposite edge.
            Painter heart = p;
            const int launch = farewell + p.facing * p.s(10);
            const int destination = f.x - p.facing * p.s(7);
            heart.ox = canvas.x1 + travel(launch, destination, 9000, 11500);
            heart.oy = p.wy(-24) - p.s(sine((phase - 9000), 5000, 3));
            const int alpha = min(progress(9000, 9200), 255 - progress(11400, 11800));
            heart.heart(0, 0, 5, opacity(f.propOpacity, alpha));
        }
        if ((girlX - f.x) * p.facing < 0) {
            p.facing = -p.facing;
            Pose goodbye = body;
            goodbye.elbow2 = {7, -20}; goodbye.hand2 = {11, -26 + wave / 2};
            blendPose(goodbye, body, progress(7600, 8100)); body = goodbye;
            body.closedEyes = phase >= 11200;
        }
        break;
    }
    case 22: {
        body.hip = {-2, -13}; body.shoulder = {5, -17}; body.head = {12, -20};
        body.knee1 = {-10, -8}; body.foot1 = {-17, -10 + wave};
        body.knee2 = {-9, -13}; body.foot2 = {-17, -15 - wave / 2};
        body.elbow2 = {12, -17}; body.hand2 = {24, -20};
        body.elbow1 = {-2, -19}; body.hand1 = {-11, -23 + wave};
        p.line(0, -19, -17, -25 + wave, Red, 2);
        p.line(-17, -25 + wave, -14, -16, Red, 2); p.line(-14, -16, -2, -12, Red);
        for (int i = 0; i < 3; ++i) p.line(-30 - i * 5, -8 - i * 5, -22 - i * 5, -8 - i * 5, Muted);
        break;
    }
    case 23: { // Fixed random endpoints; relocate only while fully invisible.
        const int phase = teleportPhase(t, f.duration);
        const int fade = phase < 2000 ? 255 : phase < 3000 ? (3000 - phase) * 255 / 1000 :
                         phase < TeleportArrivalPhase ? 0 :
                         phase < 4400 ? (phase - TeleportArrivalPhase) * 255 / 1000 : 255;
        bodyOpacity = opacity(f.opacity, fade);
        body.elbow1 = {-8, -20}; body.hand1 = {-11, -26};
        body.elbow2 = {8, -20}; body.hand2 = {11, -26};
        const Point destination = f.hasTeleportTarget ?
            Point{f.teleportTarget.x, f.teleportTarget.y} : teleportDestination(f);
        Painter portal = p;
        portal.ox = canvas.x1 + destination.x;
        portal.oy = canvas.y1 + destination.y;
        const int glow = 80 + max(0, sine(t, 1600, 150));
        p.rect(-13, -37, 13, 1, Blue, false, LV_RADIUS_CIRCLE, opacity(f.propOpacity, glow), 2);
        p.rect(-17, -40, 17, 3, Purple, false, LV_RADIUS_CIRCLE, opacity(f.propOpacity, glow / 2));
        portal.rect(-13, -37, 13, 1, Purple, false, LV_RADIUS_CIRCLE, opacity(f.propOpacity, glow), 2);
        portal.rect(-17, -40, 17, 3, Blue, false, LV_RADIUS_CIRCLE, opacity(f.propOpacity, glow / 2));
        for (int i = 0; i < 7; ++i) {
            const int xx = cosine(t + i * 180, 1600, 15);
            const int yy = -18 + sine(t + i * 180, 1600, 20);
            p.spark(xx, yy, 1, Blue, opacity(f.propOpacity, glow));
            portal.spark(-xx, yy, 1, Purple, opacity(f.propOpacity, glow));
        }
        if (phase >= TeleportArrivalPhase) p = portal;
        p.rect(-7, 1, 7, 2, Wood, true, LV_RADIUS_CIRCLE, opacity(bodyOpacity, 75));
        break;
    }
    case 24: { // Each instrument fades away before the next one arrives.
        const int instrument = (t / 3000) % 3;
        const int age = t % 3000;
        const lv_opa_t instrumentOpa = opacity(f.propOpacity, min(255, min(age, 3000 - age)));
        Painter music = p; music.props = instrumentOpa;
        body = musician(instrument, fast);
        if (instrument == 0) {
            chair(music);
            music.rect(12, -15, 26, -7, Red, false, 2, -1, 2);
            music.line(19, -7, 17, 0, Wood); music.line(19, -7, 24, 0, Wood);
            music.circle(33, -19, 7, Amber); music.line(33, -19, 33, 0, Wood);
            music.line(body.hand1.x, body.hand1.y, 18, -16, White);
            music.line(body.hand2.x, body.hand2.y, 32, -19, White);
        } else if (instrument == 1) {
            music.circle(10, -13, 6, Wood, false, -1, 2);
            music.circle(7, -17, 4, Wood, false, -1, 2);
            music.line(11, -17, 27, -28, Amber, 3); music.circle(10, -14, 2, Muted, true);
            music.line(9, -15, 27, -28, White); music.line(24, -28, 27, -30, Wood, 3);
        } else {
            music.line(4, -25, 10, -22, Amber, 2); music.line(10, -22, 15, -10, Amber, 3);
            music.line(15, -10, 21, -11, Amber, 3); music.line(21, -11, 24, -17, Amber, 3);
            music.line(20, -17, 27, -17, Amber, 2);
            for (int i = 0; i < 3; ++i) music.circle(12 + i, -19 + i * 3, 1, White, true);
        }
        if (t >= 3000 && age < 400)
            blendPose(body, musician((instrument + 2) % 3, fast), age * 255 / 400);
        p.note(-16 + wave, -28 - ramp(t, 1900, 9));
        p.note(33 - wave, -33 - ramp(t + 600, 2100, 9));
        break;
    }
    case 25: { // Root places the feet below the top edge of the number.
        sit(body); body.foot1.y = 1 + wave / 2; body.foot2.y = 2 - wave / 2;
        body.elbow1 = {-5, -17}; body.hand1 = {-7, -13};
        body.elbow2 = {7, -17}; body.hand2 = {10, -13};
        p.line(-7, -13, -9, -13, Gold, 2, f.opacity);
        p.line(10, -13, 13, -13, Gold, 2, f.opacity);
        p.note(19, -28 + wave, opacity(f.propOpacity, 140));
        break;
    }
    case 26: { // Draw a recognizable little landscape one stroke at a time.
        lean(body, 3); body.elbow2 = {8, -21}; body.hand2 = {13, -25 + wave};
        p.line(13, -25 + wave, 18, -29 + wave, White);
        const int ink = min(255, static_cast<int>(t / 8));
        Painter art = p; art.props = opacity(f.propOpacity, ink);
        art.rect(19, -40, 52, -11, Muted, false, 1);
        const Point lines[][2] = {{{22,-15},{31,-26}},{{31,-26},{39,-15}},
                                 {{35,-15},{44,-24}},{{44,-24},{49,-15}},
                                 {{22,-15},{49,-15}}};
        for (int i = 0; i < 5; ++i) {
            const int amount = clamp(static_cast<int>(t) - i * 700, 0, 700);
            if (amount == 0) continue;
            const Point a = lines[i][0], b = lines[i][1];
            art.line(a.x, a.y, a.x + (b.x - a.x) * amount / 700,
                     a.y + (b.y - a.y) * amount / 700, Green);
        }
        const int sunOpa = clamp(static_cast<int>(t) - 3500, 0, 255);
        art.circle(43, -33, 3, Gold, false, opacity(art.props, sunOpa));
        p.rect(-9, -16, -3, -8, Wood, false, 2);
        p.circle(-6, -12, 1, Red, true); p.circle(-4, -10, 1, Blue, true);
        break;
    }
    case 27: { // Articulated, travelling floor routine; actor movement is shared.
        body = gymnasticsPose(gymnasticsMotion(t, f.duration));
        break;
    }
    case 28: {
        lean(body, 4); body.elbow2 = {10, -19}; body.hand2 = {16, -17 + wave / 2};
        p.rect(15, -21 + wave / 2, 23, -14 + wave / 2, Blue, false, 2);
        p.circle(14, -19 + wave / 2, 3, Blue);
        p.line(23, -17 + wave / 2, 29, -14, Blue, 2);
        const int growth = 5 + min(14, static_cast<int>(t / 500));
        plant(p, 34, growth); plant(p, 49, max(3, growth - 4));
        for (int i = 0; i < 4; ++i) {
            const int age = (t + i * 210) % 950;
            p.line(29 + age / 140, -12 + age / 120, 30 + age / 140, -11 + age / 120,
                   Blue, 1, opacity(f.propOpacity, 230 - age / 6));
        }
        break;
    }
    case 29: {
        lean(body, 3); body.head = {5, -23};
        body.elbow2 = {8, -16}; body.hand2 = {11, -21};
        body.elbow1 = {4, -17}; body.hand1 = {11 + fast / 5, -19};
        p.rect(10, -26, 15, -17, White, false, 1);
        p.rect(11, -24, 14, -19, Blue, true, 0, opacity(f.propOpacity, 130));
        p.circle(12, -18, 1, Muted, true);
        const int age = t % 2400;
        p.rect(18, -34 - age / 800, 29, -29 - age / 800, Blue, false, 2,
               opacity(f.propOpacity, min(age, 2400 - age) / 3));
        p.line(19, -29 - age / 800, 17, -27 - age / 800, Blue, 1,
               opacity(f.propOpacity, min(age, 2400 - age) / 3));
        break;
    }
    case 30: { // Dance in place while confetti falls across the entire stage.
        const int hop = max(0, sine(t, 700, 6));
        lean(body, wave, hop);
        body.elbow1 = {-7, -23 - hop}; body.hand1 = {-12 - wave, -30 - hop};
        body.elbow2 = {7, -23 - hop}; body.hand2 = {12 - wave, -30 - hop};
        body.foot1 = {-6 - wave, -hop}; body.foot2 = {6 + wave, -hop};
        Painter confetti = {ctx, canvas.x1, canvas.y1, 256, 1, f.propOpacity};
        for (int i = 0; i < 24; ++i) {
            const uint32_t stream = mixBits(f.seed ^ (i + 1) * 0x9e3779b9U);
            const uint32_t period = 2400 + stream % 1500;
            const uint64_t clock = static_cast<uint64_t>(t) + mixBits(stream) % period;
            const uint32_t age = clock % period;
            uint32_t bits = mixBits(stream ^ static_cast<uint32_t>(clock / period) * 0x85ebca6bU);
            const auto next = [&]() { bits = mixBits(bits + 0x9e3779b9U); return bits; };
            const int size = clamp(p.s(1 + next() % 2), 1, 4);
            const uint32_t flight = period * (75 + next() % 20) / 100;
            if (age >= flight) continue;

            // Cover the width with independent streams. New paths start only
            // after the previous piece has fallen completely off the stage.
            const int band = i % 6;
            const int left = f.width * band / 6;
            const int right = f.width * (band + 1) / 6;
            const int startX = left + next() % max(1, right - left);
            const int driftRange = max(2, f.width / 16);
            const int drift = static_cast<int>(next() % (2 * driftRange + 1)) - driftRange;
            const uint32_t swayPeriod = 1000 + next() % 1400;
            const uint32_t swayPhase = next() % swayPeriod;
            const uint32_t spinPeriod = 500 + next() % 800;
            const uint32_t spinPhase = next() % spinPeriod;
            const uint32_t colors[] = {Red, Blue, Gold};
            const uint32_t color = colors[next() % 3];
            const int progress = age * 1024 / flight;
            const int xx = startX + drift * progress / 1024 +
                           sine(age + swayPhase, swayPeriod, 4) - sine(swayPhase, swayPeriod, 4);
            const int yy = -size - 2 + (f.height + 2 * size + 4) * progress / 1024;
            const int dx = sine(age + spinPhase, spinPeriod, size);
            const int dy = cosine(age + spinPhase, spinPeriod, size);
            const int fade = min(255, static_cast<int>(age < flight - age ? age : flight - age));
            confetti.line(xx - dx, yy - dy, xx + dx, yy + dy, color, 1,
                          opacity(f.propOpacity, fade));
        }
        break;
    }
    case 31: {
        lean(body, wave / 2, max(0, fast) / 3);
        body.elbow1 = {-7, -22}; body.hand1 = {-11, -26 + wave};
        body.elbow2 = {8, -22}; body.hand2 = {13, -28 - wave};
        p.rect(11, -32 - wave, 16, -27 - wave, White, false, 1);
        p.line(-44, -37, 45, -37, Wood);
        for (int i = 0; i < 7; ++i) {
            const int x = -41 + i * 13;
            p.line(x, -37, x + 5, -31, i % 2 ? Red : Blue);
            p.line(x + 5, -31, x + 10, -37, i % 2 ? Red : Blue);
        }
        for (int i = 0; i < 3; ++i) {
            Painter guest = p.offset(i == 0 ? -32 : i == 1 ? 31 : 49, 0, i == 0 ? 1 : -1);
            Pose dancer;
            const int beat = sine(t + i * 230, 850, 4);
            lean(dancer, beat / 2, max(0, beat) / 2);
            dancer.elbow1 = {-7, -21}; dancer.hand1 = {-11, -27 + beat};
            dancer.elbow2 = {7, -21}; dancer.hand2 = {11, -27 - beat};
            dancer.foot1 = {-5 - beat, 0}; dancer.foot2 = {5 + beat, 0};
            guest.body(dancer, opacity(f.propOpacity, 210), i % 2 ? Purple : Blue);
        }
        p.rect(-50, -22, -42, 0, Muted, false, 1);
        p.circle(-46, -6, 3, Blue); p.circle(-46, -17, 2, Blue);
        p.note(-20, -29 + wave); p.note(23, -42 - wave);
        break;
    }
    case 32: {
        sit(body); chair(p);
        body.head.y += sine(t, 2400, 2);
        body.elbow1 = {6, -20}; body.hand1 = {12, -24};
        body.elbow2 = {10, -18}; body.hand2 = {26, -22};
        const int flutter = sine(t, 1800, 3);
        p.rect(11, -29, 28, -15 + flutter, White, false, 1);
        p.line(19, -29, 19, -15 + flutter, Muted);
        p.line(13, -26, 17, -26, Gold, 2); p.line(21, -26, 26, -26, Gold, 2);
        for (int row = 0; row < 3; ++row) {
            p.line(13, -23 + row * 2, 17, -23 + row * 2, Muted);
            p.line(21, -23 + row * 2, 26, -23 + row * 2, Muted);
        }
        p.rect(21, -22, 24, -19, Blue, false, 0);
        break;
    }
    case ANIMATION_TREASURE: {
        // Four shovel loads travel from the hole to the same growing mound.
        // The final load exposes a chest; the shovel rests while he celebrates.
        const int phase = static_cast<int>(t < 12000 ? t : 12000);
        const int work = clamp(phase - 600, 0, 7200);
        const int cycle = min(3, work / 1800);
        const int stroke = work < 7200 ? work % 1800 : 1800;
        const auto mix = [](int a, int b, int amount) { return a + (b - a) * amount / 256; };
        const auto between = [&](Point a, Point b, int amount) {
            return Point{mix(a.x, b.x, amount), mix(a.y, b.y, amount)};
        };
        const int times[] = {0, 450, 800, 1100, 1250, 1350, 1800};
        const Point grips[] = {{5,-26},{12,-18},{13,-14},{4,-28},{5,-31},{5,-31},{5,-26}};
        const Point blades[] = {{19,-9},{22,-4},{24,-2},{23,-20},{32,-28},{32,-28},{19,-9}};
        const int bends[] = {3, 7, 10, 3, 1, 1, 3};
        int segment = 0;
        while (segment < 5 && stroke > times[segment + 1]) ++segment;
        const int amount = clamp((stroke - times[segment]) * 256 /
                                 (times[segment + 1] - times[segment]), 0, 256);
        Point grip = between(grips[segment], grips[segment + 1], amount);
        Point blade = between(blades[segment], blades[segment + 1], amount);
        lean(body, mix(bends[segment], bends[segment + 1], amount));
        body.foot1 = {-7, 0}; body.foot2 = {8, 0};
        body.knee1 = {-4, -5}; body.knee2 = {5, -5};
        body.hand1 = between(grip, blade, 40);
        body.hand2 = between(grip, blade, 145);
        body.elbow1 = {body.hand1.x - 5, body.hand1.y + 3};
        body.elbow2 = {body.hand2.x - 5, body.hand2.y - 2};

        const int dug = work >= 7200 ? 1024 :
            cycle * 256 + clamp((stroke - 450) * 256 / 350, 0, 256);
        const int deposited = work >= 7200 ? 1024 :
            cycle * 256 + clamp((stroke - 1600) * 256 / 180, 0, 256);
        const int moundHeight = deposited * 12 / 1024;
        const int moundWidth = 7 + deposited * 8 / 1024;
        p.line(-11, 2, 63, 2, Wood);
        const int holeWidth = 3 + dug * 8 / 1024;
        p.rect(24 - holeWidth, 0, 24 + holeWidth, 5, 0x241a14, true, LV_RADIUS_CIRCLE);
        p.rect(24 - holeWidth, 0, 24 + holeWidth, 5, Wood, false, LV_RADIUS_CIRCLE);
        if (deposited) {
            p.triangle({48 - moundWidth, 2}, {46, 2 - moundHeight},
                       {48 + moundWidth, 2}, Wood);
            p.triangle({43, 2}, {51, 3 - moundHeight * 3 / 4},
                       {48 + moundWidth, 2}, 0x695039);
            p.line(48 - moundWidth, 2, 46, 2 - moundHeight, 0xac8250);
            for (int i = 0; i < 6; ++i) {
                const int row = 1 + i % 3 * 2;
                if (moundHeight > row + 2) {
                    const int xx = 42 + i * 3;
                    p.rect(xx, 2 - row, xx + 1, 2 - row, i % 2 ? Muted : Amber, true);
                }
            }
        }
        // Clods leave the raised spade, arc down, and disappear into the pile.
        if (work < 7200) for (int i = 0; i < 4; ++i) {
            const int age = stroke - 1250 - i * 30;
            const int flight = 360 + i * 20;
            if (age < 0 || age >= flight) continue;
            const int progress = age * 256 / flight;
            const uint32_t bits = mixBits(f.seed ^ (cycle + 1) * 0x9e3779b9U ^ (i + 1) * 0x85ebca6bU);
            const int landing = 44 + bits % 11;
            const int xx = mix(38, landing, progress);
            const int yy = mix(-27, 2 - moundHeight, progress) -
                           4 * progress * (256 - progress) * (5 + i) / (256 * 256);
            p.circle(xx, yy, 1 + static_cast<int>((bits >> 8) & 1),
                     i % 2 ? Wood : 0xac8250, true);
        }

        const int rest = clamp((phase - 7800) * 256 / 700, 0, 256);
        if (rest) {
            grip = between(grip, {-15, -26}, rest);
            blade = between(blade, {-11, -4}, rest);
            Pose cheer;
            cheer.elbow1 = {-7, -20}; cheer.hand1 = {-10, -29 + wave / 2};
            cheer.elbow2 = {8, -21}; cheer.hand2 = {12, -29 - wave / 2};
            cheer.foot1 = body.foot1; cheer.foot2 = body.foot2;
            blendPose(cheer, body, rest * 255 / 256);
            body = cheer;
        }
        // A D grip, wooden shaft and broad pointed metal spade read separately.
        const int dx = blade.x - grip.x, dy = blade.y - grip.y;
        const int length = max(1, max(abs(dx), abs(dy)));
        const Point side = {-dy * 3 / length, dx * 3 / length};
        const Point tip = {blade.x + dx * 6 / length, blade.y + dy * 6 / length};
        p.line(grip.x, grip.y, blade.x, blade.y, Wood, 2);
        p.circle(grip.x, grip.y, 2, White);
        p.triangle({blade.x + side.x, blade.y + side.y},
                   {blade.x - side.x, blade.y - side.y}, tip, White);
        p.line(blade.x, blade.y, tip.x, tip.y, Muted);
        if (work < 7200 && stroke >= 800 && stroke < 1250)
            p.circle(tip.x, tip.y - 1, 2, Wood, true);

        if (phase > 7800) {
            Painter chest = p;
            chest.props = opacity(f.propOpacity, clamp((phase - 7800) * 255 / 700, 0, 255));
            const int rise = 7 - clamp((phase - 7800) * 7 / 1000, 0, 7);
            const int open = clamp((phase - 8700) * 256 / 900, 0, 256);
            chest.rect(15, -14 + rise - open * 9 / 256, 33, -9 + rise,
                       0x604025, true, 2);
            chest.rect(15, -14 + rise - open * 9 / 256, 33, -9 + rise,
                       Amber, false, 2);
            chest.line(18, -13 + rise - open * 9 / 256, 18, -10 + rise, Gold);
            chest.line(30, -13 + rise - open * 9 / 256, 30, -10 + rise, Gold);
            if (open) {
                const int goldAlpha = opacity(chest.props, min(255, open * 2));
                chest.rect(17, -13 + rise, 31, -9 + rise, 0x241a14, true);
                for (int i = 0; i < 5; ++i)
                    chest.circle(18 + i * 3, -11 + rise - (i % 2) * 2, 2, Gold, true, goldAlpha);
            }
            chest.rect(15, -9 + rise, 33, 2, 0x604025, true, 1);
            chest.rect(15, -9 + rise, 33, 2, Amber, false, 1);
            chest.line(18, -8 + rise, 18, 1, Gold);
            chest.line(30, -8 + rise, 30, 1, Gold);
            chest.rect(23, -7 + rise, 25, -4 + rise, Gold, true);
            if (phase >= 9500) {
                const int glow = 100 + sine(t, 1100, 90);
                chest.spark(13, -22, 3, Gold, opacity(chest.props, glow));
                chest.spark(36, -27, 3, Gold, opacity(chest.props, 255 - glow));
                chest.spark(25, -31, 2, White, opacity(chest.props, glow));
            }
        }
        break;
    }
    }

    p.body(body, bodyOpacity, Gold, f.id == ANIMATION_GYMNASTICS);
    ctx->clip_area = oldClip;
}

} // namespace StickScenes
