#include "dinorun_internal.hpp"

namespace {

constexpr h2_game_audio_step_t kJumpSteps[] = {
    {0u, 70u, 440u, 700u, 250u, 500u, H2_GAME_AUDIO_WAVE_PULSE},
};
constexpr h2_game_audio_step_t kGameOverSteps[] = {
    {0u, 120u, 330u, 220u, 280u, 500u, H2_GAME_AUDIO_WAVE_SAW},
    {130u, 180u, 220u, 110u, 260u, 500u, H2_GAME_AUDIO_WAVE_SAW},
};

} // namespace

const h2_game_audio_recipe_t h2_dinorun_jump_recipe = {
    kJumpSteps,
    sizeof(kJumpSteps) / sizeof(kJumpSteps[0]),
    70u,
};
const h2_game_audio_recipe_t h2_dinorun_game_over_recipe = {
    kGameOverSteps,
    sizeof(kGameOverSteps) / sizeof(kGameOverSteps[0]),
    310u,
};
