#ifndef H2_DINODIVE_H
#define H2_DINODIVE_H

#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "h2_game_text.h"
#include "pixa.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_DINODIVE_OK 0
#define H2_DINODIVE_ERR_INVALID_ARG -1
#define H2_DINODIVE_ERR_NO_MEMORY -2
#define H2_DINODIVE_ERR_ASSET -3

typedef struct h2_dinodive h2_dinodive_t;

typedef enum h2_dinodive_button {
    H2_DINODIVE_BUTTON_LEFT = 1,
    H2_DINODIVE_BUTTON_RIGHT = 2,
    H2_DINODIVE_BUTTON_ACTION = 3,
} h2_dinodive_button_t;

typedef enum h2_dinodive_event {
    H2_DINODIVE_EVENT_FALL = 1,
    H2_DINODIVE_EVENT_GAME_OVER = 2,
} h2_dinodive_event_t;

typedef void (*h2_dinodive_event_callback_t)(void *user, h2_dinodive_event_t event);

typedef struct h2_dinodive_texts {
    h2_game_text_span_t floor;
    h2_game_text_span_t press_start;
} h2_dinodive_texts_t;

typedef struct h2_dinodive_config {
    /** Borrowed; must remain valid until h2_dinodive_destroy(). */
    const pixa_asset_t *player;
    /** Borrowed and already started; may be NULL to disable sound in tests. */
    h2_game_audio_t *audio;
    /** Borrowed synchronous text provider. */
    const h2_game_text_api_t *text;
    /** Borrowed localized catalog. */
    const h2_dinodive_texts_t *texts;
    uint32_t seed;
    /** Synchronous observer; it must not destroy, reset, or re-enter this game from the callback. */
    h2_dinodive_event_callback_t event_callback;
    void *event_user;
} h2_dinodive_config_t;

typedef struct h2_dinodive_result {
    int32_t floor_count;
    int game_over;
} h2_dinodive_result_t;

const h2_dinodive_texts_t *h2_dinodive_english_texts(void);

/** Creates a game that borrows all config pointers until destroy. */
int h2_dinodive_create(const h2_dinodive_config_t *config, h2_dinodive_t **out_game);
/** Returns the borrowed PixelRoot scene owned by game. */
h2_game_scene_t *h2_dinodive_scene(h2_dinodive_t *game);
/** Handles DOWN for direction and action roles; UP is accepted as a no-op. */
void h2_dinodive_handle_input(h2_dinodive_t *game, const h2_game_input_event_t *event);
/** Restores the deterministic initial gameplay and seed state. */
void h2_dinodive_reset(h2_dinodive_t *game);
/** Copies the current score and terminal state into out_result. */
int h2_dinodive_get_result(const h2_dinodive_t *game, h2_dinodive_result_t *out_result);
void h2_dinodive_destroy(h2_dinodive_t *game);

#ifdef __cplusplus
}
#endif

#endif
