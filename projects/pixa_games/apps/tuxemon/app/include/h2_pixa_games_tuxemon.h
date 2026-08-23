#ifndef H2_PIXA_GAMES_TUXEMON_H
#define H2_PIXA_GAMES_TUXEMON_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PIXA_GAMES_TUXEMON_COMPONENT_UP 1u
#define H2_PIXA_GAMES_TUXEMON_COMPONENT_DOWN 2u
#define H2_PIXA_GAMES_TUXEMON_COMPONENT_LEFT 3u
#define H2_PIXA_GAMES_TUXEMON_COMPONENT_RIGHT 4u

typedef struct h2_pixa_games_tuxemon_config {
    const char *player_pixa_path;
} h2_pixa_games_tuxemon_config_t;

int h2_pixa_games_tuxemon_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_tuxemon_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
