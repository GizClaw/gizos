#pragma once

#include "graphics/DisplayConfig.h"
#include "graphics/DrawSurface.h"
#include "graphics/Renderer.h"
#include "h2_game_text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

class H2GameTestSurface final : public pixelroot32::graphics::DrawSurface {
public:
    void init() override {}
    void setRotation(uint16_t) override {}
    void clearBuffer() override { pixels_.fill(0); }
    void sendBuffer() override {}
    void drawFilledCircle(int, int, int, uint16_t) override {}
    void drawCircle(int, int, int, uint16_t) override {}
    void drawRectangle(int, int, int, int, uint16_t) override {}
    void drawFilledRectangle(int x, int y, int width, int height, uint16_t color) override {
        for (int row = y; row < y + height; ++row) {
            for (int column = x; column < x + width; ++column) drawPixel(column, row, color);
        }
    }
    void drawLine(int, int, int, int, uint16_t) override {}
    void drawBitmap(int, int, int, int, const uint8_t *, uint16_t) override {}
    void drawPixel(int x, int y, uint16_t color) override {
        if (x >= 0 && x < 240 && y >= 0 && y < 240) {
            pixels_[static_cast<size_t>(y) * 240u + static_cast<size_t>(x)] = color;
        }
    }
    uint16_t *getPixelBuffer() override { return pixels_.data(); }
    void setContrast(uint8_t) override {}
    void setTextColor(uint16_t) override {}
    void setTextSize(uint8_t) override {}
    void setCursor(int16_t, int16_t) override {}
    uint16_t color565(uint8_t red, uint8_t green, uint8_t blue) override {
        return static_cast<uint16_t>(((red & 0xf8u) << 8u) |
                                     ((green & 0xfcu) << 3u) | (blue >> 3u));
    }
    void setDisplaySize(int, int) override {}
    void present() override {}

private:
    std::array<uint16_t, 240u * 240u> pixels_{};
};

inline pixelroot32::graphics::Renderer h2_game_test_renderer() {
    auto config = pixelroot32::graphics::DisplayConfig::createCustom(
        new H2GameTestSurface(), 240, 240);
    pixelroot32::graphics::Renderer renderer(std::move(config));
    renderer.init();
    return renderer;
}

struct H2GameMockTextState {
    const h2_game_text_span_t *expected = nullptr;
    bool *drawn = nullptr;
    size_t count = 0;
};

inline int h2_game_test_measure(
    void *,
    h2_game_text_span_t text,
    uint16_t line_height,
    h2_game_text_metrics_t *out_metrics) {
    if (out_metrics == nullptr) return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    out_metrics->width_px = static_cast<int32_t>(text.byte_len * line_height / 2u);
    out_metrics->advance_px = out_metrics->width_px;
    out_metrics->height_px = line_height;
    out_metrics->baseline_px = line_height;
    return H2_GAME_TEXT_OK;
}

inline int h2_game_test_draw(
    void *user,
    const h2_game_text_surface_t *,
    h2_game_text_span_t text,
    int32_t,
    int32_t,
    h2_game_text_style_t) {
    auto *state = static_cast<H2GameMockTextState *>(user);
    for (size_t index = 0; index < state->count; ++index) {
        const h2_game_text_span_t expected = state->expected[index];
        if (text.byte_len == expected.byte_len &&
            std::equal(text.data, text.data + text.byte_len, expected.data)) {
            state->drawn[index] = true;
        }
    }
    return H2_GAME_TEXT_OK;
}

inline h2_game_text_api_t h2_game_test_text_api(H2GameMockTextState *state) {
    static const h2_game_text_vtable_t vtable = {
        h2_game_test_measure,
        h2_game_test_draw,
    };
    return {state, &vtable};
}
