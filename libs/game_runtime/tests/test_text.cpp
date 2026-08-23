#include "h2_game_text.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {

struct MockState {
    const char *expected_data;
    size_t expected_len;
    int measure_calls;
    int draw_calls;
};

int mock_measure(
    void *user,
    h2_game_text_span_t text,
    uint16_t line_height_px,
    h2_game_text_metrics_t *out_metrics) {
    auto *state = static_cast<MockState *>(user);
    assert(text.data == state->expected_data);
    assert(text.byte_len == state->expected_len);
    assert(line_height_px == 16u);
    ++state->measure_calls;
    *out_metrics = {37, 40, 16, 13};
    return H2_GAME_TEXT_OK;
}

int mock_draw(
    void *user,
    const h2_game_text_surface_t *,
    h2_game_text_span_t text,
    int32_t x,
    int32_t y,
    h2_game_text_style_t style) {
    auto *state = static_cast<MockState *>(user);
    assert(text.data == state->expected_data);
    assert(text.byte_len == state->expected_len);
    assert(x == -4);
    assert(y == 7);
    assert(style.line_height_px == 16u);
    ++state->draw_calls;
    return H2_GAME_TEXT_ERR_PROVIDER;
}

int invalid_metrics(
    void *,
    h2_game_text_span_t,
    uint16_t,
    h2_game_text_metrics_t *out_metrics) {
    *out_metrics = {-1, 0, 8, 7};
    return H2_GAME_TEXT_OK;
}

int failing_measure(
    void *,
    h2_game_text_span_t,
    uint16_t,
    h2_game_text_metrics_t *out_metrics) {
    *out_metrics = {12, 13, 14, 10};
    return H2_GAME_TEXT_ERR_PROVIDER;
}

void assert_empty_metrics(const h2_game_text_metrics_t &metrics) {
    assert(metrics.width_px == 0);
    assert(metrics.advance_px == 0);
    assert(metrics.height_px == 0);
    assert(metrics.baseline_px == 0);
}

void test_utf8_validation_and_exact_dispatch() {
    static constexpr char kChinese[] = "开始游戏";
    MockState state{kChinese, sizeof(kChinese) - 1u, 0, 0};
    const h2_game_text_vtable_t vtable{mock_measure, mock_draw};
    const h2_game_text_api_t api{&state, &vtable};
    const h2_game_text_span_t text{kChinese, sizeof(kChinese) - 1u};
    h2_game_text_metrics_t metrics{};
    uint16_t pixels[4]{};
    const h2_game_text_surface_t surface{pixels, 2u, 2u, 2u, 4u};

    assert(h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK);
    assert(h2_game_text_measure(&api, text, 16u, &metrics) == H2_GAME_TEXT_OK);
    assert(metrics.width_px == 37);
    assert(metrics.advance_px == 40);
    assert(h2_game_text_draw(&api, &surface, text, -4, 7, {0xFFFFu, 16u}) ==
        H2_GAME_TEXT_ERR_PROVIDER);
    assert(state.measure_calls == 1);
    assert(state.draw_calls == 1);

    const h2_game_text_vtable_t invalid_vtable{invalid_metrics, mock_draw};
    const h2_game_text_api_t invalid_api{&state, &invalid_vtable};
    metrics = {1, 2, 3, 4};
    assert(h2_game_text_measure(&invalid_api, text, 16u, &metrics) ==
        H2_GAME_TEXT_ERR_PROVIDER);
    assert_empty_metrics(metrics);

    const h2_game_text_vtable_t failing_vtable{failing_measure, mock_draw};
    const h2_game_text_api_t failing_api{&state, &failing_vtable};
    metrics = {1, 2, 3, 4};
    assert(h2_game_text_measure(&failing_api, text, 16u, &metrics) ==
        H2_GAME_TEXT_ERR_PROVIDER);
    assert_empty_metrics(metrics);

    static constexpr char kOverlong[] = "\xC0\xAF";
    static constexpr char kSurrogate[] = "\xED\xA0\x80";
    static constexpr char kTruncated[] = "\xF0\x9F\xA6";
    assert(h2_game_text_validate_utf8({kOverlong, sizeof(kOverlong) - 1u}) ==
        H2_GAME_TEXT_ERR_INVALID_UTF8);
    assert(h2_game_text_validate_utf8({kSurrogate, sizeof(kSurrogate) - 1u}) ==
        H2_GAME_TEXT_ERR_INVALID_UTF8);
    assert(h2_game_text_validate_utf8({kTruncated, sizeof(kTruncated) - 1u}) ==
        H2_GAME_TEXT_ERR_INVALID_UTF8);
    metrics = {1, 2, 3, 4};
    assert(h2_game_text_measure(
        &api, {kTruncated, sizeof(kTruncated) - 1u}, 16u, &metrics) ==
        H2_GAME_TEXT_ERR_INVALID_UTF8);
    assert_empty_metrics(metrics);
}

void test_builtin_font_metrics_and_clipping() {
    const h2_game_text_api_t api = h2_game_text_builtin_5x7();
    const h2_game_text_span_t text H2_GAME_TEXT_LITERAL("A");
    h2_game_text_metrics_t metrics{};
    uint16_t pixels[16]{};
    const h2_game_text_surface_t surface{pixels, 4u, 4u, 4u, 16u};

    assert(h2_game_text_measure(&api, text, 8u, &metrics) == H2_GAME_TEXT_OK);
    assert(metrics.width_px == 5);
    assert(metrics.advance_px == 6);
    assert(metrics.height_px == 7);
    assert(metrics.baseline_px == 7);

    uint16_t glyph_pixels[6u * 8u]{};
    const h2_game_text_surface_t glyph_surface{
        glyph_pixels, 6u, 8u, 6u, 6u * 8u};
    assert(h2_game_text_draw(&api, &glyph_surface, text, 0, 0, {0x55AAu, 8u}) ==
        H2_GAME_TEXT_OK);
    static constexpr uint16_t kRows[7] = {
        0x04u, 0x0Au, 0x11u, 0x11u, 0x1Fu, 0x11u, 0x11u};
    for (size_t row = 0; row < 8u; ++row) {
        for (size_t column = 0; column < 6u; ++column) {
            const bool expected = row < 7u && column < 5u &&
                (kRows[row] & (1u << (4u - column))) != 0u;
            assert(glyph_pixels[row * 6u + column] ==
                (expected ? 0x55AAu : 0u));
        }
    }

    h2_game_text_metrics_t empty_metrics{-1, -1, -1, -1};
    assert(h2_game_text_measure(&api, {nullptr, 0u}, 8u, &empty_metrics) ==
        H2_GAME_TEXT_OK);
    assert(empty_metrics.width_px == 0 && empty_metrics.advance_px == 0 &&
           empty_metrics.height_px == 7);
    assert(h2_game_text_draw(&api, &surface, {nullptr, 0u}, 0, 0, {0xFFFFu, 8u}) ==
        H2_GAME_TEXT_OK);
    assert(h2_game_text_draw(&api, &surface, text, -2, -2, {0x1234u, 8u}) ==
        H2_GAME_TEXT_OK);
    bool wrote_pixel = false;
    for (uint16_t pixel : pixels) wrote_pixel = wrote_pixel || pixel == 0x1234u;
    assert(wrote_pixel);

    static constexpr char kChinese[] = "中";
    assert(h2_game_text_measure(
        &api, {kChinese, sizeof(kChinese) - 1u}, 8u, &metrics) ==
        H2_GAME_TEXT_ERR_UNSUPPORTED_GLYPH);
    const h2_game_text_surface_t too_small{pixels, 4u, 4u, 4u, 15u};
    assert(h2_game_text_draw(&api, &too_small, text, 0, 0, {0xFFFFu, 8u}) ==
        H2_GAME_TEXT_ERR_INVALID_ARGUMENT);
    const h2_game_text_surface_t overflowing{
        pixels, 1u, SIZE_MAX, 2u, SIZE_MAX};
    assert(h2_game_text_draw(&api, &overflowing, text, 0, 0, {0xFFFFu, 8u}) ==
        H2_GAME_TEXT_ERR_OVERFLOW);
}

} // namespace

int main() {
    test_utf8_validation_and_exact_dispatch();
    test_builtin_font_metrics_and_clipping();
    return 0;
}
