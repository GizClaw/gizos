#ifndef H2_PIXA_GAMES_DINOBOUNCE_H
#define H2_PIXA_GAMES_DINOBOUNCE_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_LEFT 1u
#define H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_RIGHT 2u
#define H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_RECORD 3u

typedef struct h2_pixa_games_dinobounce_config {
    const char *player_pixa_path;
    uint32_t random_seed;
} h2_pixa_games_dinobounce_config_t;

int h2_pixa_games_dinobounce_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinobounce_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
