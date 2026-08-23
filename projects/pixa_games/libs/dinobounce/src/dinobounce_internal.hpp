#pragma once

#include "h2_dinobounce.h"
#include "core/Scene.h"

#include <array>
#include <cstdint>
#include <memory>

struct h2_dinobounce;

class DinoBounceScene final : public pixelroot32::core::Scene {
public:
    explicit DinoBounceScene(h2_dinobounce *owner) : owner(owner) {}
    void init() override;
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;
private:
    void step();
    h2_dinobounce *owner;
};

struct h2_dinobounce_brick {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t color = 0;
    bool alive = false;
};

struct h2_dinobounce {
    explicit h2_dinobounce(const h2_dinobounce_config_t &value)
        : config(value), scene(this) {}
    h2_dinobounce_config_t config;
    DinoBounceScene scene;
    uint32_t random_state = 1;
    uint64_t accumulator_us = 0;
    uint64_t elapsed_us = 0;
    int32_t paddle_x = 90;
    int32_t paddle_dir = 0;
    int32_t ball_x = 11200;
    int32_t ball_y = 17200;
    int32_t ball_vx = 0;
    int32_t ball_vy = 0;
    int32_t ball_speed = 280;
    int32_t level = 0;
    int32_t descend_frames = 0;
    std::array<h2_dinobounce_brick, 24> bricks{};
    const char *clip_name = "run_right";
    uint32_t decoded_frame = UINT32_MAX;
    const char *decoded_clip = nullptr;
    std::unique_ptr<uint8_t[]> player_bgra;
    std::unique_ptr<uint8_t[]> player_argb4444;
    bool launched = false;
    bool game_over = false;
};
