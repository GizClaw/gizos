#ifndef H2_POLYGON_BATTLE_H
#define H2_POLYGON_BATTLE_H

#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "h2_game_text.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_polygon_battle h2_polygon_battle_t;

typedef enum h2_polygon_battle_result_code {
    H2_POLYGON_BATTLE_OK = 0,
    H2_POLYGON_BATTLE_ERR_INVALID_ARGUMENT = -1,
    H2_POLYGON_BATTLE_ERR_NO_MEMORY = -2,
} h2_polygon_battle_result_code_t;

typedef enum h2_polygon_battle_button {
    H2_POLYGON_BATTLE_BUTTON_LEFT = 0,
    H2_POLYGON_BATTLE_BUTTON_RIGHT = 1,
    H2_POLYGON_BATTLE_BUTTON_ACTION = 2,
} h2_polygon_battle_button_t;

typedef enum h2_polygon_battle_shape {
    H2_POLYGON_BATTLE_SHAPE_CIRCLE = 0,
    H2_POLYGON_BATTLE_SHAPE_TRIANGLE = 3,
    H2_POLYGON_BATTLE_SHAPE_SQUARE = 4,
    H2_POLYGON_BATTLE_SHAPE_PENTAGON = 5,
    H2_POLYGON_BATTLE_SHAPE_HEXAGON = 6,
} h2_polygon_battle_shape_t;

typedef enum h2_polygon_battle_event_type {
    H2_POLYGON_BATTLE_EVENT_SHOT = 0,
    H2_POLYGON_BATTLE_EVENT_ENEMY_HIT = 1,
    H2_POLYGON_BATTLE_EVENT_ENEMY_DESTROYED = 2,
    H2_POLYGON_BATTLE_EVENT_PICKUP_COLLECTED = 3,
    H2_POLYGON_BATTLE_EVENT_PLAYER_HIT = 4,
    H2_POLYGON_BATTLE_EVENT_WAVE_STARTED = 5,
    H2_POLYGON_BATTLE_EVENT_WAVE_CLEARED = 6,
    H2_POLYGON_BATTLE_EVENT_GAME_OVER = 7,
    H2_POLYGON_BATTLE_EVENT_AUDIO_ERROR = 8,
} h2_polygon_battle_event_type_t;

typedef struct h2_polygon_battle_event {
    h2_polygon_battle_event_type_t type;
    uint16_t enemy_id;
    h2_polygon_battle_shape_t shape;
    uint8_t applied_damage;
    uint8_t remaining_hp;
    uint32_t wave;
} h2_polygon_battle_event_t;

/** Synchronous observer; must not re-enter, reset, or destroy the game. */
typedef void (*h2_polygon_battle_event_callback_t)(
    void *user,
    const h2_polygon_battle_event_t *event);

typedef struct h2_polygon_battle_texts {
    h2_game_text_span_t title;
    h2_game_text_span_t start;
    h2_game_text_span_t life;
    h2_game_text_span_t score;
    h2_game_text_span_t wave;
    h2_game_text_span_t spread;
    h2_game_text_span_t pierce;
    h2_game_text_span_t ricochet;
    h2_game_text_span_t power;
    h2_game_text_span_t game_over;
    h2_game_text_span_t destroyed;
    h2_game_text_span_t max_combo;
    h2_game_text_span_t retry;
} h2_polygon_battle_texts_t;

typedef struct h2_polygon_battle_config {
    /** Optional borrowed synthesized one-shot audio player. */
    h2_game_audio_t *audio;
    /** Borrowed synchronous text provider. */
    const h2_game_text_api_t *text;
    /** Borrowed localized catalog. */
    const h2_polygon_battle_texts_t *texts;
    uint32_t random_seed;
    h2_polygon_battle_event_callback_t event_callback;
    void *event_user;
} h2_polygon_battle_config_t;

typedef struct h2_polygon_battle_result {
    uint32_t score;
    uint32_t destroyed_count;
    uint32_t elapsed_ms;
    uint16_t max_combo;
    uint32_t wave;
    uint8_t life;
    uint8_t spread;
    uint8_t pierce;
    uint8_t ricochet;
    uint8_t power;
} h2_polygon_battle_result_t;

/** @brief Returns the built-in English text catalog. */
const h2_polygon_battle_texts_t *h2_polygon_battle_english_texts(void);

/**
 * @brief Creates a Polygon Battle instance.
 *
 * The instance borrows the audio, text provider, localized catalog, and event
 * callback configuration until h2_polygon_battle_destroy() is called.
 *
 * @param config Required borrowed configuration.
 * @param out_game Receives the owned instance on success and NULL on failure.
 * @return H2_POLYGON_BATTLE_OK or an h2_polygon_battle_result_code_t error.
 */
int h2_polygon_battle_create(
    const h2_polygon_battle_config_t *config,
    h2_polygon_battle_t **out_game);

/** @brief Returns the borrowed Game Runtime scene owned by game. */
h2_game_scene_t *h2_polygon_battle_scene(h2_polygon_battle_t *game);

/** @brief Applies one button down/up event synchronously. */
void h2_polygon_battle_handle_input(
    h2_polygon_battle_t *game,
    const h2_game_input_event_t *event);

/** @brief Clears gameplay state and returns the instance to Ready. */
void h2_polygon_battle_reset(h2_polygon_battle_t *game);

/** @brief Copies the current result snapshot into caller-provided storage. */
int h2_polygon_battle_get_result(
    const h2_polygon_battle_t *game,
    h2_polygon_battle_result_t *out_result);

/** @brief Destroys the instance without taking ownership of borrowed config. */
void h2_polygon_battle_destroy(h2_polygon_battle_t *game);

#ifdef __cplusplus
}
#endif

#endif
