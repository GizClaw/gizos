#ifndef H2_PIXA_GAMES_DINORUN_H
#define H2_PIXA_GAMES_DINORUN_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PIXA_GAMES_DINORUN_COMPONENT_RECORD 1u

typedef struct h2_pixa_games_dinorun_config {
    /** Borrowed absolute Runtime filesystem path to a Player PIXA asset. */
    const char *player_pixa_path;
    /** Deterministic platform seed; zero uses the DinoRun canonical fallback. */
    uint32_t seed;
} h2_pixa_games_dinorun_config_t;

/**
 * Runs the blocking standalone DinoRun App.
 *
 * RECORD down starts charging and RECORD up jumps. After game over, the next
 * RECORD down resets the game and starts charging the retry. Runtime, config,
 * and config strings are borrowed for the full call.
 */
int h2_pixa_games_dinorun_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinorun_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
