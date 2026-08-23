#include "polygon_battle_internal.hpp"

namespace {

constexpr h2_game_audio_step_t kShot[] = {
    {0u, 45u, 1200u, 1800u, 185u, 250u, H2_GAME_AUDIO_WAVE_PULSE},
};
constexpr h2_game_audio_step_t kHit[] = {
    {0u, 38u, 210u, 150u, 120u, 500u, H2_GAME_AUDIO_WAVE_NOISE},
};
constexpr h2_game_audio_step_t kDestroy[] = {
    {0u, 105u, 170u, 65u, 250u, 500u, H2_GAME_AUDIO_WAVE_NOISE},
    {0u, 90u, 310u, 120u, 180u, 500u, H2_GAME_AUDIO_WAVE_SAW},
};
constexpr h2_game_audio_step_t kPickup[] = {
    {0u, 55u, 660u, 880u, 190u, 500u, H2_GAME_AUDIO_WAVE_SINE},
    {60u, 80u, 880u, 1320u, 190u, 500u, H2_GAME_AUDIO_WAVE_SINE},
};
constexpr h2_game_audio_step_t kShield[] = {
    {0u, 120u, 520u, 180u, 210u, 500u, H2_GAME_AUDIO_WAVE_TRIANGLE},
};
constexpr h2_game_audio_step_t kPlayerHit[] = {
    {0u, 250u, 260u, 80u, 270u, 500u, H2_GAME_AUDIO_WAVE_NOISE},
};

} // namespace

const h2_game_audio_recipe_t h2_polygon_battle_shot_recipe = {kShot, 1u, 45u};
const h2_game_audio_recipe_t h2_polygon_battle_hit_recipe = {kHit, 1u, 38u};
const h2_game_audio_recipe_t h2_polygon_battle_destroy_recipe = {kDestroy, 2u, 105u};
const h2_game_audio_recipe_t h2_polygon_battle_pickup_recipe = {kPickup, 2u, 140u};
const h2_game_audio_recipe_t h2_polygon_battle_shield_recipe = {kShield, 1u, 120u};
const h2_game_audio_recipe_t h2_polygon_battle_player_hit_recipe = {kPlayerHit, 1u, 250u};
