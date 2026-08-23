#ifndef H2_GAME_RUNTIME_H
#define H2_GAME_RUNTIME_H

#include "h2/pal/hal/h2_pal_display.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GAME_RUNTIME_OK 0
#define H2_GAME_RUNTIME_ERR_INVALID_ARG -1
#define H2_GAME_RUNTIME_ERR_NO_MEMORY -2
#define H2_GAME_RUNTIME_ERR_DISPLAY -3
#define H2_GAME_RUNTIME_ERR_OVERFLOW -4

typedef struct h2_game_runtime h2_game_runtime_t;
typedef void h2_game_scene_t;

typedef enum h2_game_input_type {
    H2_GAME_INPUT_TOUCH_DOWN = 0,
    H2_GAME_INPUT_TOUCH_MOVE = 1,
    H2_GAME_INPUT_TOUCH_UP = 2,
    H2_GAME_INPUT_BUTTON_DOWN = 3,
    H2_GAME_INPUT_BUTTON_UP = 4,
    /** Host-recognized click gesture. Runtime preserves ordering and does not infer it. */
    H2_GAME_INPUT_BUTTON_CLICK = 5,
} h2_game_input_type_t;

typedef struct h2_game_input_event {
    h2_game_input_type_t type;
    int16_t x;
    int16_t y;
    uint8_t button;
} h2_game_input_event_t;

typedef void (*h2_game_input_handler_t)(h2_game_scene_t *scene, const h2_game_input_event_t *event);

typedef struct h2_game_runtime_config {
    const h2_pal_display_api_t *display;
    int width;
    int height;
    h2_game_scene_t *scene;
    h2_game_input_handler_t input_handler;
} h2_game_runtime_config_t;

int h2_game_runtime_create(const h2_game_runtime_config_t *config, h2_game_runtime_t **out_runtime);
int h2_game_runtime_tick(h2_game_runtime_t *runtime, uint32_t now_ms);
int h2_game_runtime_send_input(h2_game_runtime_t *runtime, const h2_game_input_event_t *event);
void h2_game_runtime_destroy(h2_game_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
