#ifndef H2_PIXA_GAMES_DINOTETRIS_H
#define H2_PIXA_GAMES_DINOTETRIS_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PIXA_GAMES_DINOTETRIS_COMPONENT_LEFT 1u
#define H2_PIXA_GAMES_DINOTETRIS_COMPONENT_RIGHT 2u
#define H2_PIXA_GAMES_DINOTETRIS_COMPONENT_RECORD 3u

typedef struct h2_pixa_games_dinotetris_config {
    uint32_t random_seed;
} h2_pixa_games_dinotetris_config_t;

int h2_pixa_games_dinotetris_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinotetris_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
