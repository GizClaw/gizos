#include "dinodive_internal.hpp"

namespace {
// Rising two-voice electronic sweep matching the legacy player_jumps cue (~0.56 s).
constexpr h2_game_audio_step_t fall_steps[] = {
    {0, 280, 180, 520, 520, 500, H2_GAME_AUDIO_WAVE_PULSE},
    {250, 310, 360, 940, 330, 250, H2_GAME_AUDIO_WAVE_SAW},
};
// Stepped descending cadence and short tail matching fall_platform (~1.2 s).
constexpr h2_game_audio_step_t game_over_steps[] = {
    {0, 230, 660, 520, 600, 500, H2_GAME_AUDIO_WAVE_PULSE},
    {220, 230, 494, 392, 570, 500, H2_GAME_AUDIO_WAVE_PULSE},
    {440, 240, 370, 294, 540, 500, H2_GAME_AUDIO_WAVE_TRIANGLE},
    {670, 270, 277, 196, 500, 500, H2_GAME_AUDIO_WAVE_SAW},
    {920, 280, 185, 110, 420, 500, H2_GAME_AUDIO_WAVE_TRIANGLE},
};
}

const h2_game_audio_recipe_t h2_dinodive_fall_recipe = {fall_steps, 2, 560};
const h2_game_audio_recipe_t h2_dinodive_game_over_recipe = {game_over_steps, 5, 1220};
