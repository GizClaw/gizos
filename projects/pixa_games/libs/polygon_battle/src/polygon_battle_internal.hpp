#pragma once

#include "h2_polygon_battle.h"
#include "core/Scene.h"

#include <array>
#include <cstdint>

enum class PolygonBattlePhase : uint8_t { Ready, Playing, GameOver };
enum class PolygonBattlePickup : uint8_t { Spread, Pierce, Ricochet, Power, Shield };

struct PolygonBattleEnemy {
    bool active = false;
    bool destroy_pending = false;
    uint16_t id = 0;
    h2_polygon_battle_shape_t shape = H2_POLYGON_BATTLE_SHAPE_CIRCLE;
    int32_t x = 0;
    int32_t y = 0;
    int16_t vx = 0;
    uint16_t fire_frames = 0;
    uint16_t destroy_frames = 0;
    uint8_t hp = 0;
    uint8_t max_hp = 0;
    bool alternate_fire = false;
};

struct PolygonBattleProjectile {
    bool active = false;
    int32_t x = 0;
    int32_t y = 0;
    int16_t vx = 0;
    int16_t vy = 0;
    uint16_t hit_ids[2]{};
    uint8_t hit_count = 0;
    uint8_t hit_capacity = 1;
    uint8_t bounces = 0;
    uint8_t damage = 1;
};

struct PolygonBattleEnemyProjectile {
    bool active = false;
    int32_t x = 0;
    int32_t y = 0;
    int16_t vx = 0;
    int16_t vy = 0;
    h2_polygon_battle_shape_t source_shape = H2_POLYGON_BATTLE_SHAPE_TRIANGLE;
};

struct PolygonBattlePickupState {
    bool active = false;
    int32_t x = 0;
    int32_t y = 0;
    PolygonBattlePickup kind = PolygonBattlePickup::Spread;
};

struct h2_polygon_battle;

class PolygonBattleScene final : public pixelroot32::core::Scene {
public:
    explicit PolygonBattleScene(h2_polygon_battle *value) : owner(value) {}
    void init() override;
    void update(unsigned long delta_ms) override;
    void draw(pixelroot32::graphics::Renderer &renderer) override;
    void step();
private:
    h2_polygon_battle *owner;
};

struct h2_polygon_battle {
    explicit h2_polygon_battle(const h2_polygon_battle_config_t &value)
        : config(value), scene(this), random_state(value.random_seed == 0u ? 1u : value.random_seed) {}
    h2_polygon_battle_config_t config;
    PolygonBattleScene scene;
    std::array<PolygonBattleEnemy, 12> enemies{};
    std::array<PolygonBattleProjectile, 48> projectiles{};
    std::array<PolygonBattleEnemyProjectile, 32> enemy_projectiles{};
    std::array<PolygonBattlePickupState, 4> pickups{};
    PolygonBattlePhase phase = PolygonBattlePhase::Ready;
    uint64_t accumulator_us = 0;
    uint32_t random_state = 1;
    uint32_t score = 0;
    uint32_t destroyed_count = 0;
    uint64_t elapsed_us = 0;
    uint16_t next_enemy_id = 1;
    uint16_t combo = 0;
    uint16_t max_combo = 0;
    uint16_t shot_frames = 0;
    uint16_t invulnerable_frames = 0;
    uint16_t wave_pause_frames = 0;
    int32_t player_x = 120000;
    uint32_t wave = 0;
    uint8_t life = 3;
    uint8_t spread = 1;
    uint8_t pierce = 1;
    uint8_t ricochet = 0;
    uint8_t power = 1;
    bool shield = false;
    bool left_down = false;
    bool right_down = false;
    bool action_down = false;
    bool audio_failed = false;
};

extern const h2_game_audio_recipe_t h2_polygon_battle_shot_recipe;
extern const h2_game_audio_recipe_t h2_polygon_battle_hit_recipe;
extern const h2_game_audio_recipe_t h2_polygon_battle_destroy_recipe;
extern const h2_game_audio_recipe_t h2_polygon_battle_pickup_recipe;
extern const h2_game_audio_recipe_t h2_polygon_battle_shield_recipe;
extern const h2_game_audio_recipe_t h2_polygon_battle_player_hit_recipe;
