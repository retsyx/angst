#ifndef DISPLAY_TEXT_BITMAP_H
#define DISPLAY_TEXT_BITMAP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"

#ifndef DISPLAY_TEXT_BITMAP_ALLOC
#define DISPLAY_TEXT_BITMAP_ALLOC(bytes) lv_mem_alloc(bytes)
#endif
#ifndef DISPLAY_TEXT_BITMAP_FREE
#define DISPLAY_TEXT_BITMAP_FREE(pointer) lv_mem_free(pointer)
#endif

// GUI-task-only cache of one white text line, with optional glyph opacity.
// Direct software rendering supports lines wider than lv_img_dsc_t's bitfield.
class TextBitmap {
public:
    lv_color_t *pixels = nullptr;
    lv_coord_t width = 0;
    lv_coord_t height = 0;

    TextBitmap() = default;
    TextBitmap(const TextBitmap &) = delete;
    TextBitmap &operator=(const TextBitmap &) = delete;
    ~TextBitmap() { clear(); }

    bool render(const char *text, const lv_font_t *font, lv_coord_t requested_width,
                lv_text_align_t align = LV_TEXT_ALIGN_LEFT, bool preserve_alpha = false)
    {
        clear();
        if (!text || !font || requested_width <= 0 || requested_width > LV_COORD_MAX ||
            font->line_height == 0 || font->line_height > LV_COORD_MAX) return false;

        lv_disp_t *display = lv_disp_get_default();
        if (!display || !display->driver) return false;

        const size_t line_width = static_cast<size_t>(requested_width);
        const size_t line_height = static_cast<size_t>(font->line_height);
        const size_t bytes_per_pixel = sizeof(lv_color_t) +
                                       (preserve_alpha ? sizeof(lv_opa_t) : 0);
        if (line_width > SIZE_MAX / line_height ||
            line_width * line_height > SIZE_MAX / bytes_per_pixel) return false;
        const size_t pixel_count = line_width * line_height;
        const size_t bytes = pixel_count * bytes_per_pixel;
        pixels = static_cast<lv_color_t *>(DISPLAY_TEXT_BITMAP_ALLOC(bytes));
        if (!pixels) return false;
        opacity = preserve_alpha ? reinterpret_cast<lv_opa_t *>(pixels + pixel_count) : nullptr;
        width = requested_width;
        height = static_cast<lv_coord_t>(line_height);
        allocated_bytes = bytes;
        memset(pixels, 0, bytes);

        lv_area_t area = {0, 0, static_cast<lv_coord_t>(width - 1),
                          static_cast<lv_coord_t>(height - 1)};
        lv_draw_sw_ctx_t context;
        lv_draw_sw_init_ctx(display->driver, &context.base_draw);
        context.base_draw.buf = pixels;
        context.base_draw.buf_area = &area;
        context.base_draw.clip_area = &area;

        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.font = font;
        label.color = lv_color_white();
        label.opa = LV_OPA_COVER;
        label.align = align;
        label.letter_space = 0;
        label.line_space = 0;
        label.flag = LV_TEXT_FLAG_EXPAND;

        // LVGL's software blender reads the globally refreshing display even
        // for an off-screen context. Restore both that pointer and its format.
        lv_disp_t *previous_display = _lv_refr_get_disp_refreshing();
        const auto previous_set_pixel = display->driver->set_px_cb;
        const uint32_t previous_transparency = display->driver->screen_transp;
        display->driver->set_px_cb = nullptr;
        display->driver->screen_transp = 0;
        _lv_refr_set_disp_refreshing(display);
        lv_draw_label(&context.base_draw, &label, &area, text, nullptr);
        lv_draw_wait_for_finish(&context.base_draw);
        if (opacity) {
            // Keep the original font coverage, before RGB565 quantization.
            // A dedicated context captures LVGL's glyph masks without changing
            // the driver's pixel callback, user data or visible framebuffer.
            context.base_draw.buf = opacity;
            context.blend = captureOpacity;
            lv_draw_label(&context.base_draw, &label, &area, text, nullptr);
            lv_draw_wait_for_finish(&context.base_draw);
        }
        display->driver->set_px_cb = previous_set_pixel;
        display->driver->screen_transp = previous_transparency;
        _lv_refr_set_disp_refreshing(previous_display);
        lv_draw_sw_deinit_ctx(display->driver, &context.base_draw);
        return true;
    }

    void draw(lv_draw_ctx_t *context, const lv_area_t &coordinates) const
    {
        if (!pixels || !context || !context->buf || !context->buf_area ||
            !context->clip_area || !_lv_refr_get_disp_refreshing()) return;
        // The blend routine derives source stride from the full image area.
        // Clipping belongs to the draw context; changing this area is unsafe.
        if (static_cast<int64_t>(coordinates.x2) - coordinates.x1 + 1 != width ||
            static_cast<int64_t>(coordinates.y2) - coordinates.y1 + 1 != height) return;
        lv_draw_img_dsc_t image;
        lv_draw_img_dsc_init(&image);
        image.opa = LV_OPA_COVER;
        lv_draw_sw_img_decoded(context, &image, &coordinates,
                               reinterpret_cast<const uint8_t *>(pixels), LV_IMG_CF_TRUE_COLOR);
    }

    // White glyphs blend onto the scene underneath, leaving counters and gaps
    // transparent. The normal draw() remains a fast opaque RGB565 copy.
    void drawTransparent(lv_draw_ctx_t *context, const lv_area_t &coordinates) const
    {
        if (!opacity) {
            draw(context, coordinates);
            return;
        }
        if (!pixels || !context || !context->buf || !context->buf_area ||
            !context->clip_area || !_lv_refr_get_disp_refreshing()) return;
        if (static_cast<int64_t>(coordinates.x2) - coordinates.x1 + 1 != width ||
            static_cast<int64_t>(coordinates.y2) - coordinates.y1 + 1 != height) return;
        lv_draw_sw_blend_dsc_t blend = {};
        blend.blend_area = &coordinates;
        blend.color = lv_color_white();
        blend.mask_buf = opacity;
        blend.mask_res = LV_DRAW_MASK_RES_CHANGED;
        blend.mask_area = &coordinates;
        blend.opa = LV_OPA_COVER;
        blend.blend_mode = LV_BLEND_MODE_NORMAL;
        lv_draw_sw_blend(context, &blend);
    }

    void clear(bool erase = false)
    {
        if (pixels) {
            if (erase) {
                volatile uint8_t *cursor = reinterpret_cast<volatile uint8_t *>(pixels);
                for (size_t i = 0; i < allocated_bytes; ++i) cursor[i] = 0;
            }
            DISPLAY_TEXT_BITMAP_FREE(pixels);
        }
        pixels = nullptr;
        opacity = nullptr;
        width = 0;
        height = 0;
        allocated_bytes = 0;
    }

private:
    lv_opa_t *opacity = nullptr;
    size_t allocated_bytes = 0;

    static void captureOpacity(lv_draw_ctx_t *context,
                               const lv_draw_sw_blend_dsc_t *blend)
    {
        if (blend->mask_buf && blend->mask_res == LV_DRAW_MASK_RES_TRANSP) return;
        lv_area_t clipped;
        if (!_lv_area_intersect(&clipped, blend->blend_area, context->clip_area)) return;
        lv_opa_t *destination = static_cast<lv_opa_t *>(context->buf);
        const lv_coord_t stride = lv_area_get_width(context->buf_area);
        const bool masked = blend->mask_buf && blend->mask_res != LV_DRAW_MASK_RES_FULL_COVER;
        const lv_coord_t mask_stride = masked ? lv_area_get_width(blend->mask_area) : 0;
        const bool antialiasing = _lv_refr_get_disp_refreshing()->driver->antialiasing;
        for (lv_coord_t y = clipped.y1; y <= clipped.y2; ++y) {
            const size_t row = static_cast<size_t>(y - context->buf_area->y1) * stride;
            for (lv_coord_t x = clipped.x1; x <= clipped.x2; ++x) {
                unsigned coverage = LV_OPA_COVER;
                if (masked) {
                    coverage = blend->mask_buf[(y - blend->mask_area->y1) * mask_stride +
                                               x - blend->mask_area->x1];
                    if (!antialiasing) coverage = coverage > 128 ? LV_OPA_COVER : LV_OPA_TRANSP;
                }
                if (blend->opa < LV_OPA_MAX && coverage != LV_OPA_COVER)
                    coverage = (coverage * blend->opa) >> 8;
                else if (blend->opa < LV_OPA_MAX) coverage = blend->opa;
                const size_t index = row + x - context->buf_area->x1;
                const unsigned previous = destination[index];
                destination[index] = static_cast<lv_opa_t>(coverage + previous -
                                                           (coverage * previous + 127) / 255);
            }
        }
    }
};

#endif // DISPLAY_TEXT_BITMAP_H
