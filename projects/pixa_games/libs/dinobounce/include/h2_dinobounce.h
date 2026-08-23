#ifndef H2_DINOBOUNCE_H
#define H2_DINOBOUNCE_H

#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "h2_game_text.h"
#include "pixa.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_dinobounce h2_dinobounce_t;

typedef enum h2_dinobounce_result_code {
    H2_DINOBOUNCE_OK = 0,
    H2_DINOBOUNCE_ERR_INVALID_ARGUMENT = -1,
    H2_DINOBOUNCE_ERR_NO_MEMORY = -2,
    H2_DINOBOUNCE_ERR_ASSET = -3,
} h2_dinobounce_result_code_t;

typedef enum h2_dinobounce_button {
    H2_DINOBOUNCE_BUTTON_LEFT = 0,
    H2_DINOBOUNCE_BUTTON_RIGHT = 1,
    H2_DINOBOUNCE_BUTTON_ACTION = 2,
} h2_dinobounce_button_t;

typedef enum h2_dinobounce_event {
    H2_DINOBOUNCE_EVENT_BOUNCE = 0,
    H2_DINOBOUNCE_EVENT_GAME_OVER = 1,
} h2_dinobounce_event_t;

/** Synchronous observer; must not re-enter, reset, or destroy the game. */
typedef void (*h2_dinobounce_event_callback_t)(
    void *user,
    h2_dinobounce_event_t event);

typedef struct h2_dinobounce_texts {
    h2_game_text_span_t press_record;
    h2_game_text_span_t game_over;
} h2_dinobounce_texts_t;

typedef struct h2_dinobounce_config {
    const pixa_asset_t *player;
    h2_game_audio_t *audio;
    /** Borrowed synchronous text provider. */
    const h2_game_text_api_t *text;
    /** Borrowed localized catalog. */
    const h2_dinobounce_texts_t *texts;
    uint32_t random_seed;
    h2_dinobounce_event_callback_t event_callback;
    void *event_user;
} h2_dinobounce_config_t;

typedef struct h2_dinobounce_result {
    uint32_t survival_ms;
    uint8_t game_over;
} h2_dinobounce_result_t;

const h2_dinobounce_texts_t *h2_dinobounce_english_texts(void);

int h2_dinobounce_create(
    const h2_dinobounce_config_t *config,
    h2_dinobounce_t **out_game);
h2_game_scene_t *h2_dinobounce_scene(h2_dinobounce_t *game);
void h2_dinobounce_destroy(h2_dinobounce_t *game);
void h2_dinobounce_handle_input(
    h2_dinobounce_t *game,
    const h2_game_input_event_t *event);
void h2_dinobounce_reset(h2_dinobounce_t *game);
int h2_dinobounce_get_result(
    const h2_dinobounce_t *game,
    h2_dinobounce_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
