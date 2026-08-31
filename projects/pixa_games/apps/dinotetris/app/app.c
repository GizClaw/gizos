#include "h2_pixa_games_dinotetris.h"

#include "h2_dinotetris.h"
#include "h2_game_runtime.h"

#include <stdint.h>

#define H2_PIXA_GAMES_DINOTETRIS_WIDTH 240
#define H2_PIXA_GAMES_DINOTETRIS_HEIGHT 240
#define H2_PIXA_GAMES_DINOTETRIS_FRAME_MS 17u
#define H2_PIXA_GAMES_DINOTETRIS_MAX_FRAME_LAG_MS 68u

typedef struct h2_pixa_games_dinotetris_state {
    h2_runtime_t *runtime;
    h2_dinotetris_t *game;
    h2_game_runtime_t *game_runtime;
    int display_opened;
} h2_pixa_games_dinotetris_state_t;

static h2_dinotetris_t *s_active_game;

static void game_input(
    h2_game_scene_t *scene,
    const h2_game_input_event_t *event) {
    (void)scene;
    if (s_active_game != NULL) {
        h2_dinotetris_handle_input(s_active_game, event);
    }
}

static int send_input(
    h2_pixa_games_dinotetris_state_t *state,
    h2_game_input_type_t type,
    uint8_t button,
    uint32_t timestamp_ms) {
    const h2_game_input_event_t input = {
        .type = type,
        .x = 0,
        .y = 0,
        .button = button,
    };
    int rc = h2_game_runtime_send_input(state->game_runtime, &input);
    if (rc == H2_GAME_RUNTIME_ERR_OVERFLOW) {
        rc = h2_game_runtime_tick(state->game_runtime, timestamp_ms);
        if (rc == H2_GAME_RUNTIME_OK) rc = h2_game_runtime_send_input(state->game_runtime, &input);
    }
    return rc;
}

static int handle_runtime_event(
    h2_pixa_games_dinotetris_state_t *state,
    const h2_runtime_event_t *event) {
    if (event->component != H2_RUNTIME_COMPONENT_BUTTON) {
        return H2_GAME_RUNTIME_OK;
    }
    if (event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
        event->payload_size < sizeof(h2_runtime_button_action_event_t)) {
        return H2_GAME_RUNTIME_OK;
    }
    const h2_runtime_button_action_event_t *action = event->payload;
    h2_game_input_type_t type;
    if (h2_runtime_button_action_is_pressed(action)) {
        type = H2_GAME_INPUT_BUTTON_DOWN;
    } else if (h2_runtime_button_action_is_released(action)) {
        type = H2_GAME_INPUT_BUTTON_UP;
    } else {
        return H2_GAME_RUNTIME_OK;
    }
    uint8_t button = 0u;
    if (event->component_id == H2_PIXA_GAMES_DINOTETRIS_COMPONENT_LEFT) button = H2_DINOTETRIS_BUTTON_LEFT;
    else if (event->component_id == H2_PIXA_GAMES_DINOTETRIS_COMPONENT_RIGHT) button = H2_DINOTETRIS_BUTTON_RIGHT;
    else if (event->component_id == H2_PIXA_GAMES_DINOTETRIS_COMPONENT_RECORD) button = H2_DINOTETRIS_BUTTON_ACTION;
    else return H2_GAME_RUNTIME_OK;
    return send_input(
        state, type, button, (uint32_t)event->timestamp_ms);
}

static int cleanup(h2_pixa_games_dinotetris_state_t *state, int rc) {
    s_active_game = NULL;
    h2_game_runtime_destroy(state->game_runtime);
    h2_dinotetris_destroy(state->game);
    if (state->display_opened) {
        int display_rc = h2_pal_display_close(state->runtime->display);
        if (rc == H2_PAL_OK) rc = display_rc;
    }
    return rc;
}

int h2_pixa_games_dinotetris_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinotetris_config_t *config) {
    if (runtime == NULL || config == NULL ||
        runtime->display == NULL || runtime->time == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pixa_games_dinotetris_state_t state = {
        .runtime = runtime,
    };
    int rc = H2_PAL_OK;

    rc = h2_pal_display_open(runtime->display);
    if (rc != H2_DISPLAY_OK) {
        return cleanup(&state, rc);
    }
    state.display_opened = 1;
    h2_display_info_t display_info = {0};
    rc = h2_pal_display_get_info(runtime->display, &display_info);
    if (rc != H2_DISPLAY_OK ||
        display_info.width != H2_PIXA_GAMES_DINOTETRIS_WIDTH ||
        display_info.height != H2_PIXA_GAMES_DINOTETRIS_HEIGHT) {
        return cleanup(
            &state,
            rc != H2_DISPLAY_OK ? rc : H2_DISPLAY_ERR_INVALID_ARG);
    }
    (void)h2_pal_display_set_brightness_percent(runtime->display, 100u);

    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    const h2_dinotetris_config_t game_config = {
        .text = &text,
        .texts = h2_dinotetris_english_texts(),
        .random_seed = config->random_seed,
        .event_callback = NULL,
        .event_user = NULL,
    };
    rc = h2_dinotetris_create(&game_config, &state.game);
    if (rc != H2_DINOTETRIS_OK) {
        return cleanup(&state, rc);
    }
    s_active_game = state.game;

    const h2_game_runtime_config_t game_runtime_config = {
        .display = runtime->display,
        .width = H2_PIXA_GAMES_DINOTETRIS_WIDTH,
        .height = H2_PIXA_GAMES_DINOTETRIS_HEIGHT,
        .scene = h2_dinotetris_scene(state.game),
        .input_handler = game_input,
    };
    rc = h2_game_runtime_create(&game_runtime_config, &state.game_runtime);
    if (rc != H2_GAME_RUNTIME_OK) {
        return cleanup(&state, rc);
    }

    uint64_t now_ms = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (rc != H2_PAL_OK) {
        return cleanup(&state, rc);
    }
    uint64_t next_frame_ms = now_ms;
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    for (;;) {
        rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
        if (rc != H2_PAL_OK) {
            return cleanup(&state, rc);
        }
        if (now_ms >= next_frame_ms) {
            rc = h2_game_runtime_tick(state.game_runtime, (uint32_t)now_ms);
            if (rc != H2_GAME_RUNTIME_OK) {
                return cleanup(&state, rc);
            }
            rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
            if (rc != H2_PAL_OK) {
                return cleanup(&state, rc);
            }
            next_frame_ms += H2_PIXA_GAMES_DINOTETRIS_FRAME_MS;
            if (now_ms > next_frame_ms &&
                now_ms - next_frame_ms >
                H2_PIXA_GAMES_DINOTETRIS_MAX_FRAME_LAG_MS) {
                next_frame_ms = now_ms + H2_PIXA_GAMES_DINOTETRIS_FRAME_MS;
            }
        }

        h2_runtime_event_t event = {
            .payload = payload,
            .payload_capacity = sizeof(payload),
        };
        uint64_t wait_ms = next_frame_ms > now_ms
                               ? next_frame_ms - now_ms
                               : 0u;
        uint32_t timeout_ms = wait_ms > UINT32_MAX
                                  ? UINT32_MAX
                                  : (uint32_t)wait_ms;
        rc = h2_runtime_wait_event(runtime, &event, timeout_ms);
        if (rc == H2_PAL_ERR_TIMEOUT) {
            continue;
        }
        if (rc != H2_PAL_OK) {
            return cleanup(&state, rc);
        }
        rc = handle_runtime_event(&state, &event);
        if (rc != H2_GAME_RUNTIME_OK) {
            return cleanup(&state, rc);
        }
    }
}
