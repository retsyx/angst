#include "password_display_mock_idf.h"

#include <cstdio>

#include "../main/password_display.cpp"

static lv_obj_t screen;

static esp_err_t request_from_network(const char *password)
{
    const unsigned previous_lvgl_calls = lvgl_calls;
    gui_context = false;
    esp_err_t result = password_display_request(password);
    gui_context = true;
    assert(lvgl_calls == previous_lvgl_calls && critical_depth == 0);
    return result;
}

static lv_obj_t *find_child(lv_obj_t *parent, bool label)
{
    for (unsigned i = 0; i < object_count; ++i) {
        lv_obj_t *object = &mock_objects[i];
        if (!object->deleted && object->parent == parent && object->label == label) return object;
    }
    assert(false);
    return nullptr;
}

static lv_obj_t *screen_overlay() { return find_child(&screen, false); }
static lv_obj_t *password_viewport() { return find_child(screen_overlay(), false); }
static lv_obj_t *password_text() { return find_child(password_viewport(), true); }

static void init()
{
    assert(password_display_init(&screen) == ESP_OK);
    lv_obj_t *cover = screen_overlay();
    assert(cover->width == 536 && cover->height == 240 && cover->x == 0 && cover->y == 0);
    assert(cover->opacity == LV_OPA_COVER && cover->background == lv_color_black());
    assert(cover->flags & LV_OBJ_FLAG_HIDDEN);
    assert(!(cover->flags & (LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE)));
    lv_obj_t *viewport = password_viewport();
    assert(viewport->x >= 0 && viewport->x + viewport->width <= cover->width);
    assert(!(viewport->flags & (LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE)));
    lv_obj_t *label = password_text();
    assert(label->font == &lv_font_montserrat_40);
    assert(!label->recolor && label->long_mode == LV_LABEL_LONG_CLIP);
    assert(label->x == 0 && label->width == viewport->width);
    assert(label->height == lv_font_montserrat_40.line_height);
    assert(label->y == (viewport->height - label->height) / 2);
    assert(label->static_text && label->static_text[0] == '\0');
    assert(!password_display_update());
}

static bool update_at(uint32_t tick)
{
    mock_tick = tick;
    bool active = password_display_update();
    assert(critical_depth == 0);
    return active;
}

static const char *expect_visible(const char *expected)
{
    lv_obj_t *cover = screen_overlay();
    lv_obj_t *label = password_text();
    assert(!(cover->flags & LV_OBJ_FLAG_HIDDEN) && cover->foreground_calls > 0);
    assert(label->static_text && std::strcmp(label->static_text, expected) == 0);
    // The password belongs to only one static label: no dynamically allocated LVGL copy.
    for (unsigned i = 0; i < object_count; ++i) {
        assert(mock_objects[i].dynamic_text.empty());
        if (&mock_objects[i] != label && mock_objects[i].static_text)
            assert(std::strcmp(mock_objects[i].static_text, expected) != 0);
    }
    return label->static_text;
}

static void expect_cleared(const char *previous_secret)
{
    lv_obj_t *label = password_text();
    assert(screen_overlay()->flags & LV_OBJ_FLAG_HIDDEN);
    assert(label->static_text && label->static_text != previous_secret && label->static_text[0] == '\0');
    assert(!password_bitmap.pixels && password_bitmap.width == 0 && password_bitmap.height == 0);
    // Check the entire bounded buffer, including any tail from a previous longer password.
    for (unsigned i = 0; i < UPGRADE_PASSWORD_MAX_LENGTH + 1; ++i) assert(previous_secret[i] == '\0');
    assert(!password_display_update());
}

static void test_invalid()
{
    const unsigned before = lvgl_calls;
    assert(password_display_init(nullptr) == ESP_ERR_INVALID_ARG);
    assert(lvgl_calls == before);
    assert(request_from_network("ValidPassword") == ESP_ERR_INVALID_STATE);
    init();
    const unsigned initialized_calls = lvgl_calls;
    assert(password_display_init(&screen) == ESP_ERR_INVALID_STATE);
    assert(lvgl_calls == initialized_calls);
    const char *invalid_passwords[] = {
        nullptr, "", " leading", "trailing ", "two\nlines", "has\tTab", "\x7f", "\xc3\xa9",
    };
    for (const char *invalid : invalid_passwords) {
        assert(request_from_network(invalid) == ESP_ERR_INVALID_ARG);
    }
    std::string too_long(UPGRADE_PASSWORD_MAX_LENGTH + 1, 'X');
    assert(request_from_network(too_long.c_str()) == ESP_ERR_INVALID_ARG);
    std::string maximum(UPGRADE_PASSWORD_MAX_LENGTH, 'Z');
    assert(request_from_network(maximum.c_str()) == ESP_OK);
    assert(update_at(0));
    const char *secret = expect_visible(maximum.c_str());
    assert(!update_at(15000));
    expect_cleared(secret);
}

static void test_fit(bool boundary)
{
    init();
    std::string exact;
    for (char character = '!'; character <= '~'; ++character) exact += character;
    exact.insert(12, " ");  // Interior spaces and all punctuation must render literally.
    char caller[UPGRADE_PASSWORD_MAX_LENGTH + 1];
    std::strcpy(caller, exact.c_str());
    mock_text_width = boundary ? password_viewport()->width : 180;
    assert(request_from_network(caller) == ESP_OK);
    caller[0] = 'x';
    assert(request_from_network("QueuedReplacement") == ESP_ERR_INVALID_STATE);
    assert(update_at(500));
    const char *secret = expect_visible(exact.c_str());
    assert(secret != caller);
    assert(password_text()->x == 0 && password_text()->width == password_viewport()->width);
    assert(password_text()->offset.x == (password_viewport()->width - mock_text_width) / 2);
    assert(request_from_network("VisibleReplacement") == ESP_ERR_INVALID_STATE);
    assert(update_at(500 + 14999));
    expect_visible(exact.c_str());
    assert(!update_at(500 + 15000));
    expect_cleared(secret);
}

static void test_overflow()
{
    init();
    mock_text_width = password_viewport()->width + 180;
    assert(request_from_network("Aa-0123456789!LongSecret") == ESP_OK);
    assert(update_at(100));
    const char *secret = expect_visible("Aa-0123456789!LongSecret");
    // A 180px overrun makes two round trips, moving for 2 seconds on each leg.
    // Initial and endpoint holds each last 2 seconds; nothing jumps at the turn.
    const struct { uint32_t elapsed; lv_coord_t offset; } samples[] = {
        {0, 0}, {1999, 0}, {2000, 0}, {2500, -45},
        {3999, -179}, {4000, -180}, {4001, -180},
        {5999, -180}, {6000, -180}, {6001, -180}, {6012, -179},
        {6500, -135}, {7999, -1}, {8000, 0}, {8001, 0}, {9999, 0},
        {10000, 0}, {10001, 0}, {10012, -1}, {10500, -45},
        {11999, -179}, {12000, -180}, {12001, -180},
        {13999, -180}, {14000, -180}, {14001, -180}, {14012, -179},
        {14500, -135}, {15999, -1}, {16000, 0}, {16001, 0}, {17999, 0},
    };
    for (const auto &sample : samples) {
        assert(update_at(100 + sample.elapsed));
        assert(password_text()->offset.x == sample.offset);
        assert(password_text()->x == 0 && password_text()->width == password_viewport()->width);
        expect_visible("Aa-0123456789!LongSecret");
        if (sample.elapsed == 10000)
            assert(request_from_network("StillBusyAfterFirstRoundTrip") == ESP_ERR_INVALID_STATE);
    }
    assert(mock_bitmap_render_calls == 1);
    assert(!update_at(100 + 18000));
    expect_cleared(secret);
    const unsigned show_count = screen_overlay()->foreground_calls;
    assert(!update_at(100 + 20000));
    assert(screen_overlay()->foreground_calls == show_count);

    // A one-pixel overrun must scroll too, using rounded-up duration instead of being treated as fitting.
    mock_text_width = password_viewport()->width + 1;
    assert(request_from_network("OnePixelOverflow") == ESP_OK);
    assert(update_at(20000));
    secret = expect_visible("OnePixelOverflow");
    assert(password_text()->offset.x == 0);
    assert(update_at(22012));
    assert(password_text()->offset.x == -1);
    assert(update_at(24011));
    assert(password_text()->offset.x == -1);
    assert(update_at(24012));
    assert(password_text()->offset.x == -1);
    assert(update_at(24013));
    assert(password_text()->offset.x == -1);
    assert(update_at(24024));
    assert(password_text()->offset.x == 0);
    assert(update_at(26023));
    assert(password_text()->offset.x == 0);
    assert(update_at(26024));
    assert(password_text()->offset.x == 0);
    assert(update_at(26025));
    assert(password_text()->offset.x == 0);
    assert(request_from_network("StillBouncing") == ESP_ERR_INVALID_STATE);
    assert(update_at(26036));
    assert(password_text()->offset.x == -1);
    assert(update_at(28035));
    assert(password_text()->offset.x == -1);
    assert(update_at(28036));
    assert(password_text()->offset.x == -1);
    assert(update_at(28037));
    assert(password_text()->offset.x == -1);
    assert(update_at(28048));
    assert(password_text()->offset.x == 0);
    assert(update_at(30047));
    assert(password_text()->offset.x == 0);
    assert(!update_at(30048));
    expect_cleared(secret);
}

static void test_wrap()
{
    init();
    assert(request_from_network("WrapSafe") == ESP_OK);
    constexpr uint32_t start = UINT32_MAX - 5000;
    assert(update_at(start));
    const char *secret = expect_visible("WrapSafe");
    assert(update_at(start + uint32_t{14999}));
    assert(!update_at(start + uint32_t{15000}));
    expect_cleared(secret);

    mock_text_width = password_viewport()->width + 180;
    assert(request_from_network("WrapSafeScroll") == ESP_OK);
    assert(update_at(start));
    secret = expect_visible("WrapSafeScroll");
    assert(update_at(start + uint32_t{4000}));
    assert(password_text()->offset.x == -180);
    assert(update_at(start + uint32_t{6000}));
    assert(password_text()->offset.x == -180);
    assert(update_at(start + uint32_t{6500}));
    assert(password_text()->offset.x == -135);
    assert(update_at(start + uint32_t{7999}));
    assert(password_text()->offset.x == -1);
    assert(update_at(start + uint32_t{8000}));
    assert(password_text()->offset.x == 0);
    assert(update_at(start + uint32_t{9999}));
    assert(password_text()->offset.x == 0);
    assert(update_at(start + uint32_t{10000}));
    assert(password_text()->offset.x == 0);
    assert(update_at(start + uint32_t{12000}));
    assert(password_text()->offset.x == -180);
    assert(update_at(start + uint32_t{14500}));
    assert(password_text()->offset.x == -135);
    assert(update_at(start + uint32_t{17999}));
    assert(password_text()->offset.x == 0);
    assert(!update_at(start + uint32_t{18000}));
    expect_cleared(secret);
}

static void test_rotation()
{
    init();
    char current_password[UPGRADE_PASSWORD_MAX_LENGTH + 1] = "OriginalPasswordWithLongTail";
    assert(request_from_network(current_password) == ESP_OK);
    assert(update_at(0));
    const char *secret = expect_visible(current_password);
    std::strcpy(current_password, "New#AbC!9");
    assert(request_from_network(current_password) == ESP_ERR_INVALID_STATE);
    expect_visible("OriginalPasswordWithLongTail");
    assert(!update_at(15000));
    expect_cleared(secret);
    assert(request_from_network(current_password) == ESP_OK);
    assert(update_at(16000));
    secret = expect_visible("New#AbC!9");
    assert(secret != current_password);
    assert(!update_at(31000));
    expect_cleared(secret);
}

static void test_scroll_frames()
{
    init();
    lv_obj_t *label = password_text();
    const lv_coord_t initial_x = label->x;
    const lv_coord_t initial_y = label->y;
    const lv_coord_t initial_width = label->width;
    const lv_coord_t initial_height = label->height;
    const unsigned initial_geometry_calls = geometry_calls;
    const unsigned initial_text_measure_calls = text_measure_calls;
    mock_text_width = password_viewport()->width + 180;
    assert(request_from_network("SmoothFramesWithoutLayout") == ESP_OK);
    assert(update_at(100));
    const char *secret = expect_visible("SmoothFramesWithoutLayout");
    assert(text_measure_calls == initial_text_measure_calls + 1);
    assert(mock_bitmap_render_calls == 1 && password_bitmap.pixels);
    assert(label->text_opacity == LV_OPA_TRANSP);
    const unsigned initial_style_calls = style_calls;

    // Scrolling updates only the draw offset. Moving the label itself causes LVGL
    // to refresh the whole overflowing object and repeatedly recalculate layout.
    unsigned drawn_frames = 0;
    const auto verify_frame = [&](uint32_t tick, lv_coord_t expected_offset) {
        const lv_coord_t previous_offset = label->offset.x;
        const unsigned previous_invalidations = label->invalidation_calls;
        assert(update_at(tick));
        assert(label->offset.x == expected_offset && label->offset.y == 0);
        assert(label->invalidation_calls == previous_invalidations +
               (previous_offset != expected_offset ? 1u : 0u));
        assert(label->x == initial_x && label->y == initial_y);
        assert(label->width == initial_width && label->height == initial_height);
        assert(geometry_calls == initial_geometry_calls);
        assert(style_calls == initial_style_calls);
        assert(text_measure_calls == initial_text_measure_calls + 1);
        assert(mock_bitmap_render_calls == 1);
        assert(label->static_text == secret);

        lv_draw_ctx_t context;
        lv_event_t event = {label, &context};
        assert(label->draw_callback);
        label->draw_callback(&event);
        assert(mock_bitmap_draw_calls == ++drawn_frames);
        assert(mock_bitmap_draw_area.x1 == password_viewport()->x + expected_offset);
        assert(mock_bitmap_draw_area.y1 == password_viewport()->y + label->y);

        // A repeated GUI tick has no visual change and must not request a redraw.
        const unsigned invalidations_after_frame = label->invalidation_calls;
        assert(update_at(tick));
        assert(label->invalidation_calls == invalidations_after_frame);
    };
    for (unsigned frame = 0; frame < 100; ++frame) {
        verify_frame(2100 + frame * 10, -static_cast<lv_coord_t>(frame * 10 * 90 / 1000));
    }
    assert(label->invalidation_calls > 0);
    const unsigned invalidations_before_hold = label->invalidation_calls;
    assert(update_at(4100));
    assert(label->offset.x == -180);
    assert(label->invalidation_calls == invalidations_before_hold + 1);
    const unsigned invalidations_at_hold = label->invalidation_calls;
    assert(update_at(5100));
    assert(label->invalidation_calls == invalidations_at_hold);
    verify_frame(6099, -180);
    verify_frame(6100, -180);
    verify_frame(6101, -180);
    for (unsigned frame = 1; frame <= 100; ++frame) {
        verify_frame(6100 + frame * 10,
                     -180 + static_cast<lv_coord_t>(frame * 10 * 90 / 1000));
    }
    verify_frame(8099, -1);
    verify_frame(8100, 0);
    verify_frame(8101, 0);
    verify_frame(10099, 0);
    verify_frame(10100, 0);
    verify_frame(10101, 0);
    assert(request_from_network("NoReplacementBetweenRoundTrips") == ESP_ERR_INVALID_STATE);
    for (unsigned frame = 1; frame <= 100; ++frame) {
        verify_frame(10100 + frame * 10, -static_cast<lv_coord_t>(frame * 10 * 90 / 1000));
    }
    verify_frame(12099, -179);
    verify_frame(12100, -180);
    verify_frame(12101, -180);
    verify_frame(14099, -180);
    verify_frame(14100, -180);
    verify_frame(14101, -180);
    for (unsigned frame = 1; frame <= 100; ++frame) {
        verify_frame(14100 + frame * 10,
                     -180 + static_cast<lv_coord_t>(frame * 10 * 90 / 1000));
    }
    verify_frame(16099, -1);
    verify_frame(16100, 0);
    verify_frame(16101, 0);
    verify_frame(18099, 0);
    assert(!update_at(18100));
    assert(mock_bitmap_erased_allocations == 1);
    expect_cleared(secret);
}

static void test_bitmap_fallback()
{
    init();
    mock_bitmap_allocation_failure = true;
    mock_text_width = password_viewport()->width + 180;
    assert(request_from_network("AllocationFallback") == ESP_OK);
    assert(update_at(100));
    const char *secret = expect_visible("AllocationFallback");
    assert(mock_bitmap_render_calls == 1 && !password_bitmap.pixels);
    assert(password_text()->text_opacity == LV_OPA_COVER);
    assert(update_at(2600) && password_text()->offset.x == -45);
    lv_draw_ctx_t context;
    lv_event_t event = {password_text(), &context};
    password_text()->draw_callback(&event);
    assert(mock_bitmap_draw_calls == 0);
    assert(update_at(6600) && password_text()->offset.x == -135);
    assert(update_at(8100) && password_text()->offset.x == 0);
    assert(update_at(10100) && password_text()->offset.x == 0);
    assert(request_from_network("FallbackStillBusy") == ESP_ERR_INVALID_STATE);
    assert(update_at(10600) && password_text()->offset.x == -45);
    assert(update_at(14600) && password_text()->offset.x == -135);
    assert(update_at(16100) && password_text()->offset.x == 0);
    assert(!update_at(18100));
    expect_cleared(secret);

    // Allocation failure is scoped to this reveal; a later reveal can cache.
    mock_bitmap_allocation_failure = false;
    mock_text_width = 180;
    assert(request_from_network("AllocationRecovered") == ESP_OK);
    assert(update_at(20000));
    secret = expect_visible("AllocationRecovered");
    assert(mock_bitmap_render_calls == 2 && password_bitmap.pixels);
    assert(password_text()->text_opacity == LV_OPA_TRANSP);
    assert(!update_at(35000));
    assert(mock_bitmap_erased_allocations == 1);
    expect_cleared(secret);
}

static void test_bitmap_start_time()
{
    init();
    mock_bitmap_render_delay = 350;
    assert(request_from_network("RasterizationTime") == ESP_OK);
    assert(update_at(100));
    const char *secret = expect_visible("RasterizationTime");
    assert(shown_at == 450);
    assert(update_at(450 + 14999));
    assert(!update_at(450 + 15000));
    expect_cleared(secret);
}

static void test_allocation_failure(unsigned stage)
{
    assert(stage >= 1 && stage <= 3);
    fail_creation_at = stage;
    assert(password_display_init(&screen) == ESP_ERR_NO_MEM);
    assert(request_from_network("NotInitialized") == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < object_count; ++i) assert(mock_objects[i].deleted);
    fail_creation_at = 0;
    init();
    assert(request_from_network("AfterRetry") == ESP_OK);
    assert(update_at(0));
    const char *secret = expect_visible("AfterRetry");
    assert(!update_at(15000));
    expect_cleared(secret);
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    const std::string scenario = argv[1];
    if (scenario == "invalid") test_invalid();
    else if (scenario == "fit") test_fit(false);
    else if (scenario == "fit_boundary") test_fit(true);
    else if (scenario == "overflow") test_overflow();
    else if (scenario == "tick_wrap") test_wrap();
    else if (scenario == "rotation") test_rotation();
    else if (scenario == "scroll_frames") test_scroll_frames();
    else if (scenario == "bitmap_fallback") test_bitmap_fallback();
    else if (scenario == "bitmap_start_time") test_bitmap_start_time();
    else if (scenario == "allocation_failure") {
        assert(argc == 3);
        test_allocation_failure(static_cast<unsigned>(argv[2][0] - '0'));
    }
    else assert(false);
    std::printf("Password display scenario passed: %s\n", argv[1]);
    return 0;
}
