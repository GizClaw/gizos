#ifndef H2_DINORUN_H
#define H2_DINORUN_H

#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "h2_game_text.h"
#include "pixa.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_DINORUN_OK 0
#define H2_DINORUN_ERR_INVALID_ARG -1
#define H2_DINORUN_ERR_NO_MEMORY -2
#define H2_DINORUN_ERR_ASSET -3

typedef struct h2_dinorun h2_dinorun_t;

typedef enum h2_dinorun_button {
  H2_DINORUN_BUTTON_ACTION = 1,
} h2_dinorun_button_t;

typedef enum h2_dinorun_event {
  H2_DINORUN_EVENT_JUMP = 1,
  H2_DINORUN_EVENT_GAME_OVER = 2,
} h2_dinorun_event_t;

typedef void (*h2_dinorun_event_callback_t)(void *user,
                                            h2_dinorun_event_t event);

typedef struct h2_dinorun_texts {
  h2_game_text_span_t press_record;
  h2_game_text_span_t game_over;
  h2_game_text_span_t distance_unit;
} h2_dinorun_texts_t;

typedef struct h2_dinorun_player_clips {
  const char *run_right;
  const char *jump;
  const char *game_over;
} h2_dinorun_player_clips_t;

typedef struct h2_dinorun_config {
  /** Borrowed; must remain valid until h2_dinorun_destroy(). */
  const pixa_asset_t *player;
  /** Borrowed and already started; may be NULL to disable sound. */
  h2_game_audio_t *audio;
  /** Borrowed synchronous text provider. */
  const h2_game_text_api_t *text;
  /** Borrowed localized catalog. */
  const h2_dinorun_texts_t *texts;
  uint32_t seed;
  /** Synchronous observer; must not re-enter or destroy the game. */
  h2_dinorun_event_callback_t event_callback;
  void *event_user;
  /**
   * Optional borrowed semantic clip mapping; the mapping and strings must
   * remain valid until h2_dinorun_destroy(). NULL uses canonical clip IDs.
   */
  const h2_dinorun_player_clips_t *player_clips;
} h2_dinorun_config_t;

typedef struct h2_dinorun_result {
  int32_t distance;
  int game_over;
} h2_dinorun_result_t;

const h2_dinorun_texts_t *h2_dinorun_english_texts(void);

int h2_dinorun_create(const h2_dinorun_config_t *config,
                      h2_dinorun_t **out_game);
h2_game_scene_t *h2_dinorun_scene(h2_dinorun_t *game);
/** Consumes ACTION DOWN/UP. DOWN charges; UP jumps or shortens ascent. */
void h2_dinorun_handle_input(h2_dinorun_t *game,
                             const h2_game_input_event_t *event);
void h2_dinorun_reset(h2_dinorun_t *game);
/**
 * Copies an optional host-owned status into the game-over overlay.
 * An empty span clears the status. The text must be valid UTF-8 and no longer
 * than 63 bytes. Call only from the thread that owns and ticks the game.
 */
int h2_dinorun_set_game_over_status(h2_dinorun_t *game,
                                    h2_game_text_span_t status);
int h2_dinorun_get_result(const h2_dinorun_t *game,
                          h2_dinorun_result_t *out_result);
void h2_dinorun_destroy(h2_dinorun_t *game);

#ifdef __cplusplus
}
#endif

#endif
