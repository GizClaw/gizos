#ifndef H2_STARBOY_INTERNAL_H
#define H2_STARBOY_INTERNAL_H

#include "h2_starboy.h"

#include <stddef.h>
#include <stdint.h>

#define H2_STARBOY_Q15_ONE 32767
#define H2_STARBOY_EYE_COUNT 2u

typedef enum h2_starboy_emotion {
    H2_STARBOY_EMOTION_CALM = 0,
    H2_STARBOY_EMOTION_ANXIOUS,
    H2_STARBOY_EMOTION_COUNT,
} h2_starboy_emotion_t;

typedef enum h2_starboy_scene_phase {
    H2_STARBOY_SCENE_ENTERING = 0,
    H2_STARBOY_SCENE_PRESENT,
    H2_STARBOY_SCENE_LEAVING,
    H2_STARBOY_SCENE_GONE,
} h2_starboy_scene_phase_t;

typedef enum h2_starboy_idle_pose {
    H2_STARBOY_IDLE_POSE_NEUTRAL = 0,
    H2_STARBOY_IDLE_POSE_CURIOUS,
    H2_STARBOY_IDLE_POSE_SQUINT,
    H2_STARBOY_IDLE_POSE_WIDE,
} h2_starboy_idle_pose_t;

typedef struct h2_starboy_palette {
    uint16_t eye_color;
    uint16_t pupil_color;
} h2_starboy_palette_t;

typedef struct h2_starboy_behavior_input {
    uint64_t now_ms;
    int pointer_active;
    int32_t pointer_x_q15;
    int32_t pointer_y_q15;
    uint32_t audio_level;
    int gravity_valid;
    int32_t gravity_x_mg;
    int32_t gravity_y_mg;
    int32_t gravity_z_mg;
    int shake_valid;
    int32_t shake_magnitude_mg;
    uint32_t shake_duration_ms;
} h2_starboy_behavior_input_t;

typedef struct h2_starboy_behavior {
    uint32_t random_state;
    uint64_t last_update_ms;
    uint64_t next_idle_scan_ms;
    uint64_t next_idle_pose_ms;
    uint64_t next_wing_flutter_ms;
    uint64_t wing_flutter_started_ms;
    uint64_t next_blink_ms;
    uint64_t blink_start_ms;
    uint64_t loud_since_ms;
    uint64_t quiet_since_ms;
    uint64_t anxious_cooldown_until_ms;
    uint64_t pupil_transition_started_ms;
    uint64_t pupil_change_cooldown_until_ms;
    uint64_t scene_transition_started_ms;
    int32_t gaze_x_q15;
    int32_t gaze_y_q15;
    int32_t head_x_q15;
    int32_t head_y_q15;
    int32_t tracking_q15;
    int32_t target_x_q15;
    int32_t target_y_q15;
    int32_t blink_q15;
    int32_t pose_tilt_q15;
    int32_t pose_squash_q15;
    int32_t pose_asymmetry_q15;
    int32_t wing_flutter_q15;
    int32_t target_pose_tilt_q15;
    int32_t target_pose_squash_q15;
    int32_t target_pose_asymmetry_q15;
    int32_t gravity_down_x_q15;
    int32_t gravity_down_y_q15;
    int gravity_turn_direction;
    int32_t emotion_mix_q15[H2_STARBOY_EMOTION_COUNT];
    uint64_t palette_transition_started_ms;
    int32_t palette_mix_q15;
    int32_t pupil_style_mix_q15;
    int32_t scene_scale_q15;
    int32_t scene_bob_q15;
    int32_t motion_offset_x_q2;
    int32_t motion_offset_y_q2;
    int audio_loud;
    h2_starboy_emotion_t emotion;
    h2_starboy_idle_pose_t idle_pose;
    h2_starboy_palette_t palette;
    h2_starboy_palette_t previous_palette;
    h2_starboy_pupil_style_t pupil_style;
    h2_starboy_pupil_style_t previous_pupil_style;
    h2_starboy_scene_phase_t scene_phase;
} h2_starboy_behavior_t;

typedef struct h2_starboy_shake_tracker {
    uint64_t last_event_at_ms;
    uint32_t streak_duration_ms;
    int seen;
} h2_starboy_shake_tracker_t;

typedef struct h2_starboy_power_hold_tracker {
    uint64_t pressed_since_ms;
    int pressed;
    int triggered;
} h2_starboy_power_hold_tracker_t;

typedef struct h2_starboy_render_regions {
    h2_display_rect_t content_rect;
    h2_display_rect_t eye_content_rects[H2_STARBOY_EYE_COUNT];
    h2_display_rect_t dirty_rects[H2_STARBOY_EYE_COUNT];
    size_t dirty_rect_count;
} h2_starboy_render_regions_t;

void h2_starboy_behavior_init(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms,
    uint32_t random_seed);

void h2_starboy_behavior_step(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input);

void h2_starboy_behavior_randomize_palette(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms);

void h2_starboy_behavior_set_initial_pupil_style(
    h2_starboy_behavior_t *behavior,
    h2_starboy_pupil_style_t pupil_style);

void h2_starboy_behavior_next_pupil_style(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms);

void h2_starboy_behavior_begin_exit(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms);

void h2_starboy_behavior_restart_entry(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms);

int h2_starboy_behavior_exit_complete(
    const h2_starboy_behavior_t *behavior);

int32_t h2_starboy_normalize_coordinate(int32_t value, uint32_t extent);

uint32_t h2_starboy_audio_level_s16(
    const int16_t *samples,
    size_t sample_count);

uint32_t h2_starboy_audio_envelope_step(
    uint32_t current_level,
    uint32_t frame_level,
    int frame_available);

int h2_starboy_shake_tracker_accept(
    h2_starboy_shake_tracker_t *tracker,
    uint64_t event_at_ms,
    uint32_t duration_ms,
    uint32_t *out_streak_duration_ms);

int h2_starboy_power_hold_update(
    h2_starboy_power_hold_tracker_t *tracker,
    int pressed,
    uint64_t now_ms);

void h2_starboy_render_dirty_regions(
    uint16_t *pixels,
    uint32_t width,
    uint32_t height,
    const h2_starboy_behavior_t *behavior,
    const h2_starboy_render_regions_t *previous_regions,
    h2_starboy_render_regions_t *out_regions);

h2_display_rect_t h2_starboy_render(
    uint16_t *pixels,
    uint32_t width,
    uint32_t height,
    const h2_starboy_behavior_t *behavior,
    const h2_display_rect_t *previous_content_rect,
    h2_display_rect_t *out_content_rect);

#endif
