#pragma once

#include "h2_dinodive.h"
#include "core/PhysicsActor.h"
#include "core/Scene.h"
#include "physics/RigidActor.h"

#include <cstdint>
#include <memory>

constexpr int kDinoDivePlatformCount = 6;
constexpr int kDinoDiveSpikeWidth = 40;
constexpr int kDinoDiveSpikeHeight = 11;

struct h2_dinodive;

class DinoDivePlayerActor final : public pixelroot32::physics::RigidActor {
public:
    explicit DinoDivePlayerActor(h2_dinodive *owner);
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;
    void onCollision(pixelroot32::core::Actor *other) override;

    bool contacted_floor = false;
    bool contacted_hazard = false;
    pixelroot32::core::PhysicsActor *floor_contact = nullptr;

private:
    h2_dinodive *owner_;
};

class DinoDivePlatformActor final : public pixelroot32::core::PhysicsActor {
public:
    DinoDivePlatformActor();
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;

    bool has_spike = false;
    bool spike_on_right = false;
};

class DinoDiveHazardActor final : public pixelroot32::core::PhysicsActor {
public:
    DinoDiveHazardActor();
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;
};

class DinoDiveScene final : public pixelroot32::core::Scene {
public:
    explicit DinoDiveScene(h2_dinodive *owner) : owner_(owner) {}
    void init() override;
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;

private:
    void stepPhysics();
    h2_dinodive *owner_;
};

struct h2_dinodive {
    explicit h2_dinodive(const h2_dinodive_config_t &value);

    h2_dinodive_config_t config;
    DinoDiveScene scene;
    DinoDivePlayerActor player;
    DinoDivePlatformActor platforms[kDinoDivePlatformCount];
    DinoDiveHazardActor hazards[kDinoDivePlatformCount];
    uint32_t initial_seed = 1;
    uint32_t random_state = 1;
    uint64_t elapsed_us = 0;
    uint64_t accumulator_us = 0;
    int32_t direction = 1;
    int32_t floor_count = 0;
    bool started = false;
    bool game_over = false;
    bool was_on_floor = false;
    int32_t decoded_player_direction = 0;
    uint32_t decoded_player_frame = 0;
    bool decoded_player_frame_valid = false;
    std::unique_ptr<uint8_t[]> player_bgra;
    std::unique_ptr<uint8_t[]> player_argb4444;
};

extern const h2_game_audio_recipe_t h2_dinodive_fall_recipe;
extern const h2_game_audio_recipe_t h2_dinodive_game_over_recipe;
