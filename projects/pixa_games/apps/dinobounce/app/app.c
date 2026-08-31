#include "h2_pixa_games_dinobounce.h"

#include "h2_dinobounce.h"
#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "pixa.h"

#include <stdint.h>

#define H2_PIXA_GAMES_DINOBOUNCE_WIDTH 240
#define H2_PIXA_GAMES_DINOBOUNCE_HEIGHT 240
#define H2_PIXA_GAMES_DINOBOUNCE_FRAME_MS 17u
#define H2_PIXA_GAMES_DINOBOUNCE_MAX_FRAME_LAG_MS 68u

typedef struct h2_pixa_games_dinobounce_state {
    h2_runtime_t *runtime;
    uint8_t *player_data;
    pixa_asset_t player_asset;
    h2_game_audio_t *audio;
    h2_dinobounce_t *game;
    h2_game_runtime_t *game_runtime;
    int display_opened;
    int speaker_started;
} h2_pixa_games_dinobounce_state_t;

static h2_dinobounce_t *s_active_game;

static int load_player(
    h2_pixa_games_dinobounce_state_t *state,
    const char *path) {
    h2_pal_fs_stat_t stat = {0};
    int rc = h2_pal_fs_stat(state->runtime->fs, path, &stat);
    if (rc != H2_PAL_OK || stat.is_dir || stat.size == 0u ||
        stat.size > SIZE_MAX) {
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_ARG;
    }

    state->player_data = h2_pal_mem_alloc(
        state->runtime->mem, (size_t)stat.size);
    if (state->player_data == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }

    h2_pal_fs_file_t *file = NULL;
    rc = h2_pal_fs_open(
        state->runtime->fs, path, H2_PAL_FS_OPEN_READ, &file);
    size_t offset = 0u;
    while (rc == H2_PAL_OK && offset < (size_t)stat.size) {
        size_t count = 0u;
        rc = h2_pal_fs_read(
            state->runtime->fs,
            file,
            state->player_data + offset,
            (size_t)stat.size - offset,
            &count);
        if (rc == H2_PAL_OK && count == 0u) {
            rc = H2_PAL_ERR_TRUNCATED;
        }
        offset += count;
    }
    if (file != NULL) {
        int close_rc = h2_pal_fs_close(state->runtime->fs, file);
        if (rc == H2_PAL_OK) {
            rc = close_rc;
        }
    }
    if (rc == H2_PAL_OK &&
        pixa_open_memory(
            state->player_data,
            (size_t)stat.size,
            &state->player_asset) != PIXA_OK) {
        rc = H2_PAL_ERR_FORMAT;
    }
    return rc;
}

static void game_input(
    h2_game_scene_t *scene,
    const h2_game_input_event_t *event) {
    (void)scene;
    if (s_active_game != NULL) {
        h2_dinobounce_handle_input(s_active_game, event);
    }
}

static int send_input(
    h2_pixa_games_dinobounce_state_t *state,
    uint8_t button,
    uint32_t timestamp_ms) {
    const h2_game_input_event_t input = {
        .type = H2_GAME_INPUT_BUTTON_CLICK,
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
    h2_pixa_games_dinobounce_state_t *state,
    const h2_runtime_event_t *event) {
    if (event->component != H2_RUNTIME_COMPONENT_BUTTON ||
        event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
        event->payload_size < sizeof(h2_runtime_button_action_event_t) ||
        !h2_runtime_button_action_is_released(event->payload)) return H2_GAME_RUNTIME_OK;
    uint8_t button = 0u;
    if (event->component_id == H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_LEFT) button = H2_DINOBOUNCE_BUTTON_LEFT;
    else if (event->component_id == H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_RIGHT) button = H2_DINOBOUNCE_BUTTON_RIGHT;
    else if (event->component_id == H2_PIXA_GAMES_DINOBOUNCE_COMPONENT_RECORD) button = H2_DINOBOUNCE_BUTTON_ACTION;
    else return H2_GAME_RUNTIME_OK;
    return send_input(state, button, (uint32_t)event->timestamp_ms);
}

static int cleanup(h2_pixa_games_dinobounce_state_t *state, int rc) {
    s_active_game = NULL;
    h2_game_runtime_destroy(state->game_runtime);
    h2_dinobounce_destroy(state->game);
    if (state->audio != NULL) {
        int audio_rc = H2_GAME_AUDIO_ERR_TASK;
        while (audio_rc == H2_GAME_AUDIO_ERR_TASK) {
            audio_rc = h2_game_audio_destroy(state->audio);
            if (audio_rc == H2_GAME_AUDIO_ERR_TASK) {
                (void)h2_pal_time_sleep_ms(state->runtime->time, 20u);
            }
        }
        if (rc == H2_PAL_OK) {
            rc = audio_rc;
        }
    }
    if (state->speaker_started) {
        int speaker_rc = h2_pal_audio_stop_speaker(state->runtime->audio);
        if (rc == H2_PAL_OK) {
            rc = speaker_rc;
        }
    }
    if (state->display_opened) {
        int display_rc = h2_pal_display_close(state->runtime->display);
        if (rc == H2_PAL_OK) {
            rc = display_rc;
        }
    }
    h2_pal_mem_free(state->runtime->mem, state->player_data);
    return rc;
}

int h2_pixa_games_dinobounce_run(
    h2_runtime_t *runtime,
    const h2_pixa_games_dinobounce_config_t *config) {
    if (runtime == NULL || config == NULL ||
        config->player_pixa_path == NULL ||
        config->player_pixa_path[0] == '\0' || runtime->display == NULL ||
        runtime->audio == NULL || runtime->fs == NULL ||
        runtime->mem == NULL || runtime->time == NULL ||
        runtime->task == NULL || runtime->queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pixa_games_dinobounce_state_t state = {
        .runtime = runtime,
    };
    int rc = load_player(&state, config->player_pixa_path);
    if (rc != H2_PAL_OK) {
        return cleanup(&state, rc);
    }

    rc = h2_pal_display_open(runtime->display);
    if (rc != H2_DISPLAY_OK) {
        return cleanup(&state, rc);
    }
    state.display_opened = 1;
    h2_display_info_t display_info = {0};
    rc = h2_pal_display_get_info(runtime->display, &display_info);
    if (rc != H2_DISPLAY_OK ||
        display_info.width != H2_PIXA_GAMES_DINOBOUNCE_WIDTH ||
        display_info.height != H2_PIXA_GAMES_DINOBOUNCE_HEIGHT) {
        return cleanup(
            &state,
            rc != H2_DISPLAY_OK ? rc : H2_DISPLAY_ERR_INVALID_ARG);
    }
    (void)h2_pal_display_set_brightness_percent(runtime->display, 100u);

    rc = h2_pal_audio_start_speaker(runtime->audio);
    if (rc != H2_AUDIO_OK) {
        return cleanup(&state, rc);
    }
    state.speaker_started = 1;
    const h2_game_audio_config_t audio_config = {
        .audio = runtime->audio,
        .task = runtime->task,
        .queue = runtime->queue,
        .mem = runtime->mem,
        .command_capacity = 8u,
    };
    rc = h2_game_audio_create(&audio_config, &state.audio);
    if (rc != H2_GAME_AUDIO_OK) {
        return cleanup(&state, rc);
    }

    const h2_game_text_api_t text = h2_game_text_builtin_5x7();
    const h2_dinobounce_config_t game_config = {
        .player = &state.player_asset,
        .audio = state.audio,
        .text = &text,
        .texts = h2_dinobounce_english_texts(),
        .random_seed = config->random_seed,
        .event_callback = NULL,
        .event_user = NULL,
    };
    rc = h2_dinobounce_create(&game_config, &state.game);
    if (rc != H2_DINOBOUNCE_OK) {
        return cleanup(&state, rc);
    }
    s_active_game = state.game;

    const h2_game_runtime_config_t game_runtime_config = {
        .display = runtime->display,
        .width = H2_PIXA_GAMES_DINOBOUNCE_WIDTH,
        .height = H2_PIXA_GAMES_DINOBOUNCE_HEIGHT,
        .scene = h2_dinobounce_scene(state.game),
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
            next_frame_ms += H2_PIXA_GAMES_DINOBOUNCE_FRAME_MS;
            if (now_ms > next_frame_ms &&
                now_ms - next_frame_ms >
                H2_PIXA_GAMES_DINOBOUNCE_MAX_FRAME_LAG_MS) {
                next_frame_ms = now_ms + H2_PIXA_GAMES_DINOBOUNCE_FRAME_MS;
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
