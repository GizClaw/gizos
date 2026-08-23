#ifndef H2_DINOTETRIS_H
#define H2_DINOTETRIS_H

#include "h2_game_runtime.h"
#include "h2_game_text.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_dinotetris h2_dinotetris_t;

typedef enum h2_dinotetris_result_code {
    H2_DINOTETRIS_OK = 0,
    H2_DINOTETRIS_ERR_INVALID_ARGUMENT = -1,
    H2_DINOTETRIS_ERR_NO_MEMORY = -2,
} h2_dinotetris_result_code_t;

typedef enum h2_dinotetris_button {
    H2_DINOTETRIS_BUTTON_LEFT = 0,
    H2_DINOTETRIS_BUTTON_RIGHT = 1,
    H2_DINOTETRIS_BUTTON_ACTION = 2,
} h2_dinotetris_button_t;

typedef enum h2_dinotetris_event {
    H2_DINOTETRIS_EVENT_ROTATE = 0,
    H2_DINOTETRIS_EVENT_FAST_DROP = 1,
    H2_DINOTETRIS_EVENT_PIECE_LOCKED = 2,
    H2_DINOTETRIS_EVENT_GAME_OVER = 3,
} h2_dinotetris_event_t;

/** Synchronous observer; must not re-enter, reset, or destroy the game. */
typedef void (*h2_dinotetris_event_callback_t)(
    void *user,
    h2_dinotetris_event_t event);

typedef struct h2_dinotetris_texts {
    h2_game_text_span_t score;
    h2_game_text_span_t level;
    h2_game_text_span_t next;
    h2_game_text_span_t game_over;
    h2_game_text_span_t experience;
} h2_dinotetris_texts_t;

typedef struct h2_dinotetris_config {
    /** Borrowed synchronous text provider. */
    const h2_game_text_api_t *text;
    /** Borrowed localized catalog. */
    const h2_dinotetris_texts_t *texts;
    uint32_t random_seed;
    h2_dinotetris_event_callback_t event_callback;
    void *event_user;
} h2_dinotetris_config_t;

typedef struct h2_dinotetris_result {
    uint32_t score;
    uint32_t lines_cleared;
    uint32_t level;
    uint8_t game_over;
} h2_dinotetris_result_t;

const h2_dinotetris_texts_t *h2_dinotetris_english_texts(void);

int h2_dinotetris_create(
    const h2_dinotetris_config_t *config,
    h2_dinotetris_t **out_game);
h2_game_scene_t *h2_dinotetris_scene(h2_dinotetris_t *game);
void h2_dinotetris_handle_input(
    h2_dinotetris_t *game,
    const h2_game_input_event_t *event);
void h2_dinotetris_reset(h2_dinotetris_t *game);
int h2_dinotetris_get_result(
    const h2_dinotetris_t *game,
    h2_dinotetris_result_t *out_result);
void h2_dinotetris_destroy(h2_dinotetris_t *game);

#ifdef __cplusplus
}
#endif

#endif
