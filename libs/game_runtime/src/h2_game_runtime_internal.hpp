#pragma once

#include "h2_game_runtime.h"

#include "graphics/DisplayConfig.h"
#include "graphics/DrawSurface.h"
#include "graphics/Renderer.h"
#include "core/Scene.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class H2PixelRootDisplaySurface final : public pixelroot32::graphics::DrawSurface {
public:
    H2PixelRootDisplaySurface(const h2_pal_display_api_t *display, int width, int height);
    ~H2PixelRootDisplaySurface() override;

    void init() override;
    void setRotation(uint16_t rotation) override;
    void clearBuffer() override;
    void sendBuffer() override;
    void drawFilledCircle(int x, int y, int radius, uint16_t color) override;
    void drawCircle(int x, int y, int radius, uint16_t color) override;
    void drawRectangle(int x, int y, int width, int height, uint16_t color) override;
    void drawFilledRectangle(int x, int y, int width, int height, uint16_t color) override;
    void drawLine(int x1, int y1, int x2, int y2, uint16_t color) override;
    void drawBitmap(int x, int y, int width, int height, const uint8_t *bitmap, uint16_t color) override;
    void drawPixel(int x, int y, uint16_t color) override;
    uint16_t *getPixelBuffer() override;
    void setContrast(uint8_t level) override;
    void setTextColor(uint16_t color) override;
    void setTextSize(uint8_t size) override;
    void setCursor(int16_t x, int16_t y) override;
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b) override;
    void setDisplaySize(int width, int height) override;
    void present() override;

    int last_status() const;

private:
    void putPixel(int x, int y, uint16_t color);

    const h2_pal_display_api_t *display_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    uint16_t *pixels_ = nullptr;
    uint32_t *cell_hashes_ = nullptr;
    int hash_columns_ = 0;
    int hash_rows_ = 0;
    bool has_presented_frame_ = false;
    int last_status_ = H2_GAME_RUNTIME_OK;
};

struct h2_game_runtime {
    const h2_pal_display_api_t *display = nullptr;
    int width = 0;
    int height = 0;
    uint32_t last_tick_ms = 0;
    bool has_last_tick = false;
    std::unique_ptr<pixelroot32::graphics::Renderer> renderer;
    pixelroot32::core::Scene *scene = nullptr;
    h2_game_input_handler_t input_handler = nullptr;
    h2_game_input_event_t input_queue[16] = {};
    size_t input_head = 0;
    size_t input_count = 0;
};

void h2_game_runtime_drain_input(h2_game_runtime_t *runtime);
