#ifndef H2_PIXA_GAMES_DINODIVE_H
#define H2_PIXA_GAMES_DINODIVE_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PIXA_GAMES_DINODIVE_COMPONENT_LEFT 1u
#define H2_PIXA_GAMES_DINODIVE_COMPONENT_RIGHT 2u
#define H2_PIXA_GAMES_DINODIVE_COMPONENT_RECORD 3u

typedef struct h2_pixa_games_dinodive_config {
    /** Borrowed absolute Runtime filesystem path to a Player PIXA asset. */
    const char *player_pixa_path;
    /** Deterministic platform seed; zero uses the DinoDive canonical fallback. */
    uint32_t seed;
} h2_pixa_games_dinodive_config_t;

/**
 * Runs the blocking standalone DinoDive App.
 *
 * LEFT and RIGHT button-down events select the persistent movement direction.
 * RECORD starts a new game and resets then restarts after game over. Runtime,
 * config, and config strings are borrowed for the full call.
 */
int h2_pixa_games_dinodive_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinodive_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
