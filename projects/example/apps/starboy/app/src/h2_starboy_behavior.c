#include "h2_starboy_internal.h"

#include <limits.h>

#define H2_STARBOY_GAZE_EASE_MS 28u
#define H2_STARBOY_HEAD_EASE_MS 135u
#define H2_STARBOY_TRACKING_EASE_MS 45u
#define H2_STARBOY_POSE_EASE_MS 260u
#define H2_STARBOY_WING_FLUTTER_RISE_MS 260u
#define H2_STARBOY_WING_FLUTTER_FALL_MS 360u
#define H2_STARBOY_WING_FLUTTER_EASE_MS 70u
#define H2_STARBOY_WING_FLUTTER_AMPLITUDE_Q15 1500
#define H2_STARBOY_COLOR_EASE_MS 220u
#define H2_STARBOY_PALETTE_EASE_MS 240u
#define H2_STARBOY_PUPIL_STYLE_EASE_MS 260u
#define H2_STARBOY_BLINK_CLOSE_MS 70u
#define H2_STARBOY_BLINK_OPEN_MS 110u
#define H2_STARBOY_AUDIO_ENTER_LEVEL 4000u
#define H2_STARBOY_AUDIO_EXIT_LEVEL 2200u
#define H2_STARBOY_AUDIO_ENTER_MS 180u
#define H2_STARBOY_AUDIO_EXIT_MS 650u
#define H2_STARBOY_AUDIO_COOLDOWN_MS 900u
#define H2_STARBOY_PUPIL_CHANGE_MAGNITUDE_MG 1200
#define H2_STARBOY_PUPIL_CHANGE_DURATION_MS 300u
#define H2_STARBOY_PUPIL_CHANGE_COOLDOWN_MS 700u
#define H2_STARBOY_SHAKE_STREAK_GAP_MS 350u
#define H2_STARBOY_GRAVITY_EASE_MS 48u
#define H2_STARBOY_GRAVITY_MIN_MG 700
#define H2_STARBOY_GRAVITY_MAX_MG 1350
#define H2_STARBOY_GRAVITY_PLANAR_MIN_MG 300
#define H2_STARBOY_GRAVITY_MAX_STEP_Q15 10000
#define H2_STARBOY_POWER_HOLD_MS 2000u
#define H2_STARBOY_SCENE_ENTER_MS 1000u
#define H2_STARBOY_SCENE_EXIT_MS 900u
#define H2_STARBOY_SCENE_FAR_SCALE_Q15 3600

static uint32_t starboy_random(h2_starboy_behavior_t *behavior) {
    behavior->random_state =
        behavior->random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return behavior->random_state;
}

typedef struct starboy_rgb888 {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} starboy_rgb888_t;

static uint16_t starboy_rgb565(starboy_rgb888_t color) {
    return (uint16_t)(
        (((uint16_t)color.red & UINT16_C(0xf8)) << 8) |
        (((uint16_t)color.green & UINT16_C(0xfc)) << 3) |
        ((uint16_t)color.blue >> 3));
}

static uint8_t starboy_luminance(starboy_rgb888_t color) {
    return (uint8_t)(
        ((uint32_t)color.red * 54u +
         (uint32_t)color.green * 183u +
         (uint32_t)color.blue * 19u + 128u) /
        256u);
}

static starboy_rgb888_t starboy_hsv(
    uint32_t hue,
    uint8_t saturation,
    uint8_t value) {
    hue %= 1536u;
    const uint32_t sector = hue / 256u;
    const uint32_t fraction = hue % 256u;
    const uint8_t low = (uint8_t)(
        (uint32_t)value * (255u - saturation) / 255u);
    const uint8_t falling = (uint8_t)(
        (uint32_t)value *
        (255u - (uint32_t)saturation * fraction / 255u) /
        255u);
    const uint8_t rising = (uint8_t)(
        (uint32_t)value *
        (255u - (uint32_t)saturation * (255u - fraction) / 255u) /
        255u);
    switch (sector) {
        case 0u:
            return (starboy_rgb888_t){value, rising, low};
        case 1u:
            return (starboy_rgb888_t){falling, value, low};
        case 2u:
            return (starboy_rgb888_t){low, value, rising};
        case 3u:
            return (starboy_rgb888_t){low, falling, value};
        case 4u:
            return (starboy_rgb888_t){rising, low, value};
        default:
            return (starboy_rgb888_t){value, low, falling};
    }
}

static starboy_rgb888_t starboy_lighten_until_visible(
    starboy_rgb888_t color) {
    while (starboy_luminance(color) < 152u) {
        color.red = (uint8_t)(color.red + (255u - color.red) / 4u + 1u);
        color.green = (uint8_t)(
            color.green + (255u - color.green) / 4u + 1u);
        color.blue = (uint8_t)(
            color.blue + (255u - color.blue) / 4u + 1u);
    }
    return color;
}

static starboy_rgb888_t starboy_darken_until_contrasting(
    starboy_rgb888_t color) {
    while (starboy_luminance(color) > 70u) {
        color.red = (uint8_t)((uint32_t)color.red * 7u / 8u);
        color.green = (uint8_t)((uint32_t)color.green * 7u / 8u);
        color.blue = (uint8_t)((uint32_t)color.blue * 7u / 8u);
    }
    return color;
}

static h2_starboy_palette_t starboy_generate_palette(
    h2_starboy_behavior_t *behavior) {
    const uint32_t eye_hue = starboy_random(behavior) % 1536u;
    const uint8_t eye_saturation = (uint8_t)(
        144u + starboy_random(behavior) % 81u);
    const uint8_t eye_value = (uint8_t)(
        224u + starboy_random(behavior) % 32u);
    const starboy_rgb888_t eye = starboy_lighten_until_visible(
        starboy_hsv(eye_hue, eye_saturation, eye_value));

    const uint32_t pupil_hue =
        (eye_hue + 640u + starboy_random(behavior) % 257u) % 1536u;
    const uint8_t pupil_saturation = (uint8_t)(
        232u + starboy_random(behavior) % 24u);
    const uint8_t pupil_value = (uint8_t)(
        128u + starboy_random(behavior) % 65u);
    const starboy_rgb888_t pupil = starboy_darken_until_contrasting(
        starboy_hsv(pupil_hue, pupil_saturation, pupil_value));

    return (h2_starboy_palette_t){
        .eye_color = starboy_rgb565(eye),
        .pupil_color = starboy_rgb565(pupil),
    };
}

static uint16_t starboy_blend_rgb565(
    uint16_t first,
    uint16_t second,
    int32_t second_weight_q15) {
    const int32_t first_weight_q15 =
        H2_STARBOY_Q15_ONE - second_weight_q15;
    const uint32_t red = (uint32_t)(
        ((int64_t)((first >> 11) & 0x1fu) * first_weight_q15 +
         (int64_t)((second >> 11) & 0x1fu) * second_weight_q15 +
         H2_STARBOY_Q15_ONE / 2) /
        H2_STARBOY_Q15_ONE);
    const uint32_t green = (uint32_t)(
        ((int64_t)((first >> 5) & 0x3fu) * first_weight_q15 +
         (int64_t)((second >> 5) & 0x3fu) * second_weight_q15 +
         H2_STARBOY_Q15_ONE / 2) /
        H2_STARBOY_Q15_ONE);
    const uint32_t blue = (uint32_t)(
        ((int64_t)(first & 0x1fu) * first_weight_q15 +
         (int64_t)(second & 0x1fu) * second_weight_q15 +
         H2_STARBOY_Q15_ONE / 2) /
        H2_STARBOY_Q15_ONE);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static int32_t starboy_clamp_q15(int32_t value) {
    if (value < -H2_STARBOY_Q15_ONE) {
        return -H2_STARBOY_Q15_ONE;
    }
    if (value > H2_STARBOY_Q15_ONE) {
        return H2_STARBOY_Q15_ONE;
    }
    return value;
}

static int32_t starboy_abs_i32(int32_t value) {
    return value < 0 ? -value : value;
}

static int32_t starboy_triangle_q15(
    uint64_t now_ms,
    uint32_t period_ms,
    uint32_t phase_offset_ms) {
    const uint32_t phase = (uint32_t)(
        (now_ms + phase_offset_ms) % period_ms);
    const uint32_t half_period = period_ms / 2u;
    const uint32_t rising = phase <= half_period
        ? phase
        : period_ms - phase;
    return (int32_t)(
        (int64_t)rising * H2_STARBOY_Q15_ONE * 2 / half_period) -
        H2_STARBOY_Q15_ONE;
}

static int32_t starboy_smoothstep_q15(int32_t progress_q15) {
    progress_q15 = progress_q15 < 0 ? 0 : progress_q15;
    progress_q15 = progress_q15 > H2_STARBOY_Q15_ONE
        ? H2_STARBOY_Q15_ONE
        : progress_q15;
    const int32_t squared_q15 = (int32_t)(
        (int64_t)progress_q15 * progress_q15 / H2_STARBOY_Q15_ONE);
    return (int32_t)(
        (int64_t)squared_q15 *
        (3 * H2_STARBOY_Q15_ONE - 2 * progress_q15) /
        H2_STARBOY_Q15_ONE);
}

static uint32_t starboy_integer_sqrt_u64(uint64_t value) {
    uint64_t remainder = value;
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > remainder) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (remainder >= result + bit) {
            remainder -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

static int32_t starboy_norm_2d(int32_t x, int32_t y) {
    const int64_t x64 = x;
    const int64_t y64 = y;
    const uint64_t squared = (uint64_t)(x64 * x64) +
        (uint64_t)(y64 * y64);
    const uint32_t norm = starboy_integer_sqrt_u64(squared);
    return norm > INT32_MAX ? INT32_MAX : (int32_t)norm;
}

static int32_t starboy_norm_3d(
    int32_t x,
    int32_t y,
    int32_t z) {
    const int64_t x64 = x;
    const int64_t y64 = y;
    const int64_t z64 = z;
    const uint64_t squared = (uint64_t)(x64 * x64) +
        (uint64_t)(y64 * y64) + (uint64_t)(z64 * z64);
    const uint32_t norm = starboy_integer_sqrt_u64(squared);
    return norm > INT32_MAX ? INT32_MAX : (int32_t)norm;
}

static int64_t starboy_elapsed_or_zero(uint64_t now_ms, uint64_t since_ms) {
    if (since_ms == 0u || now_ms < since_ms) {
        return 0;
    }
    const uint64_t elapsed = now_ms - since_ms;
    return elapsed > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)elapsed;
}

static void starboy_select_idle_target(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    const uint32_t x_random = starboy_random(behavior);
    const uint32_t y_random = starboy_random(behavior);
    behavior->target_x_q15 =
        (int32_t)(x_random % UINT32_C(49151)) - 24575;
    behavior->target_y_q15 =
        (int32_t)(y_random % UINT32_C(32769)) - 16384;
    behavior->next_idle_scan_ms =
        now_ms + UINT64_C(1100) + (starboy_random(behavior) % UINT32_C(1700));
}

static void starboy_select_idle_pose(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    behavior->idle_pose = (h2_starboy_idle_pose_t)(
        starboy_random(behavior) % UINT32_C(4));
    const int32_t direction = (starboy_random(behavior) & 1u) != 0u ? 1 : -1;
    switch (behavior->idle_pose) {
        case H2_STARBOY_IDLE_POSE_CURIOUS:
            behavior->target_pose_tilt_q15 = direction * 9200;
            behavior->target_pose_squash_q15 = 1200;
            behavior->target_pose_asymmetry_q15 = direction * 1400;
            break;
        case H2_STARBOY_IDLE_POSE_SQUINT:
            behavior->target_pose_tilt_q15 = direction * 2600;
            behavior->target_pose_squash_q15 = -1800;
            behavior->target_pose_asymmetry_q15 = direction * 800;
            break;
        case H2_STARBOY_IDLE_POSE_WIDE:
            behavior->target_pose_tilt_q15 = direction * 4100;
            behavior->target_pose_squash_q15 = 4800;
            behavior->target_pose_asymmetry_q15 = -direction * 1000;
            break;
        case H2_STARBOY_IDLE_POSE_NEUTRAL:
        default:
            behavior->target_pose_tilt_q15 = direction * 900;
            behavior->target_pose_squash_q15 = 0;
            behavior->target_pose_asymmetry_q15 = 0;
            break;
    }
    behavior->next_idle_pose_ms =
        now_ms + UINT64_C(3000) +
        (starboy_random(behavior) % UINT32_C(3001));
}

static void starboy_schedule_wing_flutter(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    behavior->next_wing_flutter_ms =
        now_ms + UINT64_C(1800) +
        (starboy_random(behavior) % UINT32_C(2801));
}

static void starboy_ease_value(
    int32_t *value,
    int32_t target,
    uint64_t delta_ms,
    uint32_t ease_ms) {
    if (delta_ms == 0u) {
        return;
    }
    const int64_t divisor = (int64_t)ease_ms + (int64_t)delta_ms;
    *value += (int32_t)(
        ((int64_t)(target - *value) * (int64_t)delta_ms) / divisor);
}

static void starboy_update_wing_flutter(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input,
    uint64_t delta_ms) {
    int32_t target_q15 = 0;
    if (input->pointer_active) {
        behavior->wing_flutter_started_ms = 0u;
        behavior->next_wing_flutter_ms = input->now_ms + UINT64_C(900);
    } else {
        if (behavior->wing_flutter_started_ms == 0u &&
            input->now_ms >= behavior->next_wing_flutter_ms) {
            behavior->wing_flutter_started_ms = input->now_ms;
        }
        if (behavior->wing_flutter_started_ms != 0u) {
            const uint64_t elapsed_ms =
                input->now_ms - behavior->wing_flutter_started_ms;
            if (elapsed_ms < H2_STARBOY_WING_FLUTTER_RISE_MS) {
                target_q15 = starboy_smoothstep_q15((int32_t)(
                    elapsed_ms * H2_STARBOY_Q15_ONE /
                    H2_STARBOY_WING_FLUTTER_RISE_MS));
            } else if (elapsed_ms <
                       H2_STARBOY_WING_FLUTTER_RISE_MS +
                           H2_STARBOY_WING_FLUTTER_FALL_MS) {
                target_q15 = H2_STARBOY_Q15_ONE -
                    starboy_smoothstep_q15((int32_t)(
                        (elapsed_ms - H2_STARBOY_WING_FLUTTER_RISE_MS) *
                        H2_STARBOY_Q15_ONE /
                        H2_STARBOY_WING_FLUTTER_FALL_MS));
            } else {
                behavior->wing_flutter_started_ms = 0u;
                starboy_schedule_wing_flutter(behavior, input->now_ms);
            }
        }
    }
    starboy_ease_value(
        &behavior->wing_flutter_q15,
        (int32_t)((int64_t)target_q15 *
                  H2_STARBOY_WING_FLUTTER_AMPLITUDE_Q15 /
                  H2_STARBOY_Q15_ONE),
        delta_ms,
        H2_STARBOY_WING_FLUTTER_EASE_MS);
}

static void starboy_update_gravity(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input,
    uint64_t delta_ms) {
    if (!input->gravity_valid || delta_ms == 0u) {
        return;
    }
    const int32_t norm_mg = starboy_norm_3d(
        input->gravity_x_mg,
        input->gravity_y_mg,
        input->gravity_z_mg);
    const int32_t planar_norm_mg = starboy_norm_2d(
        input->gravity_x_mg, input->gravity_y_mg);
    if (norm_mg < H2_STARBOY_GRAVITY_MIN_MG ||
        norm_mg > H2_STARBOY_GRAVITY_MAX_MG ||
        planar_norm_mg < H2_STARBOY_GRAVITY_PLANAR_MIN_MG) {
        return;
    }

    /* An accelerometer senses support force, opposite the direction of down. */
    const int32_t target_x_q15 = (int32_t)(
        -(int64_t)input->gravity_x_mg * H2_STARBOY_Q15_ONE /
        planar_norm_mg);
    const int32_t target_y_q15 = (int32_t)(
        -(int64_t)input->gravity_y_mg * H2_STARBOY_Q15_ONE /
        planar_norm_mg);
    const int32_t cross_q15 = (int32_t)(
        ((int64_t)behavior->gravity_down_x_q15 * target_y_q15 -
         (int64_t)behavior->gravity_down_y_q15 * target_x_q15) /
        H2_STARBOY_Q15_ONE);
    const int32_t dot_q15 = (int32_t)(
        ((int64_t)behavior->gravity_down_x_q15 * target_x_q15 +
         (int64_t)behavior->gravity_down_y_q15 * target_y_q15) /
        H2_STARBOY_Q15_ONE);
    if (starboy_abs_i32(cross_q15) > 256) {
        behavior->gravity_turn_direction = cross_q15 < 0 ? 1 : -1;
    }
    int32_t error_q15 = starboy_abs_i32(cross_q15);
    if (dot_q15 < 0) {
        error_q15 = H2_STARBOY_Q15_ONE +
            (H2_STARBOY_Q15_ONE - error_q15);
    }
    int32_t step_q15 = (int32_t)(
        (int64_t)error_q15 * (int64_t)delta_ms /
        (H2_STARBOY_GRAVITY_EASE_MS + delta_ms));
    if (step_q15 > H2_STARBOY_GRAVITY_MAX_STEP_Q15) {
        step_q15 = H2_STARBOY_GRAVITY_MAX_STEP_Q15;
    }

    const int32_t right_x_q15 = behavior->gravity_down_y_q15;
    const int32_t right_y_q15 = -behavior->gravity_down_x_q15;
    behavior->gravity_down_x_q15 += (int32_t)(
        (int64_t)behavior->gravity_turn_direction * right_x_q15 *
        step_q15 / H2_STARBOY_Q15_ONE);
    behavior->gravity_down_y_q15 += (int32_t)(
        (int64_t)behavior->gravity_turn_direction * right_y_q15 *
        step_q15 / H2_STARBOY_Q15_ONE);
    const int32_t updated_norm_q15 = starboy_norm_2d(
        behavior->gravity_down_x_q15,
        behavior->gravity_down_y_q15);
    if (updated_norm_q15 > 0) {
        behavior->gravity_down_x_q15 = (int32_t)(
            (int64_t)behavior->gravity_down_x_q15 * H2_STARBOY_Q15_ONE /
            updated_norm_q15);
        behavior->gravity_down_y_q15 = (int32_t)(
            (int64_t)behavior->gravity_down_y_q15 * H2_STARBOY_Q15_ONE /
            updated_norm_q15);
    }
}

static void starboy_update_blink(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior->blink_start_ms == 0u && now_ms >= behavior->next_blink_ms) {
        behavior->blink_start_ms = now_ms;
    }
    if (behavior->blink_start_ms == 0u) {
        behavior->blink_q15 = 0;
        return;
    }

    const uint64_t elapsed = now_ms - behavior->blink_start_ms;
    if (elapsed < H2_STARBOY_BLINK_CLOSE_MS) {
        behavior->blink_q15 = (int32_t)(
            elapsed * H2_STARBOY_Q15_ONE / H2_STARBOY_BLINK_CLOSE_MS);
        return;
    }
    if (elapsed < H2_STARBOY_BLINK_CLOSE_MS + H2_STARBOY_BLINK_OPEN_MS) {
        const uint64_t opening = elapsed - H2_STARBOY_BLINK_CLOSE_MS;
        behavior->blink_q15 = H2_STARBOY_Q15_ONE - (int32_t)(
            opening * H2_STARBOY_Q15_ONE / H2_STARBOY_BLINK_OPEN_MS);
        return;
    }

    behavior->blink_q15 = 0;
    behavior->blink_start_ms = 0u;
    const uint32_t blink_random = starboy_random(behavior);
    if (blink_random % UINT32_C(5) == 0u) {
        behavior->next_blink_ms =
            now_ms + UINT64_C(140) + (blink_random % UINT32_C(100));
    } else {
        behavior->next_blink_ms =
            now_ms + UINT64_C(1900) +
            (blink_random % UINT32_C(3300));
    }
}

static void starboy_update_audio(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input) {
    if (input->audio_level >= H2_STARBOY_AUDIO_ENTER_LEVEL) {
        behavior->quiet_since_ms = 0u;
        if (behavior->loud_since_ms == 0u) {
            behavior->loud_since_ms = input->now_ms;
        }
        if (!behavior->audio_loud &&
            input->now_ms >= behavior->anxious_cooldown_until_ms &&
            starboy_elapsed_or_zero(input->now_ms, behavior->loud_since_ms) >=
                H2_STARBOY_AUDIO_ENTER_MS) {
            behavior->audio_loud = 1;
        }
        return;
    }

    behavior->loud_since_ms = 0u;
    if (input->audio_level > H2_STARBOY_AUDIO_EXIT_LEVEL) {
        behavior->quiet_since_ms = 0u;
        return;
    }
    if (behavior->quiet_since_ms == 0u) {
        behavior->quiet_since_ms = input->now_ms;
    }
    if (behavior->audio_loud &&
        starboy_elapsed_or_zero(input->now_ms, behavior->quiet_since_ms) >=
            H2_STARBOY_AUDIO_EXIT_MS) {
        behavior->audio_loud = 0;
        behavior->anxious_cooldown_until_ms =
            input->now_ms + H2_STARBOY_AUDIO_COOLDOWN_MS;
    }
}

static void starboy_update_motion(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input) {
    if (input->shake_valid &&
        input->now_ms >= behavior->pupil_change_cooldown_until_ms &&
        (input->shake_magnitude_mg >=
             H2_STARBOY_PUPIL_CHANGE_MAGNITUDE_MG ||
         input->shake_duration_ms >= H2_STARBOY_PUPIL_CHANGE_DURATION_MS)) {
        h2_starboy_behavior_next_pupil_style(behavior, input->now_ms);
        behavior->pupil_change_cooldown_until_ms =
            input->now_ms + H2_STARBOY_PUPIL_CHANGE_COOLDOWN_MS;
    }

    if (behavior->audio_loud) {
        behavior->emotion = H2_STARBOY_EMOTION_ANXIOUS;
    } else {
        behavior->emotion = H2_STARBOY_EMOTION_CALM;
    }

    const int32_t anxious_mix =
        behavior->emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS];
    behavior->motion_offset_x_q2 = (int32_t)(
        (int64_t)starboy_triangle_q15(
            input->now_ms, 720u, 0u) * 6 * anxious_mix /
        H2_STARBOY_Q15_ONE / H2_STARBOY_Q15_ONE);
    behavior->motion_offset_y_q2 = (int32_t)(
        (int64_t)starboy_triangle_q15(
            input->now_ms, 1040u, 210u) * 4 * anxious_mix /
        H2_STARBOY_Q15_ONE / H2_STARBOY_Q15_ONE);
}

static void starboy_update_emotion_mix(
    h2_starboy_behavior_t *behavior,
    uint64_t delta_ms) {
    for (size_t emotion = 0u;
         emotion < H2_STARBOY_EMOTION_COUNT;
         ++emotion) {
        const int32_t target = emotion == (size_t)behavior->emotion
            ? H2_STARBOY_Q15_ONE
            : 0;
        starboy_ease_value(
            &behavior->emotion_mix_q15[emotion],
            target,
            delta_ms,
            H2_STARBOY_COLOR_EASE_MS);
        const int32_t remaining =
            target - behavior->emotion_mix_q15[emotion];
        if (remaining >= -32 && remaining <= 32) {
            behavior->emotion_mix_q15[emotion] = target;
        }
    }
}

static void starboy_update_palette_mix(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior->palette_mix_q15 >= H2_STARBOY_Q15_ONE) {
        behavior->palette_mix_q15 = H2_STARBOY_Q15_ONE;
        return;
    }
    const int64_t elapsed_ms = starboy_elapsed_or_zero(
        now_ms, behavior->palette_transition_started_ms);
    if (elapsed_ms >= H2_STARBOY_PALETTE_EASE_MS) {
        behavior->palette_mix_q15 = H2_STARBOY_Q15_ONE;
        behavior->previous_palette = behavior->palette;
        return;
    }
    behavior->palette_mix_q15 = starboy_smoothstep_q15((int32_t)(
        elapsed_ms * H2_STARBOY_Q15_ONE / H2_STARBOY_PALETTE_EASE_MS));
}

static void starboy_update_pupil_style_mix(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior->pupil_style_mix_q15 >= H2_STARBOY_Q15_ONE) {
        behavior->pupil_style_mix_q15 = H2_STARBOY_Q15_ONE;
        return;
    }
    const int64_t elapsed_ms = starboy_elapsed_or_zero(
        now_ms, behavior->pupil_transition_started_ms);
    if (elapsed_ms >= H2_STARBOY_PUPIL_STYLE_EASE_MS) {
        behavior->pupil_style_mix_q15 = H2_STARBOY_Q15_ONE;
        behavior->previous_pupil_style = behavior->pupil_style;
        return;
    }
    const int32_t progress_q15 = (int32_t)(
        elapsed_ms * H2_STARBOY_Q15_ONE /
        H2_STARBOY_PUPIL_STYLE_EASE_MS);
    behavior->pupil_style_mix_q15 = starboy_smoothstep_q15(progress_q15);
}

static void starboy_update_scene(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    uint32_t duration_ms = H2_STARBOY_SCENE_ENTER_MS;
    if (behavior->scene_phase == H2_STARBOY_SCENE_PRESENT ||
        behavior->scene_phase == H2_STARBOY_SCENE_GONE) {
        behavior->scene_scale_q15 =
            behavior->scene_phase == H2_STARBOY_SCENE_PRESENT
            ? H2_STARBOY_Q15_ONE
            : 0;
        behavior->scene_bob_q15 = 0;
        return;
    }
    if (behavior->scene_phase == H2_STARBOY_SCENE_LEAVING) {
        duration_ms = H2_STARBOY_SCENE_EXIT_MS;
    }
    const int64_t elapsed_ms = starboy_elapsed_or_zero(
        now_ms, behavior->scene_transition_started_ms);
    if (elapsed_ms >= duration_ms) {
        behavior->scene_phase = behavior->scene_phase ==
                H2_STARBOY_SCENE_ENTERING
            ? H2_STARBOY_SCENE_PRESENT
            : H2_STARBOY_SCENE_GONE;
        behavior->scene_scale_q15 =
            behavior->scene_phase == H2_STARBOY_SCENE_PRESENT
            ? H2_STARBOY_Q15_ONE
            : 0;
        behavior->scene_bob_q15 = 0;
        return;
    }

    const int32_t progress_q15 = (int32_t)(
        elapsed_ms * H2_STARBOY_Q15_ONE / duration_ms);
    const int32_t eased_q15 = starboy_smoothstep_q15(progress_q15);
    const int32_t approaching_scale_q15 =
        H2_STARBOY_SCENE_FAR_SCALE_Q15 + (int32_t)(
            (int64_t)(H2_STARBOY_Q15_ONE -
                      H2_STARBOY_SCENE_FAR_SCALE_Q15) *
            eased_q15 / H2_STARBOY_Q15_ONE);
    behavior->scene_scale_q15 =
        behavior->scene_phase == H2_STARBOY_SCENE_ENTERING
        ? approaching_scale_q15
        : H2_STARBOY_Q15_ONE - approaching_scale_q15 +
              H2_STARBOY_SCENE_FAR_SCALE_Q15;
    const int32_t remaining_q15 = H2_STARBOY_Q15_ONE - eased_q15;
    behavior->scene_bob_q15 = (int32_t)(
        (int64_t)starboy_triangle_q15(now_ms, 260u, 0u) *
        remaining_q15 / H2_STARBOY_Q15_ONE);
}

void h2_starboy_behavior_init(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms,
    uint32_t random_seed) {
    if (behavior == NULL) {
        return;
    }
    *behavior = (h2_starboy_behavior_t){0};
    behavior->random_state =
        random_seed == 0u ? UINT32_C(0x53a9b07d) : random_seed;
    behavior->last_update_ms = now_ms;
    behavior->emotion_mix_q15[H2_STARBOY_EMOTION_CALM] =
        H2_STARBOY_Q15_ONE;
    behavior->palette = starboy_generate_palette(behavior);
    behavior->previous_palette = behavior->palette;
    behavior->palette_mix_q15 = H2_STARBOY_Q15_ONE;
    behavior->pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN;
    behavior->previous_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN;
    behavior->pupil_style_mix_q15 = H2_STARBOY_Q15_ONE;
    behavior->scene_phase = H2_STARBOY_SCENE_ENTERING;
    behavior->scene_transition_started_ms = now_ms;
    behavior->scene_scale_q15 = H2_STARBOY_SCENE_FAR_SCALE_Q15;
    behavior->gravity_down_y_q15 = H2_STARBOY_Q15_ONE;
    behavior->gravity_turn_direction = 1;
    behavior->next_idle_scan_ms = now_ms;
    behavior->next_blink_ms =
        now_ms + UINT64_C(900) + (starboy_random(behavior) % UINT32_C(1600));
    starboy_select_idle_target(behavior, now_ms);
    starboy_select_idle_pose(behavior, now_ms);
    starboy_schedule_wing_flutter(behavior, now_ms);
}

void h2_starboy_behavior_step(
    h2_starboy_behavior_t *behavior,
    const h2_starboy_behavior_input_t *input) {
    if (behavior == NULL || input == NULL) {
        return;
    }

    uint64_t delta_ms = input->now_ms - behavior->last_update_ms;
    if (delta_ms > UINT64_C(100)) {
        delta_ms = UINT64_C(100);
    }
    behavior->last_update_ms = input->now_ms;

    starboy_update_gravity(behavior, input, delta_ms);

    if (input->pointer_active) {
        const int32_t right_x_q15 = behavior->gravity_down_y_q15;
        const int32_t right_y_q15 = -behavior->gravity_down_x_q15;
        behavior->target_x_q15 = starboy_clamp_q15((int32_t)(
            ((int64_t)right_x_q15 * input->pointer_x_q15 +
             (int64_t)right_y_q15 * input->pointer_y_q15) /
            H2_STARBOY_Q15_ONE));
        behavior->target_y_q15 = starboy_clamp_q15((int32_t)(
            ((int64_t)behavior->gravity_down_x_q15 *
                 input->pointer_x_q15 +
             (int64_t)behavior->gravity_down_y_q15 *
                 input->pointer_y_q15) /
            H2_STARBOY_Q15_ONE));
        behavior->next_idle_scan_ms = input->now_ms + UINT64_C(650);
    } else if (input->now_ms >= behavior->next_idle_scan_ms) {
        starboy_select_idle_target(behavior, input->now_ms);
    }

    if (input->now_ms >= behavior->next_idle_pose_ms) {
        starboy_select_idle_pose(behavior, input->now_ms);
    }

    starboy_ease_value(
        &behavior->gaze_x_q15,
        behavior->target_x_q15,
        delta_ms,
        H2_STARBOY_GAZE_EASE_MS);
    starboy_ease_value(
        &behavior->gaze_y_q15,
        behavior->target_y_q15,
        delta_ms,
        H2_STARBOY_GAZE_EASE_MS);
    starboy_ease_value(
        &behavior->head_x_q15,
        behavior->target_x_q15,
        delta_ms,
        H2_STARBOY_HEAD_EASE_MS);
    starboy_ease_value(
        &behavior->head_y_q15,
        behavior->target_y_q15,
        delta_ms,
        H2_STARBOY_HEAD_EASE_MS);
    starboy_ease_value(
        &behavior->tracking_q15,
        input->pointer_active ? H2_STARBOY_Q15_ONE : 0,
        delta_ms,
        H2_STARBOY_TRACKING_EASE_MS);
    starboy_ease_value(
        &behavior->pose_tilt_q15,
        behavior->target_pose_tilt_q15,
        delta_ms,
        H2_STARBOY_POSE_EASE_MS);
    starboy_ease_value(
        &behavior->pose_squash_q15,
        behavior->target_pose_squash_q15,
        delta_ms,
        H2_STARBOY_POSE_EASE_MS);
    starboy_ease_value(
        &behavior->pose_asymmetry_q15,
        behavior->target_pose_asymmetry_q15,
        delta_ms,
        H2_STARBOY_POSE_EASE_MS);
    starboy_update_wing_flutter(behavior, input, delta_ms);

    starboy_update_blink(behavior, input->now_ms);
    starboy_update_audio(behavior, input);
    starboy_update_motion(behavior, input);
    starboy_update_emotion_mix(behavior, delta_ms);
    starboy_update_palette_mix(behavior, input->now_ms);
    starboy_update_pupil_style_mix(behavior, input->now_ms);
    starboy_update_scene(behavior, input->now_ms);
}

void h2_starboy_behavior_set_initial_pupil_style(
    h2_starboy_behavior_t *behavior,
    h2_starboy_pupil_style_t pupil_style) {
    if (behavior == NULL ||
        (unsigned int)pupil_style >= H2_STARBOY_PUPIL_STYLE_COUNT) {
        return;
    }
    behavior->pupil_style = pupil_style;
    behavior->previous_pupil_style = pupil_style;
    behavior->pupil_style_mix_q15 = H2_STARBOY_Q15_ONE;
    behavior->pupil_transition_started_ms = 0u;
}

void h2_starboy_behavior_randomize_palette(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior == NULL) {
        return;
    }
    behavior->previous_palette = (h2_starboy_palette_t){
        .eye_color = starboy_blend_rgb565(
            behavior->previous_palette.eye_color,
            behavior->palette.eye_color,
            behavior->palette_mix_q15),
        .pupil_color = starboy_blend_rgb565(
            behavior->previous_palette.pupil_color,
            behavior->palette.pupil_color,
            behavior->palette_mix_q15),
    };
    do {
        behavior->palette = starboy_generate_palette(behavior);
    } while (behavior->palette.eye_color ==
                 behavior->previous_palette.eye_color ||
             behavior->palette.pupil_color ==
                 behavior->previous_palette.pupil_color);
    behavior->palette_mix_q15 = 0;
    behavior->palette_transition_started_ms = now_ms;
}

void h2_starboy_behavior_next_pupil_style(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior == NULL) {
        return;
    }
    behavior->previous_pupil_style = behavior->pupil_style;
    behavior->pupil_style = (h2_starboy_pupil_style_t)(
        ((uint32_t)behavior->pupil_style + 1u) %
        (uint32_t)H2_STARBOY_PUPIL_STYLE_COUNT);
    behavior->pupil_style_mix_q15 = 0;
    behavior->pupil_transition_started_ms = now_ms;
}

void h2_starboy_behavior_begin_exit(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior == NULL || behavior->scene_phase == H2_STARBOY_SCENE_LEAVING ||
        behavior->scene_phase == H2_STARBOY_SCENE_GONE) {
        return;
    }
    behavior->scene_phase = H2_STARBOY_SCENE_LEAVING;
    behavior->scene_transition_started_ms = now_ms;
    behavior->scene_scale_q15 = H2_STARBOY_Q15_ONE;
}

void h2_starboy_behavior_restart_entry(
    h2_starboy_behavior_t *behavior,
    uint64_t now_ms) {
    if (behavior == NULL) {
        return;
    }
    behavior->scene_phase = H2_STARBOY_SCENE_ENTERING;
    behavior->scene_transition_started_ms = now_ms;
    behavior->scene_scale_q15 = H2_STARBOY_SCENE_FAR_SCALE_Q15;
    behavior->scene_bob_q15 = 0;
}

int h2_starboy_behavior_exit_complete(
    const h2_starboy_behavior_t *behavior) {
    return behavior != NULL && behavior->scene_phase == H2_STARBOY_SCENE_GONE;
}

int32_t h2_starboy_normalize_coordinate(int32_t value, uint32_t extent) {
    if (extent <= 1u) {
        return 0;
    }
    if (value < 0) {
        value = 0;
    }
    if ((uint32_t)value >= extent) {
        value = (int32_t)(extent - 1u);
    }
    const int64_t numerator =
        (int64_t)value * (int64_t)(H2_STARBOY_Q15_ONE * 2);
    return (int32_t)(numerator / (int64_t)(extent - 1u)) -
           H2_STARBOY_Q15_ONE;
}

uint32_t h2_starboy_audio_level_s16(
    const int16_t *samples,
    size_t sample_count) {
    if (samples == NULL || sample_count == 0u) {
        return 0u;
    }
    uint64_t magnitude_sum = 0u;
    uint32_t peak_magnitude = 0u;
    for (size_t index = 0u; index < sample_count; ++index) {
        const int32_t sample = samples[index];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        magnitude_sum += magnitude;
        if (magnitude > peak_magnitude) {
            peak_magnitude = magnitude;
        }
    }
    const uint32_t mean_magnitude = (uint32_t)(magnitude_sum / sample_count);
    return mean_magnitude + (peak_magnitude - mean_magnitude) / 2u;
}

uint32_t h2_starboy_audio_envelope_step(
    uint32_t current_level,
    uint32_t frame_level,
    int frame_available) {
    if (!frame_available) {
        return current_level;
    }
    if (frame_level >= current_level) {
        return (uint32_t)(
            ((uint64_t)current_level + (uint64_t)frame_level * 3u) / 4u);
    }
    return (uint32_t)(
        ((uint64_t)current_level * 7u + frame_level) / 8u);
}

int h2_starboy_shake_tracker_accept(
    h2_starboy_shake_tracker_t *tracker,
    uint64_t event_at_ms,
    uint32_t duration_ms,
    uint32_t *out_streak_duration_ms) {
    if (tracker == NULL || out_streak_duration_ms == NULL ||
        duration_ms == 0u) {
        return 0;
    }
    if (tracker->seen && tracker->last_event_at_ms == event_at_ms) {
        return 0;
    }

    const int continues_streak = tracker->seen &&
        event_at_ms > tracker->last_event_at_ms &&
        event_at_ms - tracker->last_event_at_ms <=
            H2_STARBOY_SHAKE_STREAK_GAP_MS;
    if (!continues_streak) {
        tracker->streak_duration_ms = duration_ms;
    } else if (tracker->streak_duration_ms > UINT32_MAX - duration_ms) {
        tracker->streak_duration_ms = UINT32_MAX;
    } else {
        tracker->streak_duration_ms += duration_ms;
    }
    tracker->last_event_at_ms = event_at_ms;
    tracker->seen = 1;
    *out_streak_duration_ms = tracker->streak_duration_ms;
    return 1;
}

int h2_starboy_power_hold_update(
    h2_starboy_power_hold_tracker_t *tracker,
    int pressed,
    uint64_t now_ms) {
    if (tracker == NULL) {
        return 0;
    }
    if (!pressed) {
        *tracker = (h2_starboy_power_hold_tracker_t){0};
        return 0;
    }
    if (!tracker->pressed || now_ms < tracker->pressed_since_ms) {
        tracker->pressed = 1;
        tracker->pressed_since_ms = now_ms;
        tracker->triggered = 0;
        return 0;
    }
    if (tracker->triggered ||
        now_ms - tracker->pressed_since_ms < H2_STARBOY_POWER_HOLD_MS) {
        return 0;
    }
    tracker->triggered = 1;
    return 1;
}
