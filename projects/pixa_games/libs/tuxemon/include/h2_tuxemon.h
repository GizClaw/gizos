#ifndef H2_TUXEMON_H
#define H2_TUXEMON_H

#include "h2_game_runtime.h"
#include "pixa_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_TUXEMON_OK 0
#define H2_TUXEMON_ERR_INVALID_ARG -1
#define H2_TUXEMON_ERR_NO_MEMORY -2
#define H2_TUXEMON_ERR_ASSET -3

typedef struct h2_tuxemon h2_tuxemon_t;

/**
 * Borrowed assets used by the game. Their lifetime must cover `h2_tuxemon_t`.
 * The player canvas must be between 1 x 1 and 128 x 128 pixels.
 */
typedef struct h2_tuxemon_config {
    const pixa_asset_t *player;
} h2_tuxemon_config_t;

/** Logical movement buttons accepted by the Tuxemon scene. */
typedef enum h2_tuxemon_button {
    H2_TUXEMON_BUTTON_UP = 1,
    H2_TUXEMON_BUTTON_DOWN = 2,
    H2_TUXEMON_BUTTON_LEFT = 3,
    H2_TUXEMON_BUTTON_RIGHT = 4,
} h2_tuxemon_button_t;

/** Observable prototype state used by host integrations and tests. */
typedef struct h2_tuxemon_state {
    uint8_t room;
    uint8_t tile_x;
    uint8_t tile_y;
    uint8_t moving;
    uint8_t is_world;
    uint8_t viewport_tile_x;
    uint8_t viewport_tile_y;
    uint32_t room_change_count;
} h2_tuxemon_state_t;

/** Allocate a Tuxemon game instance. The caller owns the returned handle. */
int h2_tuxemon_create(
    const h2_tuxemon_config_t *config,
    h2_tuxemon_t **out_game);

/** Borrow the game scene for the lifetime of `game`. */
h2_game_scene_t *h2_tuxemon_scene(h2_tuxemon_t *game);

/** Deliver one Game Runtime input event to the scene. */
void h2_tuxemon_handle_input(
    h2_tuxemon_t *game,
    const h2_game_input_event_t *event);

/** Copy the current room and player state into `out_state`. */
int h2_tuxemon_get_state(
    const h2_tuxemon_t *game,
    h2_tuxemon_state_t *out_state);

/** Destroy a game instance. A null handle is accepted. */
void h2_tuxemon_destroy(h2_tuxemon_t *game);

#ifdef __cplusplus
}
#endif

#endif
