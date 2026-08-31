#include "board_config.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_starboy.h"

#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>

#define H2_STARBOY_HEALTH_TIMEOUT_MS 1200u
#define H2_STARBOY_HEALTH_POLL_MS 20u

typedef struct h2_starboy_entry_state {
    h2_runtime_t *runtime;
    int confirmed;
} h2_starboy_entry_state_t;

static h2_pal_result_t validate_display(h2_runtime_t *runtime) {
    h2_pal_result_t rc = (h2_pal_result_t)h2_pal_display_open(runtime->display);
    h2_display_info_t info = {0};
    if (rc == H2_PAL_OK) {
        rc = (h2_pal_result_t)h2_pal_display_get_info(runtime->display, &info);
    }
    if (rc == H2_PAL_OK && (info.width != 368 || info.height != 448)) {
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_OK) {
        rc = (h2_pal_result_t)h2_pal_display_close(runtime->display);
    } else {
        (void)h2_pal_display_close(runtime->display);
    }
    return rc;
}

static h2_pal_result_t validate_touch(h2_runtime_t *runtime) {
    h2_pal_result_t rc = h2_pal_touch_open(runtime->touch);
    h2_pal_touch_info_t info = {0};
    if (rc == H2_PAL_OK) {
        rc = h2_pal_touch_get_info(runtime->touch, &info);
    }
    if (rc == H2_PAL_OK && (info.width != 368u || info.height != 448u)) {
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_touch_close(runtime->touch);
    } else {
        (void)h2_pal_touch_close(runtime->touch);
    }
    return rc;
}

static h2_pal_result_t validate_motion(
    h2_runtime_t *runtime,
    const h2_starboy_motion_api_t *motion) {
    uint32_t elapsed_ms = 0u;
    while (elapsed_ms < H2_STARBOY_HEALTH_TIMEOUT_MS) {
        h2_starboy_motion_sample_t sample = {0};
        h2_pal_result_t rc = motion->vtable->read(
            motion->user, &sample);
        if (rc == H2_PAL_OK) {
            return H2_PAL_OK;
        }
        rc = h2_pal_time_sleep_ms(
            runtime->time, H2_STARBOY_HEALTH_POLL_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        elapsed_ms += H2_STARBOY_HEALTH_POLL_MS;
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t validate_button(
    h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id) {
    uint32_t elapsed_ms = 0u;
    while (elapsed_ms < H2_STARBOY_HEALTH_TIMEOUT_MS) {
        h2_runtime_button_state_t state = {0};
        h2_pal_result_t rc = h2_runtime_component_state_button(
            runtime, component_id, &state);
        if (rc == H2_PAL_OK && state.updated_at_ms != 0u &&
            state.result == H2_PAL_OK) {
            return H2_PAL_OK;
        }
        rc = h2_pal_time_sleep_ms(
            runtime->time, H2_STARBOY_HEALTH_POLL_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        elapsed_ms += H2_STARBOY_HEALTH_POLL_MS;
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t validate_power(h2_runtime_t *runtime) {
    h2_pal_power_capabilities_t capabilities = {0};
    h2_pal_result_t rc = h2_pal_power_get_capabilities(
        runtime->power, &capabilities);
    if (rc == H2_PAL_OK &&
        (capabilities.flags & H2_PAL_POWER_CAPABILITY_SHUTDOWN) == 0u) {
        rc = H2_PAL_ERR_UNAVAILABLE;
    }
    return rc;
}

static h2_pal_result_t validate_microphone(h2_runtime_t *runtime) {
    h2_audio_info_t info = {0};
    int rc = h2_pal_audio_get_info(runtime->audio, &info);
    if (rc != H2_AUDIO_OK || !info.available || !info.mic_supported ||
        info.mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        info.mic_format.channels == 0u ||
        info.mic_format.frame_samples_per_channel == 0u) {
        return rc == H2_AUDIO_OK
            ? H2_PAL_ERR_UNAVAILABLE
            : (h2_pal_result_t)rc;
    }

    if (info.mic_format.frame_samples_per_channel >
        SIZE_MAX / info.mic_format.channels) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t sample_count =
        (size_t)info.mic_format.channels *
        info.mic_format.frame_samples_per_channel;
    if (sample_count > SIZE_MAX / sizeof(int16_t)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t buffer_bytes = sample_count * sizeof(int16_t);
    int16_t *samples =
        (int16_t *)h2_pal_mem_alloc(runtime->mem, buffer_bytes);
    if (samples == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }

    rc = h2_pal_audio_start_mic(runtime->audio);
    const int mic_started = rc == H2_AUDIO_OK;
    uint32_t elapsed_ms = 0u;
    while (rc == H2_AUDIO_OK && elapsed_ms < H2_STARBOY_HEALTH_TIMEOUT_MS) {
        h2_audio_frame_t frame = h2_audio_frame_for_buffer(
            samples, buffer_bytes, info.mic_format);
        rc = h2_pal_audio_mic_read(runtime->audio, &frame, 0u);
        if (rc == H2_AUDIO_OK) {
            if (frame.bytes > 0u && frame.bytes <= buffer_bytes) {
                break;
            }
            if (frame.bytes > buffer_bytes) {
                rc = H2_PAL_ERR_INVALID_STATE;
                break;
            }
            rc = H2_AUDIO_ERR_WOULD_BLOCK;
        }
        if (rc != H2_AUDIO_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) {
            break;
        }
        rc = (int)h2_pal_time_sleep_ms(
            runtime->time, H2_STARBOY_HEALTH_POLL_MS);
        elapsed_ms += H2_STARBOY_HEALTH_POLL_MS;
    }
    if (rc == H2_AUDIO_OK && elapsed_ms >= H2_STARBOY_HEALTH_TIMEOUT_MS) {
        rc = H2_PAL_ERR_TIMEOUT;
    }
    if (mic_started &&
        h2_pal_audio_stop_mic(runtime->audio) != H2_AUDIO_OK &&
        rc == H2_AUDIO_OK) {
        rc = H2_PAL_ERR_IO;
    }
    h2_pal_mem_free(runtime->mem, samples);
    return (h2_pal_result_t)rc;
}

static h2_pal_result_t validate_hardware(
    h2_runtime_t *runtime,
    const h2_starboy_motion_api_t *motion) {
    h2_pal_result_t rc = validate_display(runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_FAIL stage=display rc=%d\n", (int)rc);
        return rc;
    }
    rc = validate_touch(runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_DEGRADED capability=touch rc=%d\n", (int)rc);
    }
    rc = validate_motion(runtime, motion);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_DEGRADED capability=imu rc=%d\n", (int)rc);
    }
    rc = validate_button(runtime, H2_STARBOY_COMPONENT_THEME_BUTTON);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_FAIL stage=boot_button rc=%d\n", (int)rc);
        return rc;
    }
    rc = validate_button(runtime, H2_STARBOY_COMPONENT_POWER_BUTTON);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_FAIL stage=power_button rc=%d\n", (int)rc);
        return rc;
    }
    rc = validate_power(runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_FAIL stage=power rc=%d\n", (int)rc);
        return rc;
    }
    rc = validate_microphone(runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_DEGRADED capability=microphone rc=%d\n", (int)rc);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t confirm_healthy(void *user) {
    h2_starboy_entry_state_t *state = (h2_starboy_entry_state_t *)user;
    if (state == NULL || state->runtime == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_esp_platform_confirm_running_app();
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_confirm(state->runtime);
    }
    if (rc == H2_PAL_OK) {
        state->confirmed = 1;
        printf(
            "H2_STARBOY_READY touch=ft3168@0x38 "
            "imu=qmi8658@0x6b,0x6a orientation=gravity "
            "microphone=es8311 theme_button=boot "
            "palette=random-high-contrast pupil_styles=4 "
            "power_button=pwr:2s pmic=axp2101@0x34\n");
    }
    return rc;
}

static void wait_for_recovery(h2_pal_result_t rc) {
    printf("H2_STARBOY_FAIL stage=run rc=%d recovery=app_command\n", (int)rc);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

static void starboy_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = {0};
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_starboy_amoled_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &runtime_config, "starboy", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&runtime_config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(
            runtime, "starboy", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        /* Hardware validation reads Button state, so input must be running. */
        h2_runtime_input_poll_config_t input_poll = {0};
        rc = h2_starboy_amoled_input_poll_config(&input_poll);
        if (rc == H2_PAL_OK) {
            rc = h2_runtime_input_start(runtime, &input_poll);
        }
    }
    const h2_starboy_motion_api_t motion =
        h2_starboy_amoled_motion_api(runtime);
    if (rc == H2_PAL_OK) {
        rc = validate_hardware(runtime, &motion);
    }
    if (rc != H2_PAL_OK) {
        printf("H2_STARBOY_FAIL stage=healthy rc=%d\n", (int)rc);
        esp_restart();
    }

    h2_starboy_entry_state_t entry_state = {
        .runtime = runtime,
    };
    const h2_starboy_config_t app_config = {
        .frame_interval_ms = 16u,
        .random_seed = esp_random(),
        .motion = &motion,
        .enable_theme_button = 1,
        .enable_power_button = 1,
        .initial_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN,
        .ready = confirm_healthy,
        .ready_user = &entry_state,
    };
    rc = (h2_pal_result_t)h2_starboy_run(runtime, &app_config);
    if (!entry_state.confirmed) {
        printf("H2_STARBOY_FAIL stage=pre_confirm rc=%d\n", (int)rc);
        esp_restart();
    }
    wait_for_recovery(rc);
}

void app_main(void) {
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "amoled/starboy", starboy_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=amoled image=starboy code=%d\n",
            (int)rc);
    }
}
