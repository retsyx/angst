#pragma once

#include <string>
#include <memory>
#include <lvgl.h>
#include <string.h>
#include <vector>
#include "text_bitmap.h"

#ifndef ODOMETER_ANIMATION_DURATION_MS
#ifdef CONFIG_DISPLAY_ANIMATION_DURATION_MS
#define ODOMETER_ANIMATION_DURATION_MS CONFIG_DISPLAY_ANIMATION_DURATION_MS
#else
#define ODOMETER_ANIMATION_DURATION_MS 5000
#endif
#endif

class OdometerDisplay {
public:
    void begin(lv_obj_t *screen, const lv_font_t *displayFont) {
        root = screen;
        font = displayFont;
        height = font->line_height;
        textLabel = makeLabel(root, "");
        lv_obj_set_size(textLabel, lv_pct(100), height);
        // Center relative to the case, accounting for the touch button.
        lv_obj_set_pos(textLabel, -15, (lv_obj_get_content_height(root) - 20 - height) / 2);

        for (char digit = '0'; digit <= '9'; ++digit) {
            char text[] = {digit, '\0'};
            lv_coord_t width = textWidth(text);
            if (width > digitWidth) digitWidth = width;
        }
        digitBitmapsReady = true;
        for (unsigned digit = 0; digit < 10; ++digit) {
            char text[] = {static_cast<char>('0' + digit), '\0'};
            if (!digitBitmaps[digit].render(text, font, digitWidth, LV_TEXT_ALIGN_CENTER, true)) {
                // A partial cache must never make a reel disappear. Fall back
                // to normal LVGL glyph rendering if memory is unavailable.
                digitBitmapsReady = false;
                for (TextBitmap &bitmap : digitBitmaps) bitmap.clear();
                break;
            }
        }
    }

    void setValue(const std::string &message) {
        std::string text = message;
        const auto first = text.find_first_not_of(" \t\r\n\f\v");
        if (first == std::string::npos) text.clear();
        else text = text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
        if (text == currentText) return;

        Number next;
        bool isNumber = parseNumber(text, next);
        lastChangeDirection = isNumber && hasNumber ? compareValues(next, number) : 0;
        if (lastChangeDirection) ++changeRevision;
        int direction = 0;
        if (isNumber && hasNumber && animationEnabled && ODOMETER_ANIMATION_DURATION_MS > 0) {
            direction = compareMagnitudes(next, number);
        }

        Number previous = number;
        stopAnimation();
        currentText = text;
        number = next;
        hasNumber = isNumber;
        if (isNumber) {
            renderNumber(direction ? &previous : nullptr, direction);
        } else {
            lv_obj_clear_flag(textLabel, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(textLabel, text.c_str());
        }
    }

    bool isAnimating() const {
        return animating;
    }

    int valueChangeDirection() const { return lastChangeDirection; }
    uint32_t valueChangeRevision() const { return changeRevision; }

    // Rehearse a full reel turn without changing the displayed price.
    void replayAnimation() {
        if (!hasNumber || !animationEnabled || ODOMETER_ANIMATION_DURATION_MS == 0) return;
        Number previous = number;
        renderNumber(&previous, 1, true);
    }

    bool numberBounds(lv_area_t *bounds) const {
        if (numberLayer) return relativeBounds(numberLayer, bounds);
        return textLabel && !currentText.empty() &&
               !lv_obj_has_flag(textLabel, LV_OBJ_FLAG_HIDDEN) &&
               relativeBounds(textLabel, bounds);
    }

    size_t digitBounds(lv_area_t *out, size_t capacity) const {
        if (!root || !numberLayer) return 0;
        size_t count = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(numberLayer); ++i) {
            lv_obj_t *child = lv_obj_get_child(numberLayer, i);
            if (lv_obj_get_child_cnt(child) == 0) continue;
            if (out && count < capacity) relativeBounds(child, &out[count]);
            ++count;
        }
        return count;
    }

    bool interactionTarget(lv_area_t *bounds, int *direction) const {
        if (direction) *direction = 0;
        if (!numberLayer || !bounds) return false;
        if (animating && !rolls.empty()) {
            const Roll &roll = rolls.front();
            if (direction) *direction = roll.endY > roll.startY ? 1 : -1;
            return relativeBounds(lv_obj_get_parent(roll.strip), bounds);
        }
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(numberLayer); ++i) {
            lv_obj_t *child = lv_obj_get_child(numberLayer, i);
            if (lv_obj_get_child_cnt(child) == 0) continue;
            if (!relativeBounds(child, bounds)) return false;
            lv_font_glyph_dsc_t glyph;
            const char digit = lv_label_get_text(lv_obj_get_child(child, 0))[0];
            if (lv_font_get_glyph_dsc(font, &glyph, digit, '\0') &&
                glyph.box_w && glyph.box_h) {
                bounds->x1 += (digitWidth - glyph.adv_w) / 2 + glyph.ofs_x;
                bounds->y1 += height - font->base_line - glyph.box_h - glyph.ofs_y;
                bounds->x2 = bounds->x1 + glyph.box_w - 1;
                bounds->y2 = bounds->y1 + glyph.box_h - 1;
            }
            return true;
        }
        return false;
    }

    void setAnimationEnabled(bool enabled) {
        animationEnabled = enabled;
        if (!enabled && animating) renderNumber(nullptr, 0);
    }

private:
    struct Number {
        std::string integer;
        std::string fraction;
        unsigned int start = 0;
        bool hasDecimal = false;
        bool negative = false;
    };

    struct Roll {
        lv_obj_t *strip;
        lv_coord_t startY;
        lv_coord_t endY;
        lv_coord_t x;
    };

    struct FramePart {
        lv_coord_t x;
        lv_coord_t width;
        lv_obj_t *strip;
        const char *reel;
        unsigned rows;
        std::unique_ptr<TextBitmap> punctuation;
    };

    lv_obj_t *root = nullptr;
    lv_obj_t *textLabel = nullptr;
    lv_obj_t *numberLayer = nullptr;
    const lv_font_t *font = nullptr;
    lv_coord_t height = 0;
    lv_coord_t digitWidth = 0;
    std::string currentText;
    Number number;
    bool hasNumber = false;
    bool animating = false;
    bool animationEnabled = true;
    int lastChangeDirection = 0;
    uint32_t changeRevision = 0;
    std::vector<Roll> rolls;
    TextBitmap digitBitmaps[10];
    bool digitBitmapsReady = false;
    std::vector<FramePart> frameParts;
    bool frameCacheReady = false;

    bool relativeBounds(lv_obj_t *object, lv_area_t *bounds) const {
        if (!root || !object || !bounds) return false;
        lv_obj_update_layout(root);
        lv_area_t origin;
        lv_obj_get_content_coords(root, &origin);
        lv_obj_get_coords(object, bounds);
        bounds->x1 -= origin.x1;
        bounds->x2 -= origin.x1;
        bounds->y1 -= origin.y1;
        bounds->y2 -= origin.y1;
        return true;
    }

    static bool isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    // Accept plain numbers and prices, while leaving messages such as
    // "Error 500" as ordinary text. Commas are integer grouping separators.
    static bool parseNumber(const std::string &text, Number &result) {
        unsigned int pos = 0;
        if (text[pos] == '$') ++pos;
        if (text[pos] == '-' || text[pos] == '+') {
            result.negative = text[pos] == '-';
            ++pos;
        }
        if (text[pos] == '$') ++pos;
        result.start = pos;

        bool foundDigit = false;
        for (; pos < text.size(); ++pos) {
            char c = text[pos];
            if (isDigit(c)) {
                foundDigit = true;
                if (result.hasDecimal) result.fraction += c;
                else result.integer += c;
            } else if (c == '.' && !result.hasDecimal) {
                result.hasDecimal = true;
            } else if (c == ',' && !result.hasDecimal &&
                       result.integer.size() && isDigit(text[pos + 1])) {
                continue;
            } else {
                return false;
            }
        }

        return foundDigit;
    }

    static bool negativeValue(const Number &value) {
        if (!value.negative) return false;
        for (char digit : value.integer) if (digit != '0') return true;
        for (char digit : value.fraction) if (digit != '0') return true;
        return false;
    }

    static int compareValues(const Number &left, const Number &right) {
        const bool leftNegative = negativeValue(left);
        const bool rightNegative = negativeValue(right);
        if (leftNegative != rightNegative) return leftNegative ? -1 : 1;
        const int magnitude = compareMagnitudes(left, right);
        return leftNegative ? -magnitude : magnitude;
    }

    // The highest differing digit chooses the common direction, including for
    // negative displays. Compare places directly to avoid decimal rounding.
    static int compareMagnitudes(const Number &left, const Number &right) {
        unsigned int leftStart = 0;
        unsigned int rightStart = 0;
        while (leftStart < left.integer.size() && left.integer[leftStart] == '0') ++leftStart;
        while (rightStart < right.integer.size() && right.integer[rightStart] == '0') ++rightStart;
        unsigned int leftLength = left.integer.size() - leftStart;
        unsigned int rightLength = right.integer.size() - rightStart;
        int order = leftLength > rightLength ? 1 : leftLength < rightLength ? -1 : 0;
        for (unsigned int i = 0; !order && i < leftLength; ++i) {
            char a = left.integer[leftStart + i];
            char b = right.integer[rightStart + i];
            if (a != b) order = a > b ? 1 : -1;
        }
        for (unsigned int i = 0; !order &&
             (i < left.fraction.size() || i < right.fraction.size()); ++i) {
            int a = digitAt(left.fraction, i);
            int b = digitAt(right.fraction, i);
            if (a != b) order = a > b ? 1 : -1;
        }
        return order;
    }

    lv_coord_t textWidth(const char *text) const {
        return lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE);
    }

    lv_obj_t *makeLabel(lv_obj_t *parent, const char *text) {
        lv_obj_t *label = lv_label_create(parent);
        // Only glyph coverage may obscure the animated scene behind the price.
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(label, 0, 0);
        lv_obj_set_style_text_line_space(label, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, text);
        return label;
    }

    lv_obj_t *makeViewport(lv_obj_t *parent) {
        lv_obj_t *viewport = lv_obj_create(parent);
        lv_obj_remove_style_all(viewport);
        lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE |
                                   LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        return viewport;
    }

    void stopAnimation() {
        lv_anim_del(this, scrollDigits);
        animating = false;
        rolls.clear();
        frameCacheReady = false;
        frameParts.clear();
        if (numberLayer) {
            lv_obj_del(numberLayer);
            numberLayer = nullptr;
        }
    }

    static int digitAt(const std::string &digits, int index) {
        return index >= 0 && index < static_cast<int>(digits.size())
            ? digits[index] - '0' : 0;
    }

    void renderNumber(const Number *previous, int direction, bool rehearsal = false) {
        stopAnimation();
        lv_obj_add_flag(textLabel, LV_OBJ_FLAG_HIDDEN);
        numberLayer = makeViewport(root);
        bool cacheReady = digitBitmapsReady;

        std::string text = currentText;
        unsigned int integerPlaces = number.integer.size();
        if (previous) {
            // Keep disappearing places until their rolls finish. Align integer
            // places from the right and fractional places after the decimal.
            std::string leadingZeros;
            for (unsigned int i = integerPlaces; i < previous->integer.size(); ++i) {
                leadingZeros += '0';
            }
            integerPlaces += leadingZeros.size();
            text = text.substr(0, number.start) + leadingZeros + text.substr(number.start);
            if (previous->fraction.size() > number.fraction.size()) {
                if (!number.hasDecimal) text += '.';
                for (unsigned int i = number.fraction.size(); i < previous->fraction.size(); ++i) {
                    text += '0';
                }
            }
        }

        lv_coord_t x = 0;
        int integerIndex = 0;
        int fractionIndex = 0;
        bool fractional = false;
        bool affected = rehearsal;
        for (unsigned int pos = 0; pos < text.size();) {
            char c = text[pos];
            if (!isDigit(c)) {
                unsigned int start = pos;
                do {
                    if (text[pos] == '.') fractional = true;
                    ++pos;
                } while (pos < text.size() && !isDigit(text[pos]));
                std::string punctuation = text.substr(start, pos - start);
                lv_coord_t width = textWidth(punctuation.c_str());
                lv_obj_t *label = makeLabel(numberLayer, punctuation.c_str());
                lv_obj_set_size(label, width, height);
                lv_obj_set_pos(label, x, 0);
                if (cacheReady) {
                    std::unique_ptr<TextBitmap> bitmap(new(std::nothrow) TextBitmap);
                    if (!bitmap || !bitmap->render(punctuation.c_str(), font, width,
                                                    LV_TEXT_ALIGN_CENTER, true)) {
                        cacheReady = false;
                        frameParts.clear();
                    } else {
                        frameParts.push_back({x, width, nullptr, nullptr, 0, std::move(bitmap)});
                    }
                }
                x += width;
                continue;
            }

            int target = c - '0';
            int initial = target;
            if (previous) {
                initial = fractional
                    ? digitAt(previous->fraction, fractionIndex)
                    : digitAt(previous->integer, integerIndex -
                              static_cast<int>(integerPlaces - previous->integer.size()));
            }
            if (fractional) ++fractionIndex;
            else ++integerIndex;

            int steps = 0;
            if (previous && initial != target) affected = true;
            if (previous && affected) {
                steps = (direction * (target - initial) + 10) % 10;
                // Lower places roll together, even when their digit is unchanged.
                if (!steps) steps = 10;
            }
            std::string reel;
            for (int row = 0; row <= steps; ++row) {
                int step = direction > 0 ? steps - row : row;
                reel += static_cast<char>('0' + (initial + direction * step + 100) % 10);
                if (row < steps) reel += '\n';
            }

            lv_obj_t *viewport = makeViewport(numberLayer);
            lv_obj_set_size(viewport, digitWidth, height);
            lv_obj_set_pos(viewport, x, 0);
            lv_obj_t *strip = makeLabel(viewport, reel.c_str());
            lv_obj_set_size(strip, digitWidth, height);
            lv_coord_t startY = direction > 0 ? -steps * height : 0;
            lv_coord_t endY = direction < 0 ? -steps * height : 0;
            lv_obj_set_pos(strip, 0, 0);
            setReelOffset(strip, startY);
            if (cacheReady) {
                frameParts.push_back({x, digitWidth, strip, lv_label_get_text(strip),
                                      static_cast<unsigned>(steps + 1), nullptr});
            }
            if (previous && affected) rolls.push_back({strip, startY, endY, x});
            x += digitWidth;
            ++pos;
        }

        lv_obj_set_size(numberLayer, x, height);
        lv_obj_update_layout(root);
        lv_obj_set_pos(numberLayer, (lv_obj_get_content_width(root) - x) / 2 - 15,
                       (lv_obj_get_content_height(root) - 20 - height) / 2);
        frameCacheReady = cacheReady;
        if (frameCacheReady) {
            // The children retain the ordinary renderer for allocation failure
            // and reel metadata. A single layer draws the complete cached view.
            lv_obj_add_event_cb(numberLayer, drawFrame, LV_EVENT_DRAW_MAIN, this);
            for (unsigned child = 0; child < lv_obj_get_child_cnt(numberLayer); ++child)
                lv_obj_add_flag(lv_obj_get_child(numberLayer, child), LV_OBJ_FLAG_HIDDEN);
        }
        if (rolls.empty()) return;

        // One progress animation starts and finishes all digit rolls together.
        animating = true;
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, this);
        lv_anim_set_values(&animation, 0, 1000);
        lv_anim_set_time(&animation, ODOMETER_ANIMATION_DURATION_MS);
        lv_anim_set_exec_cb(&animation, scrollDigits);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
        lv_anim_set_ready_cb(&animation, animationFinished);
        lv_anim_start(&animation);
    }

    static void scrollDigits(void *object, int32_t progress) {
        OdometerDisplay *display = static_cast<OdometerDisplay *>(object);
        lv_area_t layer_coords;
        lv_area_t dirty;
        bool changed = false;
        if (display->frameCacheReady) lv_obj_get_content_coords(display->numberLayer, &layer_coords);
        for (const Roll &roll : display->rolls) {
            if (!setReelOffset(roll.strip, roll.startY +
                              (roll.endY - roll.startY) * progress / 1000)) continue;
            if (!display->frameCacheReady) {
                lv_obj_invalidate(roll.strip);
                continue;
            }
            const lv_area_t digit_area = {
                static_cast<lv_coord_t>(layer_coords.x1 + roll.x), layer_coords.y1,
                static_cast<lv_coord_t>(layer_coords.x1 + roll.x + display->digitWidth - 1),
                static_cast<lv_coord_t>(layer_coords.y1 + display->height - 1),
            };
            if (changed) _lv_area_join(&dirty, &dirty, &digit_area);
            else dirty = digit_area;
            changed = true;
        }
        if (changed) lv_obj_invalidate_area(display->numberLayer, &dirty);
    }

    static bool setReelOffset(lv_obj_t *strip, lv_coord_t y) {
        // Keep the reel's layout fixed. Moving its Y style remeasures every
        // digit line and updates parent layout on each animation frame.
        lv_label_t *label = reinterpret_cast<lv_label_t *>(strip);
        if (label->offset.y == y) return false;
        label->offset.y = y;
        return true;
    }

    static void drawFrame(lv_event_t *event) {
        auto *display = static_cast<OdometerDisplay *>(lv_event_get_user_data(event));
        lv_draw_ctx_t *context = lv_event_get_draw_ctx(event);
        lv_area_t coords;
        lv_obj_get_content_coords(lv_event_get_target(event), &coords);
        for (const FramePart &part : display->frameParts) {
            const lv_coord_t x1 = coords.x1 + part.x;
            const lv_coord_t x2 = x1 + part.width - 1;
            if (x1 > context->clip_area->x2 || x2 < context->clip_area->x1) continue;
            if (part.punctuation) {
                const lv_area_t bitmap_coords = {
                    x1, coords.y1, x2,
                    static_cast<lv_coord_t>(coords.y1 + display->height - 1),
                };
                part.punctuation->drawTransparent(context, bitmap_coords);
                continue;
            }
            const auto *label = reinterpret_cast<const lv_label_t *>(part.strip);
            unsigned row = 0;
            int32_t y = coords.y1 + label->offset.y;
            // Each reel row is one ASCII digit followed by a newline. Copy
            // only the one or two rows intersecting this draw buffer.
            while (y + display->height - 1 < context->clip_area->y1) {
                y += display->height;
                ++row;
            }
            while (row < part.rows && y <= context->clip_area->y2) {
                const unsigned digit = static_cast<unsigned>(part.reel[row * 2U] - '0');
                if (digit < 10) {
                    const lv_area_t bitmap_coords = {
                        x1, static_cast<lv_coord_t>(y), x2,
                        static_cast<lv_coord_t>(y + display->height - 1),
                    };
                    display->digitBitmaps[digit].drawTransparent(context, bitmap_coords);
                }
                y += display->height;
                ++row;
            }
        }
    }

    static void animationFinished(lv_anim_t *animation) {
        OdometerDisplay *display = static_cast<OdometerDisplay *>(animation->var);
        display->renderNumber(nullptr, 0);
    }
};
