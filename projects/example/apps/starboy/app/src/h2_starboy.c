#include "h2_starboy.h"

#include "h2_starboy_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define H2_STARBOY_DEFAULT_FRAME_INTERVAL_MS 16u
#define H2_STARBOY_MAX_TOUCH_EVENTS_PER_FRAME 16u
#define H2_STARBOY_PERF_INTERVAL_MS 1000u
#define H2_STARBOY_TOUCH_RETRY_MS 1000u
#define H2_STARBOY_RAW_SHAKE_THRESHOLD_MG 1500
#define H2_STARBOY_RAW_SHAKE_SAMPLE_MS 100u
#define H2_STARBOY_MAX_AUDIO_FRAMES_PER_POLL 4u
#define H2_STARBOY_AUDIO_STALE_MS 96u
#define H2_STARBOY_AUDIO_RELEASE_STEP_MS 32u

typedef struct h2_starboy_perf {
    uint64_t window_started_ms;
    uint64_t input_total_ms;
    uint64_t render_total_ms;
    uint64_t draw_total_ms;
    uint64_t present_total_ms;
    uint64_t frame_total_ms;
    uint64_t dirty_pixels_total;
    uint32_t frame_count;
    uint32_t overrun_count;
    uint32_t frame_max_ms;
    uint32_t draw_max_ms;
    uint32_t motion_success_count;
    uint32_t motion_error_count;
    h2_pal_result_t last_motion_result;
} h2_starboy_perf_t;

typedef struct h2_starboy_app {
    h2_runtime_t *runtime;
    h2_starboy_config_t config;
    h2_starboy_behavior_t behavior;
    h2_starboy_shake_tracker_t shake_tracker;
    h2_starboy_power_hold_tracker_t power_hold_tracker;
    h2_pal_touch_info_t touch_info;
    h2_audio_info_t audio_info;
    uint16_t *framebuffer;
    int16_t *mic_samples;
    size_t framebuffer_bytes;
    size_t mic_bytes;
    uint64_t last_imu_update_ms;
    uint64_t last_audio_frame_ms;
    uint64_t last_audio_release_ms;
    uint64_t raw_shake_started_ms;
    uint64_t next_touch_retry_ms;
    uint32_t audio_level;
    uint32_t touch_recovery_count;
    uint8_t event_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_starboy_render_regions_t previous_render_regions;
    h2_starboy_perf_t perf;
    int display_opened;
    int touch_opened;
    int mic_started;
    int pointer_active;
    int previous_content_valid;
    int shutdown_pending;
    int raw_shake_active;
    int raw_shake_reported;
    int32_t pointer_x_q15;
    int32_t pointer_y_q15;
} h2_starboy_app_t;

static uint32_t starboy_duration_ms(uint64_t started_ms, uint64_t ended_ms) {
    if (ended_ms <= started_ms) {
        return 0u;
    }
    const uint64_t duration_ms = ended_ms - started_ms;
    return duration_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_ms;
}

static void starboy_record_performance(
    h2_starboy_app_t *app,
    const h2_display_info_t *display_info,
    const h2_starboy_render_regions_t *render_regions,
    uint64_t frame_started_ms,
    uint64_t input_finished_ms,
    uint64_t render_finished_ms,
    uint64_t draw_finished_ms,
    uint64_t frame_finished_ms) {
    h2_starboy_perf_t *perf = &app->perf;
    if (perf->window_started_ms == 0u) {
        perf->window_started_ms = frame_started_ms;
    }

    const uint32_t input_ms =
        starboy_duration_ms(frame_started_ms, input_finished_ms);
    const uint32_t render_ms =
        starboy_duration_ms(input_finished_ms, render_finished_ms);
    const uint32_t draw_ms =
        starboy_duration_ms(render_finished_ms, draw_finished_ms);
    const uint32_t present_ms =
        starboy_duration_ms(draw_finished_ms, frame_finished_ms);
    const uint32_t frame_ms =
        starboy_duration_ms(frame_started_ms, frame_finished_ms);
    perf->input_total_ms += input_ms;
    perf->render_total_ms += render_ms;
    perf->draw_total_ms += draw_ms;
    perf->present_total_ms += present_ms;
    perf->frame_total_ms += frame_ms;
    uint64_t dirty_pixels = 0u;
    for (size_t index = 0u;
         index < render_regions->dirty_rect_count;
         ++index) {
        dirty_pixels +=
            (uint64_t)render_regions->dirty_rects[index].width *
            (uint64_t)render_regions->dirty_rects[index].height;
    }
    perf->dirty_pixels_total += dirty_pixels;
    ++perf->frame_count;
    if (frame_ms >= app->config.frame_interval_ms) {
        ++perf->overrun_count;
    }
    if (frame_ms > perf->frame_max_ms) {
        perf->frame_max_ms = frame_ms;
    }
    if (draw_ms > perf->draw_max_ms) {
        perf->draw_max_ms = draw_ms;
    }

    const uint64_t window_ms = frame_finished_ms >= perf->window_started_ms
        ? frame_finished_ms - perf->window_started_ms
        : 0u;
    if (window_ms < H2_STARBOY_PERF_INTERVAL_MS) {
        return;
    }

    const uint64_t frames = perf->frame_count;
    const uint64_t fps_x10 = frames * UINT64_C(10000) / window_ms;
    const uint64_t full_frame_pixels =
        (uint64_t)display_info->width * (uint64_t)display_info->height;
    const uint64_t dirty_percent_x10 = full_frame_pixels == 0u
        ? 0u
        : perf->dirty_pixels_total * UINT64_C(1000) /
              (full_frame_pixels * frames);
    printf(
        "H2_STARBOY_PERF window_ms=%" PRIu64
        " frames=%" PRIu32 " fps=%" PRIu64 ".%" PRIu64
        " overruns=%" PRIu32 " frame_ms=%" PRIu64 "/%" PRIu32
        " input_ms=%" PRIu64 " render_ms=%" PRIu64
        " draw_ms=%" PRIu64 "/%" PRIu32 " present_ms=%" PRIu64
        " dirty_pct=%" PRIu64 ".%" PRIu64
        " regions=%zu content=%dx%d audio=%" PRIu32 " emotion=%d"
        " touch=%d touch_recoveries=%" PRIu32
        " motion=%" PRIu32 "/%" PRIu32 " motion_rc=%d"
        " down=%" PRId32 ",%" PRId32 "\n",
        window_ms,
        perf->frame_count,
        fps_x10 / 10u,
        fps_x10 % 10u,
        perf->overrun_count,
        perf->frame_total_ms / frames,
        perf->frame_max_ms,
        perf->input_total_ms / frames,
        perf->render_total_ms / frames,
        perf->draw_total_ms / frames,
        perf->draw_max_ms,
        perf->present_total_ms / frames,
        dirty_percent_x10 / 10u,
        dirty_percent_x10 % 10u,
        render_regions->dirty_rect_count,
        render_regions->content_rect.width,
        render_regions->content_rect.height,
        app->audio_level,
        (int)app->behavior.emotion,
        app->touch_opened,
        app->touch_recovery_count,
        perf->motion_success_count,
        perf->motion_error_count,
        (int)perf->last_motion_result,
        app->behavior.gravity_down_x_q15,
        app->behavior.gravity_down_y_q15);
    *perf = (h2_starboy_perf_t){
        .window_started_ms = frame_finished_ms,
    };
}

static int starboy_should_stop(const h2_starboy_app_t *app) {
    return app->config.should_stop != NULL &&
           app->config.should_stop(app->config.should_stop_user) != 0;
}

static void starboy_cleanup(h2_starboy_app_t *app) {
    if (app->mic_started) {
        (void)h2_pal_audio_stop_mic(app->runtime->audio);
        app->mic_started = 0;
    }
    if (app->mic_samples != NULL) {
        h2_pal_mem_free(app->runtime->mem, app->mic_samples);
        app->mic_samples = NULL;
    }
    if (app->touch_opened) {
        (void)h2_pal_touch_close(app->runtime->touch);
        app->touch_opened = 0;
    }
    if (app->framebuffer != NULL) {
        h2_pal_mem_free(app->runtime->mem, app->framebuffer);
        app->framebuffer = NULL;
    }
    if (app->display_opened) {
        (void)h2_pal_display_close(app->runtime->display);
        app->display_opened = 0;
    }
}

static h2_pal_result_t starboy_try_open_touch(h2_starboy_app_t *app) {
    h2_pal_result_t result = h2_pal_touch_open(app->runtime->touch);
    if (result != H2_PAL_OK) {
        return result;
    }
    app->touch_opened = 1;
    result = h2_pal_touch_get_info(
        app->runtime->touch, &app->touch_info);
    if (result != H2_PAL_OK ||
        app->touch_info.width == 0u || app->touch_info.height == 0u) {
        (void)h2_pal_touch_close(app->runtime->touch);
        app->touch_opened = 0;
        app->touch_info = (h2_pal_touch_info_t){0};
        return result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result;
    }
    return H2_PAL_OK;
}

static int starboy_touch_result_is_retryable(h2_pal_result_t result) {
    return result == H2_PAL_ERR_IO || result == H2_PAL_ERR_TIMEOUT ||
        result == H2_PAL_ERR_UNAVAILABLE ||
        result == H2_PAL_ERR_INVALID_STATE;
}

static void starboy_try_start_microphone(h2_starboy_app_t *app) {
    if (h2_pal_audio_get_info(app->runtime->audio, &app->audio_info) !=
            H2_AUDIO_OK ||
        !app->audio_info.available || !app->audio_info.mic_supported ||
        app->audio_info.mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        app->audio_info.mic_format.channels == 0u ||
        app->audio_info.mic_format.frame_samples_per_channel == 0u) {
        app->audio_info = (h2_audio_info_t){0};
        return;
    }

    const size_t samples =
        (size_t)app->audio_info.mic_format.frame_samples_per_channel;
    if (samples > SIZE_MAX / app->audio_info.mic_format.channels) {
        app->audio_info = (h2_audio_info_t){0};
        return;
    }
    const size_t interleaved_samples =
        samples * app->audio_info.mic_format.channels;
    if (interleaved_samples > SIZE_MAX / sizeof(*app->mic_samples)) {
        app->audio_info = (h2_audio_info_t){0};
        return;
    }
    app->mic_bytes = interleaved_samples * sizeof(*app->mic_samples);
    app->mic_samples =
        (int16_t *)h2_pal_mem_alloc(app->runtime->mem, app->mic_bytes);
    if (app->mic_samples == NULL) {
        app->audio_info = (h2_audio_info_t){0};
        app->mic_bytes = 0u;
        return;
    }
    if (h2_pal_audio_start_mic(app->runtime->audio) != H2_AUDIO_OK) {
        h2_pal_mem_free(app->runtime->mem, app->mic_samples);
        app->mic_samples = NULL;
        app->mic_bytes = 0u;
        app->audio_info = (h2_audio_info_t){0};
        return;
    }
    app->mic_started = 1;
}

static void starboy_poll_touch(h2_starboy_app_t *app, uint64_t now_ms) {
    if (!app->touch_opened) {
        if (app->next_touch_retry_ms == 0u ||
            now_ms < app->next_touch_retry_ms) {
            return;
        }
        const h2_pal_result_t open_result = starboy_try_open_touch(app);
        if (open_result != H2_PAL_OK) {
            app->next_touch_retry_ms = starboy_touch_result_is_retryable(
                open_result)
                ? now_ms + H2_STARBOY_TOUCH_RETRY_MS
                : 0u;
            return;
        }
        app->next_touch_retry_ms = 0u;
        ++app->touch_recovery_count;
        printf(
            "H2_STARBOY_TOUCH state=recovered count=%" PRIu32 "\n",
            app->touch_recovery_count);
    }
    for (uint32_t index = 0u;
         index < H2_STARBOY_MAX_TOUCH_EVENTS_PER_FRAME;
         ++index) {
        h2_pal_touch_event_t event = {0};
        const h2_pal_result_t result =
            h2_pal_touch_poll_event(app->runtime->touch, &event);
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            return;
        }
        if (result != H2_PAL_OK) {
            (void)h2_pal_touch_close(app->runtime->touch);
            app->touch_opened = 0;
            app->pointer_active = 0;
            app->touch_info = (h2_pal_touch_info_t){0};
            app->next_touch_retry_ms = starboy_touch_result_is_retryable(
                result)
                ? now_ms + H2_STARBOY_TOUCH_RETRY_MS
                : 0u;
            printf(
                "H2_STARBOY_TOUCH state=retry rc=%d after_ms=%u\n",
                (int)result,
                app->next_touch_retry_ms == 0u
                    ? 0u
                    : H2_STARBOY_TOUCH_RETRY_MS);
            return;
        }
        if (event.kind == H2_PAL_TOUCH_EVENT_UP) {
            app->pointer_active = 0;
            continue;
        }
        if (event.kind != H2_PAL_TOUCH_EVENT_DOWN &&
            event.kind != H2_PAL_TOUCH_EVENT_MOVE) {
            continue;
        }
        app->pointer_active = 1;
        app->pointer_x_q15 = h2_starboy_normalize_coordinate(
            event.x, app->touch_info.width);
        app->pointer_y_q15 = h2_starboy_normalize_coordinate(
            event.y, app->touch_info.height);
    }
}

static void starboy_poll_microphone(
    h2_starboy_app_t *app,
    uint64_t now_ms) {
    if (!app->mic_started) {
        return;
    }
    int frame_received = 0;
    for (uint32_t frame_index = 0u;
         frame_index < H2_STARBOY_MAX_AUDIO_FRAMES_PER_POLL;
         ++frame_index) {
        h2_audio_frame_t frame = h2_audio_frame_for_buffer(
            app->mic_samples,
            app->mic_bytes,
            app->audio_info.mic_format);
        const int result = h2_pal_audio_mic_read(
            app->runtime->audio, &frame, 0u);
        if (result == H2_AUDIO_ERR_WOULD_BLOCK ||
            result == H2_PAL_ERR_TIMEOUT) {
            break;
        }
        if (result != H2_AUDIO_OK) {
            (void)h2_pal_audio_stop_mic(app->runtime->audio);
            app->mic_started = 0;
            app->audio_level = 0u;
            return;
        }
        if (frame.bytes == 0u || frame.bytes > app->mic_bytes ||
            frame.bytes % sizeof(*app->mic_samples) != 0u) {
            continue;
        }
        const uint32_t level = h2_starboy_audio_level_s16(
            app->mic_samples, frame.bytes / sizeof(*app->mic_samples));
        app->audio_level = h2_starboy_audio_envelope_step(
            app->audio_level, level, 1);
        frame_received = 1;
    }
    if (frame_received) {
        app->last_audio_frame_ms = now_ms;
        app->last_audio_release_ms = now_ms;
        return;
    }
    if (app->last_audio_frame_ms != 0u &&
        now_ms >= app->last_audio_frame_ms + H2_STARBOY_AUDIO_STALE_MS &&
        now_ms >= app->last_audio_release_ms +
            H2_STARBOY_AUDIO_RELEASE_STEP_MS) {
        app->audio_level = h2_starboy_audio_envelope_step(
            app->audio_level, 0u, 1);
        app->last_audio_release_ms = now_ms;
    }
}

static void starboy_poll_imu(
    h2_starboy_app_t *app,
    h2_starboy_behavior_input_t *input) {
    if (!app->config.enable_imu_component) {
        return;
    }
    h2_runtime_imu_state_t state = {0};
    if (h2_runtime_component_state_imu(
            app->runtime, H2_STARBOY_COMPONENT_IMU, &state) != H2_PAL_OK ||
        state.result != H2_PAL_OK ||
        state.updated_at_ms == app->last_imu_update_ms) {
        return;
    }
    app->last_imu_update_ms = state.updated_at_ms;
    uint32_t streak_duration_ms = 0u;
    if (state.gesture_kind == H2_RUNTIME_IMU_GESTURE_SHAKE &&
        h2_starboy_shake_tracker_accept(
            &app->shake_tracker,
            state.updated_at_ms,
            state.gesture.shake.duration_ms,
            &streak_duration_ms)) {
        input->shake_valid = 1;
        input->shake_magnitude_mg = state.gesture.shake.magnitude_mg;
        input->shake_duration_ms = streak_duration_ms;
    }
}

static int32_t starboy_abs_i32(int32_t value) {
    return value < 0 ? -value : value;
}

static void starboy_poll_motion(
    h2_starboy_app_t *app,
    h2_starboy_behavior_input_t *input) {
    const h2_starboy_motion_api_t *motion = app->config.motion;
    if (motion == NULL) {
        starboy_poll_imu(app, input);
        return;
    }
    h2_starboy_motion_sample_t sample = {0};
    const h2_pal_result_t rc = motion->vtable->read(
        motion->user, &sample);
    app->perf.last_motion_result = rc;
    if (rc != H2_PAL_OK) {
        ++app->perf.motion_error_count;
        return;
    }
    ++app->perf.motion_success_count;
    input->gravity_valid = 1;
    input->gravity_x_mg = sample.accel_mg[0];
    input->gravity_y_mg = sample.accel_mg[1];
    input->gravity_z_mg = sample.accel_mg[2];

    int32_t magnitude_mg = starboy_abs_i32(sample.accel_mg[0]);
    const int32_t abs_y = starboy_abs_i32(sample.accel_mg[1]);
    const int32_t abs_z = starboy_abs_i32(sample.accel_mg[2]);
    if (abs_y > magnitude_mg) {
        magnitude_mg = abs_y;
    }
    if (abs_z > magnitude_mg) {
        magnitude_mg = abs_z;
    }
    if (magnitude_mg < H2_STARBOY_RAW_SHAKE_THRESHOLD_MG) {
        app->raw_shake_active = 0;
        app->raw_shake_reported = 0;
        app->raw_shake_started_ms = 0u;
        return;
    }
    if (!app->raw_shake_active) {
        app->raw_shake_active = 1;
        app->raw_shake_started_ms = input->now_ms;
        return;
    }
    if (input->now_ms < app->raw_shake_started_ms ||
        input->now_ms - app->raw_shake_started_ms <
            H2_STARBOY_RAW_SHAKE_SAMPLE_MS) {
        return;
    }
    if (app->raw_shake_reported) {
        return;
    }
    uint32_t streak_duration_ms = 0u;
    if (h2_starboy_shake_tracker_accept(
            &app->shake_tracker,
            input->now_ms,
            H2_STARBOY_RAW_SHAKE_SAMPLE_MS,
            &streak_duration_ms)) {
        input->shake_valid = 1;
        input->shake_magnitude_mg = magnitude_mg;
        input->shake_duration_ms = streak_duration_ms;
        app->raw_shake_reported = 1;
    }
    app->raw_shake_started_ms = input->now_ms;
}

static h2_pal_result_t starboy_poll_button_state(
    h2_starboy_app_t *app,
    h2_runtime_component_id_t component_id,
    h2_runtime_button_state_t *out_state) {
    if (component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    h2_runtime_button_state_t state = {0};
    h2_pal_result_t rc = h2_runtime_component_state_button(
        app->runtime, component_id, &state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (state.result != H2_PAL_OK) {
        return state.result;
    }
    if (state.updated_at_ms == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    *out_state = state;
    return H2_PAL_OK;
}

static void starboy_poll_buttons(
    h2_starboy_app_t *app,
    uint64_t now_ms) {
    for (size_t index = 0u; index < 16u; ++index) {
        h2_runtime_event_t event = {
            .payload = app->event_payload,
            .payload_capacity = sizeof(app->event_payload),
        };
        const h2_pal_result_t rc =
            h2_runtime_poll_event(app->runtime, &event);
        if (rc == H2_PAL_ERR_WOULD_BLOCK) {
            break;
        }
        if (rc != H2_PAL_OK) {
            printf("H2_STARBOY_BUTTON event_poll rc=%d\n", (int)rc);
            break;
        }
        if (app->config.enable_theme_button &&
            event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
            event.component_id == H2_STARBOY_COMPONENT_THEME_BUTTON &&
            event.payload_size >= sizeof(h2_runtime_button_action_event_t) &&
            h2_runtime_button_action_is_released(event.payload)) {
            h2_starboy_behavior_randomize_palette(
                &app->behavior, now_ms);
            printf(
                "H2_STARBOY_PALETTE eye=%04x pupil=%04x\n",
                (unsigned int)app->behavior.palette.eye_color,
                (unsigned int)app->behavior.palette.pupil_color);
        }
    }

    if (app->config.enable_power_button) {
        h2_runtime_button_state_t state = {0};
        h2_pal_result_t rc = starboy_poll_button_state(
            app,
            H2_STARBOY_COMPONENT_POWER_BUTTON,
            &state);
        if (rc == H2_PAL_OK) {
            if (!app->shutdown_pending && h2_starboy_power_hold_update(
                    &app->power_hold_tracker, state.pressed, now_ms)) {
                app->shutdown_pending = 1;
                h2_starboy_behavior_begin_exit(&app->behavior, now_ms);
                printf("H2_STARBOY_POWER action=departure\n");
            }
        } else if (rc != H2_PAL_ERR_NOT_FOUND &&
                   rc != H2_PAL_ERR_WOULD_BLOCK) {
            (void)h2_starboy_power_hold_update(
                &app->power_hold_tracker, 0, now_ms);
            printf("H2_STARBOY_BUTTON component=power rc=%d\n", (int)rc);
        }
    }
}

static h2_pal_result_t starboy_initialize(
    h2_starboy_app_t *app,
    h2_display_info_t *out_display_info,
    uint64_t *out_now_ms) {
    int result = h2_pal_display_open(app->runtime->display);
    if (result != H2_DISPLAY_OK) {
        return (h2_pal_result_t)result;
    }
    app->display_opened = 1;
    result = h2_pal_display_get_info(app->runtime->display, out_display_info);
    if (result != H2_DISPLAY_OK || out_display_info->width <= 0 ||
        out_display_info->height <= 0) {
        return result == H2_DISPLAY_OK
            ? H2_PAL_ERR_INVALID_ARG
            : (h2_pal_result_t)result;
    }
    if ((size_t)out_display_info->width >
        SIZE_MAX / (size_t)out_display_info->height / sizeof(uint16_t)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    app->framebuffer_bytes =
        (size_t)out_display_info->width * (size_t)out_display_info->height *
        sizeof(uint16_t);
    app->framebuffer =
        (uint16_t *)h2_pal_mem_alloc(app->runtime->mem, app->framebuffer_bytes);
    if (app->framebuffer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (h2_pal_time_get_monotonic_ms(app->runtime->time, out_now_ms) !=
        H2_PAL_OK) {
        return H2_PAL_ERR_UNAVAILABLE;
    }

    (void)h2_pal_display_set_brightness_percent(app->runtime->display, 90u);
    const h2_pal_result_t touch_result = starboy_try_open_touch(app);
    if (touch_result != H2_PAL_OK &&
        starboy_touch_result_is_retryable(touch_result)) {
        app->next_touch_retry_ms =
            *out_now_ms + H2_STARBOY_TOUCH_RETRY_MS;
    }
    starboy_try_start_microphone(app);
    h2_starboy_behavior_init(
        &app->behavior, *out_now_ms, app->config.random_seed);
    h2_starboy_behavior_set_initial_pupil_style(
        &app->behavior, app->config.initial_pupil_style);
    printf(
        "H2_STARBOY_LOOK eye=%04x pupil=%04x shape=%d\n",
        (unsigned int)app->behavior.palette.eye_color,
        (unsigned int)app->behavior.palette.pupil_color,
        (int)app->behavior.pupil_style);
    return H2_PAL_OK;
}

int h2_starboy_run(
    h2_runtime_t *runtime,
    const h2_starboy_config_t *config) {
    if (runtime == NULL || runtime->display == NULL || runtime->mem == NULL ||
        runtime->time == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_starboy_app_t app = {0};
    app.runtime = runtime;
    app.config = config == NULL ? (h2_starboy_config_t){0} : *config;
    if (app.config.frame_interval_ms == 0u) {
        app.config.frame_interval_ms = H2_STARBOY_DEFAULT_FRAME_INTERVAL_MS;
    }
    if ((unsigned int)app.config.initial_pupil_style >=
        H2_STARBOY_PUPIL_STYLE_COUNT) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (app.config.motion != NULL &&
        (app.config.motion->vtable == NULL ||
         app.config.motion->vtable->read == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_display_info_t display_info = {0};
    uint64_t now_ms = 0u;
    h2_pal_result_t result =
        starboy_initialize(&app, &display_info, &now_ms);
    if (result != H2_PAL_OK) {
        starboy_cleanup(&app);
        return result;
    }

    int ready_sent = 0;
    while (!starboy_should_stop(&app)) {
        if (h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) !=
            H2_PAL_OK) {
            result = H2_PAL_ERR_UNAVAILABLE;
            break;
        }
        starboy_poll_touch(&app, now_ms);
        starboy_poll_microphone(&app, now_ms);
        starboy_poll_buttons(&app, now_ms);

        h2_starboy_behavior_input_t input = {
            .now_ms = now_ms,
            .pointer_active = app.pointer_active,
            .pointer_x_q15 = app.pointer_x_q15,
            .pointer_y_q15 = app.pointer_y_q15,
            .audio_level = app.audio_level,
        };
        starboy_poll_motion(&app, &input);
        const h2_starboy_pupil_style_t previous_pupil_style =
            app.behavior.pupil_style;
        h2_starboy_behavior_step(&app.behavior, &input);
        if (app.behavior.pupil_style != previous_pupil_style) {
            printf(
                "H2_STARBOY_PUPIL style=%d\n",
                (int)app.behavior.pupil_style);
        }
        uint64_t input_finished_ms = 0u;
        if (h2_pal_time_get_monotonic_ms(
                runtime->time, &input_finished_ms) != H2_PAL_OK) {
            result = H2_PAL_ERR_UNAVAILABLE;
            break;
        }
        h2_starboy_render_regions_t render_regions = {0};
        const h2_starboy_render_regions_t *previous_render_regions =
            app.previous_content_valid
                ? &app.previous_render_regions
                : NULL;
        h2_starboy_render_dirty_regions(
            app.framebuffer,
            (uint32_t)display_info.width,
            (uint32_t)display_info.height,
            &app.behavior,
            previous_render_regions,
            &render_regions);
        uint64_t render_finished_ms = 0u;
        if (h2_pal_time_get_monotonic_ms(
                runtime->time, &render_finished_ms) != H2_PAL_OK) {
            result = H2_PAL_ERR_UNAVAILABLE;
            break;
        }
        for (size_t dirty_index = 0u;
             dirty_index < render_regions.dirty_rect_count;
             ++dirty_index) {
            const h2_display_rect_t *draw_rect =
                &render_regions.dirty_rects[dirty_index];
            const uint16_t *draw_pixels = app.framebuffer +
                (size_t)draw_rect->y * (size_t)display_info.width +
                (size_t)draw_rect->x;
            result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
                runtime->display,
                draw_rect,
                draw_pixels,
                (size_t)display_info.width * sizeof(*app.framebuffer),
                H2_DISPLAY_PIXEL_RGB565);
            if (result != H2_PAL_OK) {
                break;
            }
        }
        if (result != H2_PAL_OK) {
            break;
        }
        uint64_t draw_finished_ms = 0u;
        if (h2_pal_time_get_monotonic_ms(
                runtime->time, &draw_finished_ms) != H2_PAL_OK) {
            result = H2_PAL_ERR_UNAVAILABLE;
            break;
        }
        app.previous_render_regions = render_regions;
        app.previous_content_valid = 1;
        result = (h2_pal_result_t)h2_pal_display_present(runtime->display);
        if (result != H2_PAL_OK) {
            break;
        }
        if (!ready_sent && app.config.ready != NULL) {
            result = app.config.ready(app.config.ready_user);
            if (result != H2_PAL_OK) {
                break;
            }
        }
        ready_sent = 1;
        if (app.shutdown_pending &&
            h2_starboy_behavior_exit_complete(&app.behavior)) {
            result = h2_pal_power_shutdown(app.runtime->power, 0u);
            printf(
                "H2_STARBOY_POWER action=shutdown rc=%d\n",
                (int)result);
            if (result != H2_PAL_OK) {
                app.shutdown_pending = 0;
                h2_starboy_behavior_restart_entry(&app.behavior, now_ms);
            } else {
                break;
            }
        }
        uint64_t frame_finished_ms = 0u;
        if (h2_pal_time_get_monotonic_ms(
                runtime->time, &frame_finished_ms) != H2_PAL_OK) {
            result = H2_PAL_ERR_UNAVAILABLE;
            break;
        }
        starboy_record_performance(
            &app,
            &display_info,
            &render_regions,
            now_ms,
            input_finished_ms,
            render_finished_ms,
            draw_finished_ms,
            frame_finished_ms);
        const uint64_t frame_elapsed_ms = frame_finished_ms >= now_ms
            ? frame_finished_ms - now_ms
            : 0u;
        if (frame_elapsed_ms < app.config.frame_interval_ms) {
            result = h2_pal_time_sleep_ms(
                runtime->time,
                app.config.frame_interval_ms - (uint32_t)frame_elapsed_ms);
            if (result != H2_PAL_OK) {
                break;
            }
        }
    }

    starboy_cleanup(&app);
    return result;
}
