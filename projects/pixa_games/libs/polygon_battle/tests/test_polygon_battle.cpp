#include "polygon_battle_internal.hpp"
#include "h2_game_test_render.hpp"

#include <cassert>

struct Events {
    int shots = 0;
    int hits = 0;
    int destroyed = 0;
    uint8_t last_damage = 0;
    uint32_t last_wave = 0;
};

static void onEvent(void *user, const h2_polygon_battle_event_t *event) {
    auto *events = static_cast<Events *>(user);
    if (event->type == H2_POLYGON_BATTLE_EVENT_SHOT) ++events->shots;
    if (event->type == H2_POLYGON_BATTLE_EVENT_ENEMY_HIT) {
        ++events->hits;
        events->last_damage = event->applied_damage;
    }
    if (event->type == H2_POLYGON_BATTLE_EVENT_ENEMY_DESTROYED) ++events->destroyed;
    if (event->type == H2_POLYGON_BATTLE_EVENT_WAVE_STARTED) events->last_wave = event->wave;
}

static void input(h2_polygon_battle *game, h2_game_input_type_t type, uint8_t button) {
    const h2_game_input_event_t event = {type, 0, 0, button};
    h2_polygon_battle_handle_input(game, &event);
}

static PolygonBattleEnemy enemy(uint16_t id, h2_polygon_battle_shape_t shape, int32_t x, int32_t y) {
    PolygonBattleEnemy value{};
    value.active = true;
    value.id = id;
    value.shape = shape;
    value.max_hp = shape == H2_POLYGON_BATTLE_SHAPE_CIRCLE ? 1u : static_cast<uint8_t>(shape);
    value.hp = value.max_hp;
    value.x = x;
    value.y = y;
    value.fire_frames = 1000u;
    return value;
}

int main() {
    assert(h2_polygon_battle_shot_recipe.duration_ms == 45u);
    assert(h2_polygon_battle_pickup_recipe.duration_ms == 140u);
    assert(h2_polygon_battle_player_hit_recipe.duration_ms == 250u);
    const h2_game_audio_recipe_t *recipes[] = {
        &h2_polygon_battle_shot_recipe, &h2_polygon_battle_hit_recipe,
        &h2_polygon_battle_destroy_recipe, &h2_polygon_battle_pickup_recipe,
        &h2_polygon_battle_shield_recipe, &h2_polygon_battle_player_hit_recipe};
    for (const auto *recipe : recipes) {
        assert(recipe->steps != nullptr && recipe->step_count != 0u);
        for (size_t index = 0; index < recipe->step_count; ++index) {
            const auto &step = recipe->steps[index];
            assert(step.start_ms + step.duration_ms <= recipe->duration_ms);
            assert(step.frequency_hz != 0u && step.end_frequency_hz != 0u);
            assert(step.volume_permille <= 1000u && step.duty_permille <= 1000u);
        }
    }

    Events events;
    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    const h2_polygon_battle_config_t config = {
        nullptr, &text, h2_polygon_battle_english_texts(), 0x12345678u, onEvent, &events};
    h2_polygon_battle_t *invalid_game = reinterpret_cast<h2_polygon_battle_t *>(1);
    assert(h2_polygon_battle_create(nullptr, &invalid_game) ==
           H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT);
    assert(invalid_game == nullptr);
    assert(h2_polygon_battle_create(&config, nullptr) ==
           H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT);
    h2_polygon_battle_config_t invalid_config = config;
    h2_polygon_battle_texts_t invalid_texts = *config.texts;
    invalid_texts.title = {};
    invalid_config.texts = &invalid_texts;
    invalid_game = reinterpret_cast<h2_polygon_battle_t *>(1);
    assert(h2_polygon_battle_create(&invalid_config, &invalid_game) ==
           H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT);
    assert(invalid_game == nullptr);

    h2_polygon_battle_t *game = nullptr;
    assert(h2_polygon_battle_create(&config, &game) == H2_POLYGON_BATTLE_OK);
    assert(game != nullptr && game->phase == PolygonBattlePhase::Ready);

    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_POLYGON_BATTLE_BUTTON_ACTION);
    assert(game->phase == PolygonBattlePhase::Playing && game->wave == 1u);
    int wave_one_enemies = 0;
    int wave_one_shooters = 0;
    for (const auto &wave_enemy : game->enemies) if (wave_enemy.active) {
        ++wave_one_enemies;
        if (wave_enemy.shape != H2_POLYGON_BATTLE_SHAPE_CIRCLE) ++wave_one_shooters;
    }
    assert(wave_one_enemies == 6 && wave_one_shooters == 2);
    game->scene.step();
    assert(events.shots == 1);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_POLYGON_BATTLE_BUTTON_ACTION);

    game->enemies = {};
    game->left_down = true;
    game->player_x = 20000;
    game->scene.step();
    assert(game->player_x == 20000);
    game->left_down = false;
    game->right_down = true;
    game->player_x = 220000;
    game->scene.step();
    assert(game->player_x == 220000);
    game->right_down = false;

    game->enemies = {};
    game->projectiles = {};
    game->spread = 3u;
    game->pierce = 2u;
    game->ricochet = 2u;
    game->power = 2u;
    game->shot_frames = 0u;
    for (size_t index = 0; index < game->projectiles.size() - 2u; ++index) {
        game->projectiles[index].active = true;
    }
    const int shots_before_full_fire = events.shots;
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_POLYGON_BATTLE_BUTTON_ACTION);
    int active = 0;
    for (const auto &projectile : game->projectiles) {
        if (projectile.active) ++active;
    }
    assert(active == 46);
    assert(events.shots == shots_before_full_fire);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_POLYGON_BATTLE_BUTTON_ACTION);

    game->projectiles = {};
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_POLYGON_BATTLE_BUTTON_ACTION);
    active = 0;
    for (const auto &projectile : game->projectiles) if (projectile.active) {
        ++active;
        assert(projectile.hit_capacity == 2u && projectile.bounces == 2u && projectile.damage == 2u);
    }
    assert(active == 3);
    input(game, H2_GAME_INPUT_BUTTON_UP, H2_POLYGON_BATTLE_BUTTON_ACTION);

    game->projectiles = {};
    game->enemies = {};
    game->enemies[0] = enemy(10u, H2_POLYGON_BATTLE_SHAPE_TRIANGLE, 100000, 100000);
    game->projectiles[0].active = true;
    game->projectiles[0].x = 100000;
    game->projectiles[0].y = 100000;
    game->projectiles[0].hit_capacity = 2u;
    game->projectiles[0].damage = 2u;
    game->scene.step();
    assert(game->enemies[0].hp == 1u && events.last_damage == 2u);
    game->scene.step();
    assert(game->enemies[0].hp == 1u);

    game->projectiles = {};
    game->enemies = {};
    game->enemies[0] = enemy(11u, H2_POLYGON_BATTLE_SHAPE_CIRCLE, 100000, 100000);
    game->enemies[1] = enemy(12u, H2_POLYGON_BATTLE_SHAPE_SQUARE, 100000, 100000);
    game->projectiles[0].active = true;
    game->projectiles[0].x = 100000;
    game->projectiles[0].y = 100000;
    game->projectiles[0].hit_capacity = 2u;
    game->projectiles[0].damage = 2u;
    game->scene.step();
    assert(game->enemies[0].destroy_pending && game->enemies[0].hp == 0u);
    assert(game->enemies[1].hp == 2u);
    assert(events.last_damage == 2u);

    game->projectiles = {};
    game->projectiles[0].active = true;
    game->projectiles[0].x = 3100;
    game->projectiles[0].y = 13100;
    game->projectiles[0].vx = -60;
    game->projectiles[0].vy = -200;
    game->projectiles[0].bounces = 2u;
    game->scene.step();
    assert(game->projectiles[0].active && game->projectiles[0].bounces == 1u);
    assert(game->projectiles[0].vx == 60 && game->projectiles[0].vy == 200);

    game->enemy_projectiles = {};
    game->enemies = {};
    game->enemies[0] = enemy(20u, H2_POLYGON_BATTLE_SHAPE_CIRCLE, 100000, 50000);
    game->enemies[0].fire_frames = 0u;
    game->scene.step();
    for (const auto &shot : game->enemy_projectiles) assert(!shot.active);
    game->enemies[0] = enemy(21u, H2_POLYGON_BATTLE_SHAPE_TRIANGLE, 100000, 50000);
    game->enemies[0].fire_frames = 0u;
    game->wave = 1u;
    game->scene.step();
    bool enemy_fired = false;
    for (const auto &shot : game->enemy_projectiles) enemy_fired = enemy_fired || shot.active;
    assert(enemy_fired);
    assert(game->enemies[0].fire_frames >= 240u && game->enemies[0].fire_frames <= 279u);
    int16_t wave_one_speed = 0;
    for (const auto &shot : game->enemy_projectiles) if (shot.active) wave_one_speed = shot.vy;
    assert(wave_one_speed == 90);

    game->enemy_projectiles = {};
    game->enemies[0].fire_frames = 0u;
    game->wave = 5u;
    game->scene.step();
    assert(game->enemies[0].fire_frames >= 85u && game->enemies[0].fire_frames <= 124u);
    int16_t wave_five_speed = 0;
    for (const auto &shot : game->enemy_projectiles) if (shot.active) wave_five_speed = shot.vy;
    assert(wave_five_speed == 150 && wave_five_speed > wave_one_speed);

    game->enemy_projectiles = {};
    game->enemies = {};
    game->wave = 5u;
    game->wave_pause_frames = 0u;
    for (int frame = 0; frame < 55; ++frame) game->scene.step();
    assert(game->phase == PolygonBattlePhase::Playing);
    assert(game->wave == 6u && events.last_wave == 6u);
    int wave_six_enemies = 0;
    for (const auto &wave_enemy : game->enemies) if (wave_enemy.active) ++wave_six_enemies;
    assert(wave_six_enemies == 10);

    game->enemy_projectiles = {};
    game->enemies[1].fire_frames = 0u;
    game->wave = 100u;
    game->scene.step();
    assert(game->enemies[1].fire_frames >= 85u && game->enemies[1].fire_frames <= 124u);
    int16_t capped_speed = 0;
    for (const auto &shot : game->enemy_projectiles) if (shot.active) capped_speed = shot.vy;
    assert(capped_speed == 150);

    auto renderer = h2_game_test_renderer();
    game->scene.draw(renderer);
    const uint16_t *pixels = renderer.getDrawSurface().getPixelBuffer();
    size_t solid_enemy_pixels = 0u;
    for (size_t index = 0; index < 240u * 240u; ++index) {
        if (pixels[index] == 0x8b5fu) ++solid_enemy_pixels;
    }
    assert(solid_enemy_pixels > 100u);
    h2_polygon_battle_result_t result{};
    assert(h2_polygon_battle_get_result(game, &result) == H2_POLYGON_BATTLE_OK);
    assert(result.spread == 3u && result.pierce == 2u && result.ricochet == 2u && result.power == 2u);
    game->combo = 9u;
    game->max_combo = 12u;
    game->shot_frames = 7u;
    game->invulnerable_frames = 8u;
    game->wave_pause_frames = 9u;
    game->next_enemy_id = 42u;
    h2_polygon_battle_reset(game);
    assert(game->phase == PolygonBattlePhase::Ready);
    assert(game->combo == 0u && game->max_combo == 0u);
    assert(game->shot_frames == 0u && game->invulnerable_frames == 0u);
    assert(game->wave_pause_frames == 0u && game->next_enemy_id == 1u);
    input(game, H2_GAME_INPUT_BUTTON_DOWN, H2_POLYGON_BATTLE_BUTTON_ACTION);
    for (int frame = 0; frame < 60; ++frame) game->scene.step();
    assert(h2_polygon_battle_get_result(game, &result) ==
           H2_POLYGON_BATTLE_OK);
    assert(result.elapsed_ms == 1000u);
    h2_polygon_battle_destroy(game);
    return 0;
}
