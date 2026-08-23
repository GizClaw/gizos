#pragma once

#include "h2_dinotetris.h"
#include "core/Scene.h"

#include <array>
#include <cstdint>

struct h2_dinotetris;

struct h2_dinotetris_piece {
    uint8_t type = 0;
    int8_t x = 3;
    int8_t y = 0;
    uint8_t rotation = 0;
};

class DinoTetrisScene final : public pixelroot32::core::Scene {
public:
    explicit DinoTetrisScene(h2_dinotetris *owner) : owner(owner) {}
    void init() override;
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;
private:
    void step();
    h2_dinotetris *owner;
};

struct h2_dinotetris {
    explicit h2_dinotetris(const h2_dinotetris_config_t &value)
        : config(value), scene(this),
          random_state(value.random_seed == 0u ? 1u : value.random_seed) {}
    h2_dinotetris_config_t config;
    DinoTetrisScene scene;
    std::array<std::array<uint8_t, 10>, 20> board{};
    h2_dinotetris_piece current{};
    uint32_t random_state;
    uint64_t accumulator_us = 0;
    uint32_t score = 0;
    uint32_t lines_cleared = 0;
    uint32_t level = 1;
    uint32_t game_over_animation_ms = 0;
    uint32_t restart_animation_ms = 0;
    uint16_t drop_frames = 0;
    uint16_t lock_frames = 0;
    uint16_t action_frames = 0;
    uint16_t horizontal_hold_frames = 0;
    uint8_t next_shape = 0;
    int8_t horizontal_hold_direction = 0;
    bool action_pressed = false;
    bool fast_drop = false;
    bool game_over = false;
    bool restart_requested = false;
    bool initialized = false;
};
