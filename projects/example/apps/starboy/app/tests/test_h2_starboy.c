#include "h2_starboy.h"
#include "h2_starboy_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_TOUCH_RETRY_MS 1000u
#define TEST_RGB565(red, green, blue) ((uint16_t)( \
    ((((uint16_t)(red)) & UINT16_C(0xf8)) << 8) | \
     ((((uint16_t)(green)) & UINT16_C(0xfc)) << 3) | \
     (((uint16_t)(blue)) >> 3)))

typedef struct test_state {
    uint64_t now_ms;
    unsigned allocation_count;
    unsigned display_open_count;
    unsigned display_close_count;
    unsigned draw_count;
    unsigned present_count;
    unsigned touch_open_count;
    unsigned touch_close_count;
    unsigned touch_event_index;
    unsigned audio_start_count;
    unsigned audio_stop_count;
    unsigned ready_count;
    unsigned stop_count;
    unsigned stop_after_sleeps;
    uint32_t sleep_extra_ms;
    h2_display_rect_t draw_rects[4];
    void *framebuffer_allocation;
    int fail_allocation;
    int touch_unavailable;
    int touch_fail_poll_once;
    int touch_poll_failed;
    int audio_unavailable;
    h2_pal_result_t ready_result;
} test_state_t;

static void test_set_palette(
    h2_starboy_behavior_t *behavior,
    uint16_t eye_color,
    uint16_t pupil_color) {
    behavior->palette = (h2_starboy_palette_t){eye_color, pupil_color};
    behavior->previous_palette = behavior->palette;
    behavior->palette_mix_q15 = H2_STARBOY_Q15_ONE;
}

static uint8_t test_rgb565_luminance(uint16_t color) {
    const uint32_t red = ((color >> 11) & 0x1fu) * 255u / 31u;
    const uint32_t green = ((color >> 5) & 0x3fu) * 255u / 63u;
    const uint32_t blue = (color & 0x1fu) * 255u / 31u;
    return (uint8_t)((red * 54u + green * 183u + blue * 19u) / 256u);
}

static void test_coordinate_normalization(void) {
    assert(h2_starboy_normalize_coordinate(-100, 368u) ==
           -H2_STARBOY_Q15_ONE);
    assert(h2_starboy_normalize_coordinate(0, 368u) ==
           -H2_STARBOY_Q15_ONE);
    assert(h2_starboy_normalize_coordinate(367, 368u) ==
           H2_STARBOY_Q15_ONE);
    assert(h2_starboy_normalize_coordinate(1000, 368u) ==
           H2_STARBOY_Q15_ONE);
    assert(h2_starboy_normalize_coordinate(17, 1u) == 0);
    const int32_t middle = h2_starboy_normalize_coordinate(184, 368u);
    assert(middle >= 0 && middle < 200);
}

static void test_smooth_pointer_and_idle_gaze(void) {
    h2_starboy_behavior_t first;
    h2_starboy_behavior_t second;
    h2_starboy_behavior_init(&first, 100u, 77u);
    h2_starboy_behavior_init(&second, 100u, 77u);
    assert(first.target_x_q15 == second.target_x_q15);
    assert(first.target_y_q15 == second.target_y_q15);

    const h2_starboy_behavior_input_t pointer = {
        .now_ms = 116u,
        .pointer_active = 1,
        .pointer_x_q15 = H2_STARBOY_Q15_ONE,
        .pointer_y_q15 = -H2_STARBOY_Q15_ONE,
    };
    h2_starboy_behavior_step(&first, &pointer);
    assert(first.gaze_x_q15 > H2_STARBOY_Q15_ONE / 4);
    assert(first.gaze_x_q15 < H2_STARBOY_Q15_ONE);
    assert(first.gaze_y_q15 < 0);
    assert(first.gaze_y_q15 > -H2_STARBOY_Q15_ONE);
    assert(first.head_x_q15 > 0);
    assert(first.head_x_q15 < first.gaze_x_q15);
    assert(first.tracking_q15 > 0);

    const int32_t first_x = first.gaze_x_q15;
    h2_starboy_behavior_input_t later = pointer;
    later.now_ms = 132u;
    h2_starboy_behavior_step(&first, &later);
    assert(first.gaze_x_q15 > first_x);
    assert(first.gaze_x_q15 < H2_STARBOY_Q15_ONE);

    later.now_ms = 180u;
    h2_starboy_behavior_step(&first, &later);
    assert(first.gaze_x_q15 > H2_STARBOY_Q15_ONE * 3 / 4);
    assert(first.head_x_q15 < first.gaze_x_q15);

    h2_starboy_behavior_input_t idle = {
        .now_ms = first.next_idle_scan_ms,
    };
    h2_starboy_behavior_step(&first, &idle);
    assert(first.next_idle_scan_ms > idle.now_ms);
}

static void test_natural_blink(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 4u);
    behavior.next_blink_ms = 200u;
    h2_starboy_behavior_input_t input = {.now_ms = 220u};
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.blink_start_ms == 220u);

    input.now_ms = 255u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.blink_q15 > 0);
    assert(behavior.blink_q15 < H2_STARBOY_Q15_ONE);

    input.now_ms = 290u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.blink_q15 > H2_STARBOY_Q15_ONE / 2);

    input.now_ms = 410u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.blink_q15 == 0);
    assert(behavior.next_blink_ms > input.now_ms);
}

static void test_idle_pose_changes_smoothly(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    const int32_t initial_tilt = behavior.pose_tilt_q15;
    const uint64_t next_pose_ms = behavior.next_idle_pose_ms;
    assert(next_pose_ms > 100u);

    h2_starboy_behavior_input_t input = {.now_ms = next_pose_ms};
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.next_idle_pose_ms > next_pose_ms);
    assert(behavior.pose_tilt_q15 != behavior.target_pose_tilt_q15 ||
           behavior.pose_squash_q15 != behavior.target_pose_squash_q15 ||
           behavior.pose_asymmetry_q15 !=
               behavior.target_pose_asymmetry_q15);
    assert(behavior.pose_tilt_q15 != initial_tilt ||
           behavior.pose_squash_q15 != 0 ||
           behavior.pose_asymmetry_q15 != 0);

    const int32_t first_tilt = behavior.pose_tilt_q15;
    input.now_ms += 16u;
    h2_starboy_behavior_step(&behavior, &input);
    if (first_tilt != behavior.target_pose_tilt_q15) {
        assert(behavior.pose_tilt_q15 != first_tilt);
    }
}

static void test_audio_hysteresis_and_cooldown(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 8u);
    h2_starboy_behavior_input_t input = {
        .now_ms = 100u,
        .audio_level = 4500u,
    };
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_CALM);
    input.now_ms = 300u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_ANXIOUS);
    assert(behavior.emotion_mix_q15[H2_STARBOY_EMOTION_CALM] > 0);
    assert(behavior.emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS] > 0);
    assert(behavior.emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS] <
           H2_STARBOY_Q15_ONE);

    input.audio_level = 2000u;
    input.now_ms = 400u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_ANXIOUS);
    input.now_ms = 1100u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_CALM);
    assert(behavior.emotion_mix_q15[H2_STARBOY_EMOTION_CALM] > 0);
    assert(behavior.emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS] > 0);

    input.audio_level = 4500u;
    input.now_ms = 1200u;
    h2_starboy_behavior_step(&behavior, &input);
    input.now_ms = 1450u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_CALM);
    input.now_ms = behavior.anxious_cooldown_until_ms + 200u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.emotion == H2_STARBOY_EMOTION_ANXIOUS);
}

static void test_shake_cycles_pupil_style_with_cooldown(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 11u);
    h2_starboy_behavior_set_initial_pupil_style(
        &behavior, H2_STARBOY_PUPIL_STYLE_DOT);
    h2_starboy_behavior_input_t input = {
        .now_ms = 200u,
        .shake_valid = 1,
        .shake_magnitude_mg = 1400,
        .shake_duration_ms = 180u,
    };
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.pupil_style == H2_STARBOY_PUPIL_STYLE_CIRCLE);
    assert(behavior.previous_pupil_style == H2_STARBOY_PUPIL_STYLE_DOT);
    assert(behavior.emotion == H2_STARBOY_EMOTION_CALM);

    input.now_ms = 600u;
    input.shake_magnitude_mg = 3000;
    input.shake_duration_ms = 700u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.pupil_style == H2_STARBOY_PUPIL_STYLE_CIRCLE);

    input.now_ms = behavior.pupil_change_cooldown_until_ms;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.pupil_style == H2_STARBOY_PUPIL_STYLE_CAT);
    assert(behavior.pupil_style_mix_q15 == 0);
}

static void test_audio_emotion_preserves_palette_colors(void) {
    enum { WIDTH = 100, HEIGHT = 120 };
    uint16_t pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t transition;
    h2_starboy_behavior_init(&transition, 100u, 884u);
    h2_starboy_behavior_input_t input = {
        .now_ms = 100u,
        .audio_level = 9000u,
    };
    h2_starboy_behavior_step(&transition, &input);
    input.now_ms = 300u;
    h2_starboy_behavior_step(&transition, &input);
    for (size_t frame = 0u; frame < 240u; ++frame) {
        input.now_ms += 16u;
        h2_starboy_behavior_step(&transition, &input);
    }
    assert(transition.emotion_mix_q15[H2_STARBOY_EMOTION_CALM] == 0);
    assert(transition.emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS] ==
           H2_STARBOY_Q15_ONE);

    h2_display_rect_t content_rect = {0};
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &transition, NULL, &content_rect);

    const uint16_t eye_color = transition.palette.eye_color;
    const uint16_t pupil_color = transition.palette.pupil_color;
    size_t eye_pixel_count = 0u;
    size_t pupil_pixel_count = 0u;
    for (size_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        eye_pixel_count += pixels[index] == eye_color;
        pupil_pixel_count += pixels[index] == pupil_color;
    }
    assert(eye_pixel_count > 0u);
    assert(pupil_pixel_count > 0u);
}

static uint64_t test_pixel_hash(
    const uint16_t *pixels,
    size_t pixel_count) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0u; index < pixel_count; ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void test_random_palette_transition_and_contrast(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_t repeated;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    h2_starboy_behavior_init(&repeated, 100u, 884u);
    assert(behavior.palette.eye_color == repeated.palette.eye_color);
    assert(behavior.palette.pupil_color == repeated.palette.pupil_color);
    assert(behavior.palette_mix_q15 == H2_STARBOY_Q15_ONE);

    const h2_starboy_palette_t initial = behavior.palette;
    h2_starboy_behavior_randomize_palette(&behavior, 200u);
    assert(behavior.previous_palette.eye_color == initial.eye_color);
    assert(behavior.previous_palette.pupil_color == initial.pupil_color);
    assert(behavior.palette.eye_color != initial.eye_color);
    assert(behavior.palette.pupil_color != initial.pupil_color);
    assert(behavior.palette_mix_q15 == 0);
    h2_starboy_behavior_input_t input = {.now_ms = 320u};
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.palette_mix_q15 > H2_STARBOY_Q15_ONE * 2 / 5);
    assert(behavior.palette_mix_q15 < H2_STARBOY_Q15_ONE * 3 / 5);
    input.now_ms = 500u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.palette_mix_q15 == H2_STARBOY_Q15_ONE);
    assert(behavior.previous_palette.eye_color == behavior.palette.eye_color);
    assert(behavior.previous_palette.pupil_color ==
           behavior.palette.pupil_color);

    enum { WIDTH = 100, HEIGHT = 120 };
    uint16_t pixels[WIDTH * HEIGHT];
    enum { PALETTE_SAMPLE_COUNT = 64 };
    uint64_t hashes[PALETTE_SAMPLE_COUNT] = {0};
    for (size_t sample = 0u; sample < PALETTE_SAMPLE_COUNT; ++sample) {
        h2_starboy_behavior_randomize_palette(
            &behavior, 600u + sample * 300u);
        behavior.previous_palette = behavior.palette;
        behavior.palette_mix_q15 = H2_STARBOY_Q15_ONE;
        const uint8_t eye_luminance =
            test_rgb565_luminance(behavior.palette.eye_color);
        const uint8_t pupil_luminance =
            test_rgb565_luminance(behavior.palette.pupil_color);
        assert(eye_luminance >= 145u);
        assert(pupil_luminance <= 76u);
        assert(eye_luminance - pupil_luminance >= 75u);
        h2_display_rect_t content_rect = {0};
        (void)h2_starboy_render(
            pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
        hashes[sample] = test_pixel_hash(pixels, WIDTH * HEIGHT);
    }
    for (size_t first = 0u;
         first < PALETTE_SAMPLE_COUNT;
         ++first) {
        for (size_t second = first + 1u;
             second < PALETTE_SAMPLE_COUNT;
             ++second) {
            assert(hashes[first] != hashes[second]);
        }
    }
}

static void test_four_pupil_styles_render_distinctly(void) {
    enum { WIDTH = 120, HEIGHT = 140 };
    uint16_t pixels[WIDTH * HEIGHT];
    uint64_t hashes[H2_STARBOY_PUPIL_STYLE_COUNT] = {0};
    size_t pupil_counts[H2_STARBOY_PUPIL_STYLE_COUNT] = {0};
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    const uint16_t pupil_color = behavior.palette.pupil_color;
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    behavior.target_x_q15 = 0;
    behavior.target_y_q15 = 0;
    behavior.gaze_x_q15 = 0;
    behavior.gaze_y_q15 = 0;
    behavior.head_x_q15 = 0;
    behavior.head_y_q15 = 0;
    behavior.pose_tilt_q15 = 0;
    behavior.pose_squash_q15 = 0;
    behavior.pose_asymmetry_q15 = 0;
    for (size_t pupil_style = 0u;
         pupil_style < H2_STARBOY_PUPIL_STYLE_COUNT;
         ++pupil_style) {
        h2_starboy_behavior_set_initial_pupil_style(
            &behavior, (h2_starboy_pupil_style_t)pupil_style);
        h2_display_rect_t content_rect = {0};
        (void)h2_starboy_render(
            pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
        hashes[pupil_style] = test_pixel_hash(pixels, WIDTH * HEIGHT);
        for (size_t index = 0u; index < WIDTH * HEIGHT; ++index) {
            pupil_counts[pupil_style] +=
                pixels[index] == pupil_color;
        }
    }
    for (size_t first = 0u;
         first < H2_STARBOY_PUPIL_STYLE_COUNT;
         ++first) {
        for (size_t second = first + 1u;
             second < H2_STARBOY_PUPIL_STYLE_COUNT;
             ++second) {
            assert(hashes[first] != hashes[second]);
        }
    }
    assert(pupil_counts[H2_STARBOY_PUPIL_STYLE_DOT] <
           pupil_counts[H2_STARBOY_PUPIL_STYLE_CIRCLE]);
    assert(pupil_counts[H2_STARBOY_PUPIL_STYLE_CAT] <
           pupil_counts[H2_STARBOY_PUPIL_STYLE_ACORN]);
}

static void test_acorn_matches_reference_proportions_and_clipping(void) {
    enum { WIDTH = 240, HEIGHT = 280 };
    uint16_t pixels[WIDTH * HEIGHT];
    const uint16_t eye_color = TEST_RGB565(255, 160, 0);
    const uint16_t pupil_color = TEST_RGB565(48, 24, 64);
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    behavior.target_x_q15 = 0;
    behavior.target_y_q15 = 0;
    behavior.gaze_x_q15 = 0;
    behavior.gaze_y_q15 = 0;
    behavior.head_x_q15 = 0;
    behavior.head_y_q15 = 0;
    behavior.pose_tilt_q15 = 0;
    behavior.pose_squash_q15 = 0;
    behavior.pose_asymmetry_q15 = 0;
    h2_starboy_behavior_set_initial_pupil_style(
        &behavior, H2_STARBOY_PUPIL_STYLE_ACORN);
    test_set_palette(&behavior, eye_color, pupil_color);
    h2_display_rect_t content_rect = {0};
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);

    for (size_t eye_index = 0u; eye_index < 2u; ++eye_index) {
        const size_t first_x = eye_index == 0u ? 0u : WIDTH / 2u;
        const size_t last_x = eye_index == 0u ? WIDTH / 2u : WIDTH;
        size_t eye_min_x = WIDTH;
        size_t eye_max_x = 0u;
        size_t eye_min_y = HEIGHT;
        size_t eye_max_y = 0u;
        size_t pupil_min_x = WIDTH;
        size_t pupil_max_x = 0u;
        size_t pupil_min_y = HEIGHT;
        size_t pupil_max_y = 0u;
        uint64_t pupil_y_sum = 0u;
        size_t pupil_count = 0u;
        for (size_t y = 0u; y < HEIGHT; ++y) {
            for (size_t x = first_x; x < last_x; ++x) {
                const uint16_t color = pixels[y * WIDTH + x];
                if (color != 0u) {
                    eye_min_x = x < eye_min_x ? x : eye_min_x;
                    eye_max_x = x > eye_max_x ? x : eye_max_x;
                    eye_min_y = y < eye_min_y ? y : eye_min_y;
                    eye_max_y = y > eye_max_y ? y : eye_max_y;
                }
                if (color == pupil_color) {
                    pupil_min_x = x < pupil_min_x ? x : pupil_min_x;
                    pupil_max_x = x > pupil_max_x ? x : pupil_max_x;
                    pupil_min_y = y < pupil_min_y ? y : pupil_min_y;
                    pupil_max_y = y > pupil_max_y ? y : pupil_max_y;
                    pupil_y_sum += y;
                    ++pupil_count;
                }
            }
        }
        const size_t eye_width = eye_max_x - eye_min_x + 1u;
        const size_t eye_height = eye_max_y - eye_min_y + 1u;
        const size_t pupil_width = pupil_max_x - pupil_min_x + 1u;
        const size_t pupil_height = pupil_max_y - pupil_min_y + 1u;
        assert(pupil_width * 100u >= eye_width * 44u);
        assert(pupil_width * 100u <= eye_width * 56u);
        assert(pupil_height * 100u >= eye_height * 76u);
        assert(pupil_height * 100u <= eye_height * 82u);
        assert(pupil_count > 0u);
        assert(pupil_y_sum / pupil_count >
               (eye_min_y + eye_max_y) / 2u + eye_height / 12u);
        assert(pupil_min_y > eye_min_y + eye_height / 6u);
        assert(pupil_max_y + 1u >= eye_max_y);
        if (eye_index == 0u) {
            assert(pupil_min_x + pupil_max_x > eye_min_x + eye_max_x);
        } else {
            assert(pupil_min_x + pupil_max_x < eye_min_x + eye_max_x);
        }
    }
}

static void test_default_eyes_are_subtly_oval_and_wing_tilted(void) {
    enum { WIDTH = 240, HEIGHT = 280 };
    uint16_t pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    behavior.target_x_q15 = 0;
    behavior.target_y_q15 = 0;
    behavior.gaze_x_q15 = 0;
    behavior.gaze_y_q15 = 0;
    behavior.head_x_q15 = 0;
    behavior.head_y_q15 = 0;
    behavior.pose_tilt_q15 = 0;
    behavior.pose_squash_q15 = 0;
    behavior.pose_asymmetry_q15 = 0;
    h2_starboy_behavior_set_initial_pupil_style(
        &behavior, H2_STARBOY_PUPIL_STYLE_DOT);
    test_set_palette(
        &behavior,
        TEST_RGB565(224, 160, 64),
        TEST_RGB565(224, 160, 64));

    h2_display_rect_t content_rect = {0};
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);

    size_t eye_heights[2] = {0u, 0u};
    for (size_t eye_index = 0u; eye_index < 2u; ++eye_index) {
        const size_t half_min_x = eye_index == 0u ? 0u : WIDTH / 2u;
        const size_t half_max_x = eye_index == 0u ? WIDTH / 2u : WIDTH;
        size_t min_x = WIDTH;
        size_t max_x = 0u;
        size_t min_y = HEIGHT;
        size_t max_y = 0u;
        for (size_t y = 0u; y < HEIGHT; ++y) {
            for (size_t x = half_min_x; x < half_max_x; ++x) {
                if (pixels[y * WIDTH + x] == 0u) {
                    continue;
                }
                min_x = x < min_x ? x : min_x;
                max_x = x > max_x ? x : max_x;
                min_y = y < min_y ? y : min_y;
                max_y = y > max_y ? y : max_y;
            }
        }
        const size_t eye_width = max_x - min_x + 1u;
        const size_t eye_height = max_y - min_y + 1u;
        eye_heights[eye_index] = eye_height;
        assert(eye_height * 100u >= eye_width * 112u);
        assert(eye_height * 100u <= eye_width * 126u);

        const size_t center_x = (min_x + max_x) / 2u;
        uint64_t outer_y_sum = 0u;
        uint64_t inner_y_sum = 0u;
        size_t outer_count = 0u;
        size_t inner_count = 0u;
        for (size_t y = min_y; y <= max_y; ++y) {
            for (size_t x = min_x; x <= max_x; ++x) {
                if (pixels[y * WIDTH + x] == 0u || x == center_x) {
                    continue;
                }
                const int outer = eye_index == 0u
                    ? x < center_x
                    : x > center_x;
                if (outer) {
                    outer_y_sum += y;
                    ++outer_count;
                } else {
                    inner_y_sum += y;
                    ++inner_count;
                }
            }
        }
        assert(outer_count > 0u && inner_count > 0u);
        assert(outer_y_sum * inner_count < inner_y_sum * outer_count);
    }
    assert(eye_heights[0] <= eye_heights[1] + 2u);
    assert(eye_heights[1] <= eye_heights[0] + 2u);

    behavior.pose_squash_q15 = -1800;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    for (size_t eye_index = 0u; eye_index < 2u; ++eye_index) {
        const size_t half_min_x = eye_index == 0u ? 0u : WIDTH / 2u;
        const size_t half_max_x = eye_index == 0u ? WIDTH / 2u : WIDTH;
        size_t min_x = WIDTH;
        size_t max_x = 0u;
        size_t min_y = HEIGHT;
        size_t max_y = 0u;
        for (size_t y = 0u; y < HEIGHT; ++y) {
            for (size_t x = half_min_x; x < half_max_x; ++x) {
                if (pixels[y * WIDTH + x] == 0u) {
                    continue;
                }
                min_x = x < min_x ? x : min_x;
                max_x = x > max_x ? x : max_x;
                min_y = y < min_y ? y : min_y;
                max_y = y > max_y ? y : max_y;
            }
        }
        assert(max_y - min_y > max_x - min_x);
    }
}

static void test_idle_wing_flutter_is_subtle_and_pointer_cancelled(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    assert(behavior.next_wing_flutter_ms > 100u);

    h2_starboy_behavior_input_t input = {
        .now_ms = behavior.next_wing_flutter_ms,
    };
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.wing_flutter_started_ms == input.now_ms);

    input.now_ms += 200u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.wing_flutter_q15 > 0);
    assert(behavior.wing_flutter_q15 < 1500);

    const int32_t flutter_before_pointer = behavior.wing_flutter_q15;
    input.now_ms += 32u;
    input.pointer_active = 1;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.wing_flutter_started_ms == 0u);
    assert(behavior.wing_flutter_q15 < flutter_before_pointer);
}

static void test_scene_entry_and_exit_are_time_based(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    const int32_t far_scale = behavior.scene_scale_q15;
    assert(behavior.scene_phase == H2_STARBOY_SCENE_ENTERING);
    assert(far_scale > 0 && far_scale < H2_STARBOY_Q15_ONE / 4);

    h2_starboy_behavior_input_t input = {.now_ms = 600u};
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.scene_scale_q15 > far_scale);
    assert(behavior.scene_scale_q15 < H2_STARBOY_Q15_ONE);
    input.now_ms = 1100u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.scene_phase == H2_STARBOY_SCENE_PRESENT);
    assert(behavior.scene_scale_q15 == H2_STARBOY_Q15_ONE);

    h2_starboy_behavior_begin_exit(&behavior, 1200u);
    input.now_ms = 1650u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.scene_phase == H2_STARBOY_SCENE_LEAVING);
    assert(behavior.scene_scale_q15 > 0);
    assert(behavior.scene_scale_q15 < H2_STARBOY_Q15_ONE);
    input.now_ms = 2100u;
    h2_starboy_behavior_step(&behavior, &input);
    assert(h2_starboy_behavior_exit_complete(&behavior));
    assert(behavior.scene_scale_q15 == 0);
}

static void test_shake_streak_accumulation(void) {
    h2_starboy_shake_tracker_t tracker = {0};
    uint32_t streak_duration_ms = 0u;
    assert(h2_starboy_shake_tracker_accept(
        &tracker, 1000u, 120u, &streak_duration_ms));
    assert(streak_duration_ms == 120u);
    assert(!h2_starboy_shake_tracker_accept(
        &tracker, 1000u, 120u, &streak_duration_ms));
    assert(h2_starboy_shake_tracker_accept(
        &tracker, 1150u, 120u, &streak_duration_ms));
    assert(streak_duration_ms == 240u);
    assert(h2_starboy_shake_tracker_accept(
        &tracker, 1300u, 320u, &streak_duration_ms));
    assert(streak_duration_ms == 560u);
    assert(h2_starboy_shake_tracker_accept(
        &tracker, 1800u, 120u, &streak_duration_ms));
    assert(streak_duration_ms == 120u);
    assert(!h2_starboy_shake_tracker_accept(
        NULL, 1900u, 120u, &streak_duration_ms));
    assert(!h2_starboy_shake_tracker_accept(
        &tracker, 1900u, 120u, NULL));
}

static void test_two_second_power_hold(void) {
    h2_starboy_power_hold_tracker_t tracker = {0};
    assert(!h2_starboy_power_hold_update(&tracker, 1, 100u));
    assert(!h2_starboy_power_hold_update(&tracker, 1, 2099u));
    assert(h2_starboy_power_hold_update(&tracker, 1, 2100u));
    assert(!h2_starboy_power_hold_update(&tracker, 1, 4000u));
    assert(!h2_starboy_power_hold_update(&tracker, 0, 4100u));
    assert(!h2_starboy_power_hold_update(&tracker, 1, 4200u));
    assert(h2_starboy_power_hold_update(&tracker, 1, 6200u));
    assert(!h2_starboy_power_hold_update(NULL, 1, 7000u));
}

static void test_gravity_orientation_eases_and_freezes(void) {
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    assert(behavior.gravity_down_x_q15 == 0);
    assert(behavior.gravity_down_y_q15 == H2_STARBOY_Q15_ONE);

    h2_starboy_behavior_input_t input = {
        .now_ms = 116u,
        .gravity_valid = 1,
        .gravity_x_mg = -1000,
    };
    h2_starboy_behavior_step(&behavior, &input);
    const int32_t first_x = behavior.gravity_down_x_q15;
    const int32_t first_y = behavior.gravity_down_y_q15;
    assert(first_x > 0 && first_x < H2_STARBOY_Q15_ONE);
    assert(first_y > 0);

    input.now_ms += 16u;
    h2_starboy_behavior_step(&behavior, &input);
    const int32_t second_increment = behavior.gravity_down_x_q15 - first_x;
    assert(second_increment > 0);
    for (size_t frame = 0u; frame < 32u; ++frame) {
        input.now_ms += 16u;
        h2_starboy_behavior_step(&behavior, &input);
    }
    assert(behavior.gravity_down_x_q15 > H2_STARBOY_Q15_ONE * 9 / 10);
    assert(behavior.gravity_down_y_q15 < H2_STARBOY_Q15_ONE / 4);

    const int32_t stable_x = behavior.gravity_down_x_q15;
    const int32_t stable_y = behavior.gravity_down_y_q15;
    input.now_ms += 16u;
    input.gravity_x_mg = -2500;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.gravity_down_x_q15 == stable_x);
    assert(behavior.gravity_down_y_q15 == stable_y);

    input.now_ms += 16u;
    input.gravity_x_mg = 0;
    input.gravity_z_mg = 1000;
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.gravity_down_x_q15 == stable_x);
    assert(behavior.gravity_down_y_q15 == stable_y);

    h2_starboy_behavior_init(&behavior, 100u, 884u);
    input = (h2_starboy_behavior_input_t){
        .now_ms = 116u,
        .gravity_valid = 1,
        .gravity_y_mg = 1000,
    };
    h2_starboy_behavior_step(&behavior, &input);
    assert(behavior.gravity_down_x_q15 > 0);
    assert(behavior.gravity_down_y_q15 > 0);
    for (size_t frame = 0u; frame < 48u; ++frame) {
        input.now_ms += 16u;
        h2_starboy_behavior_step(&behavior, &input);
    }
    assert(behavior.gravity_down_y_q15 < -H2_STARBOY_Q15_ONE * 9 / 10);
}

static void test_gravity_rotates_complete_eye_geometry(void) {
    enum { WIDTH = 160, HEIGHT = 160 };
    uint16_t pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 884u);
    h2_display_rect_t content_rect = {0};
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    const uint64_t upright_hash = test_pixel_hash(pixels, WIDTH * HEIGHT);

    behavior.gravity_down_x_q15 = H2_STARBOY_Q15_ONE;
    behavior.gravity_down_y_q15 = 0;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    const uint64_t sideways_hash = test_pixel_hash(pixels, WIDTH * HEIGHT);
    assert(sideways_hash != upright_hash);
    assert(content_rect.width > 0 && content_rect.height > 0);
    assert(content_rect.x >= 0 && content_rect.y >= 0);
    assert(content_rect.x + content_rect.width <= WIDTH);
    assert(content_rect.y + content_rect.height <= HEIGHT);

}

static void test_render_centroid(
    const uint16_t *pixels,
    size_t width,
    size_t height,
    uint16_t required_color,
    uint64_t *out_x,
    uint64_t *out_y) {
    uint64_t sum_x = 0u;
    uint64_t sum_y = 0u;
    uint64_t count = 0u;
    for (size_t y = 0u; y < height; ++y) {
        for (size_t x = 0u; x < width; ++x) {
            const uint16_t color = pixels[y * width + x];
            if ((required_color == 0u && color == 0u) ||
                (required_color != 0u && color != required_color)) {
                continue;
            }
            sum_x += x;
            sum_y += y;
            ++count;
        }
    }
    assert(count > 0u);
    *out_x = sum_x / count;
    *out_y = sum_y / count;
}

static void test_audio_level_and_render(void) {
    const int16_t samples[] = {-1000, 1000, -3000, 3000};
    assert(h2_starboy_audio_level_s16(samples, 4u) == 2500u);
    assert(h2_starboy_audio_level_s16(NULL, 0u) == 0u);
    assert(h2_starboy_audio_envelope_step(0u, 8000u, 1) == 6000u);
    assert(h2_starboy_audio_envelope_step(6000u, 0u, 0) == 6000u);
    assert(h2_starboy_audio_envelope_step(6000u, 2000u, 1) == 5500u);

    enum { WIDTH = 100, HEIGHT = 120 };
    uint16_t pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 2u);
    const uint16_t pupil_color = behavior.palette.pupil_color;
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    behavior.target_x_q15 = 0;
    behavior.target_y_q15 = 0;
    behavior.gaze_x_q15 = 0;
    behavior.gaze_y_q15 = 0;
    behavior.head_x_q15 = 0;
    behavior.head_y_q15 = 0;
    behavior.pose_tilt_q15 = 0;
    behavior.pose_squash_q15 = 0;
    behavior.pose_asymmetry_q15 = 0;
    h2_display_rect_t content_rect = {0};
    const h2_display_rect_t dirty_rect = h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    assert(dirty_rect.x == 0 && dirty_rect.y == 0);
    assert(dirty_rect.width == WIDTH && dirty_rect.height == HEIGHT);
    assert(content_rect.x >= 0 && content_rect.y >= 0);
    assert(content_rect.width > 0 && content_rect.height > 0);
    assert(content_rect.x + content_rect.width <= WIDTH);
    assert(content_rect.y + content_rect.height <= HEIGHT);
    assert(pixels[0] == 0u);
    assert(pixels[WIDTH - 1u] == 0u);
    assert(pixels[(HEIGHT - 1u) * WIDTH] == 0u);
    size_t visible_pixels = 0u;
    for (size_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        if (pixels[index] != 0u) {
            const size_t x = index % WIDTH;
            const size_t y = index / WIDTH;
            assert(x >= (size_t)content_rect.x);
            assert(x < (size_t)(content_rect.x + content_rect.width));
            assert(y >= (size_t)content_rect.y);
            assert(y < (size_t)(content_rect.y + content_rect.height));
            ++visible_pixels;
        }
    }
    assert(visible_pixels > WIDTH * HEIGHT / 10u);

    uint16_t unique_colors[16] = {0};
    size_t unique_color_count = 0u;
    for (size_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        size_t color_index = 0u;
        while (color_index < unique_color_count &&
               unique_colors[color_index] != pixels[index]) {
            ++color_index;
        }
        if (color_index == unique_color_count &&
            unique_color_count < 16u) {
            unique_colors[unique_color_count++] = pixels[index];
        }
    }
    /* Black, two solid colors, and blended edge coverage must be present. */
    assert(unique_color_count > 3u);

    uint64_t left_pupil_x_sum = 0u;
    uint64_t right_pupil_x_sum = 0u;
    size_t left_pupil_count = 0u;
    size_t right_pupil_count = 0u;
    for (size_t y = 0u; y < HEIGHT; ++y) {
        assert(pixels[y * WIDTH + WIDTH / 2u] == 0u);
        for (size_t x = 0u; x < WIDTH; ++x) {
            if (pixels[y * WIDTH + x] != pupil_color) {
                continue;
            }
            if (x < WIDTH / 2u) {
                left_pupil_x_sum += x;
                ++left_pupil_count;
            } else {
                right_pupil_x_sum += x;
                ++right_pupil_count;
            }
        }
    }
    assert(left_pupil_count > 0u && right_pupil_count > 0u);
    assert(left_pupil_x_sum / left_pupil_count >= WIDTH * 31u / 100u);
    assert(right_pupil_x_sum / right_pupil_count <= WIDTH * 69u / 100u);

    uint16_t neutral_pixels[WIDTH * HEIGHT];
    memcpy(neutral_pixels, pixels, sizeof(pixels));
    behavior.pose_tilt_q15 = 9000;
    behavior.pose_asymmetry_q15 = 3500;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    assert(memcmp(neutral_pixels, pixels, sizeof(pixels)) != 0);

    behavior = (h2_starboy_behavior_t){0};
    test_set_palette(
        &behavior,
        TEST_RGB565(32, 231, 157),
        TEST_RGB565(1, 36, 17));
    behavior.head_x_q15 = H2_STARBOY_Q15_ONE;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    size_t left_eye_pixels = 0u;
    size_t right_eye_pixels = 0u;
    for (size_t y = 0u; y < HEIGHT; ++y) {
        for (size_t x = 0u; x < WIDTH; ++x) {
            if (pixels[y * WIDTH + x] == 0u) {
                continue;
            }
            /* Positive yaw brings the left sphere anchor toward the viewer. */
            if (x < WIDTH * 58u / 100u) {
                ++left_eye_pixels;
            } else {
                ++right_eye_pixels;
            }
        }
    }
    assert(left_eye_pixels > right_eye_pixels);
    assert(left_eye_pixels * 10u < right_eye_pixels * 16u);

    behavior = (h2_starboy_behavior_t){
        .gaze_x_q15 = -H2_STARBOY_Q15_ONE,
        .gaze_y_q15 = -H2_STARBOY_Q15_ONE,
        .head_x_q15 = -H2_STARBOY_Q15_ONE,
        .head_y_q15 = -H2_STARBOY_Q15_ONE,
        .tracking_q15 = H2_STARBOY_Q15_ONE,
        .palette = {
            TEST_RGB565(32, 231, 157),
            TEST_RGB565(1, 36, 17),
        },
        .previous_palette = {
            TEST_RGB565(32, 231, 157),
            TEST_RGB565(1, 36, 17),
        },
        .palette_mix_q15 = H2_STARBOY_Q15_ONE,
        .pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN,
        .previous_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN,
        .pupil_style_mix_q15 = H2_STARBOY_Q15_ONE,
    };
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    uint64_t upper_left_eye_x = 0u;
    uint64_t upper_left_eye_y = 0u;
    uint64_t upper_left_pupil_x = 0u;
    uint64_t upper_left_pupil_y = 0u;
    test_render_centroid(
        pixels,
        WIDTH,
        HEIGHT,
        0u,
        &upper_left_eye_x,
        &upper_left_eye_y);
    test_render_centroid(
        pixels,
        WIDTH,
        HEIGHT,
        TEST_RGB565(1, 36, 17),
        &upper_left_pupil_x,
        &upper_left_pupil_y);

    behavior.gaze_x_q15 = H2_STARBOY_Q15_ONE;
    behavior.gaze_y_q15 = H2_STARBOY_Q15_ONE;
    behavior.head_x_q15 = H2_STARBOY_Q15_ONE;
    behavior.head_y_q15 = H2_STARBOY_Q15_ONE;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    uint64_t lower_right_eye_x = 0u;
    uint64_t lower_right_eye_y = 0u;
    uint64_t lower_right_pupil_x = 0u;
    uint64_t lower_right_pupil_y = 0u;
    test_render_centroid(
        pixels,
        WIDTH,
        HEIGHT,
        0u,
        &lower_right_eye_x,
        &lower_right_eye_y);
    test_render_centroid(
        pixels,
        WIDTH,
        HEIGHT,
        TEST_RGB565(1, 36, 17),
        &lower_right_pupil_x,
        &lower_right_pupil_y);
    assert(lower_right_eye_x >= upper_left_eye_x + WIDTH / 12u);
    assert(lower_right_eye_y > upper_left_eye_y + HEIGHT / 14u);
    assert(lower_right_pupil_x > upper_left_pupil_x + WIDTH / 12u);
    assert(lower_right_pupil_y > upper_left_pupil_y + HEIGHT / 14u);

    behavior.blink_q15 = H2_STARBOY_Q15_ONE;
    (void)h2_starboy_render(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    assert(pixels[0] == 0u);
}

static size_t test_count_visible_pixels(
    const uint16_t *pixels,
    size_t pixel_count) {
    size_t visible_count = 0u;
    for (size_t index = 0u; index < pixel_count; ++index) {
        visible_count += pixels[index] != 0u;
    }
    return visible_count;
}

static void test_anxious_rounded_star_rotates(void) {
    enum { WIDTH = 100, HEIGHT = 120 };
    uint16_t calm_pixels[WIDTH * HEIGHT];
    uint16_t first_star_pixels[WIDTH * HEIGHT];
    uint16_t rotated_star_pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 22u);
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    h2_display_rect_t content_rect = {0};
    (void)h2_starboy_render(
        calm_pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);

    behavior.emotion = H2_STARBOY_EMOTION_ANXIOUS;
    behavior.emotion_mix_q15[H2_STARBOY_EMOTION_CALM] = 0;
    behavior.emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS] =
        H2_STARBOY_Q15_ONE;
    behavior.last_update_ms = 300u;
    (void)h2_starboy_render(
        first_star_pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);
    behavior.last_update_ms = 600u;
    (void)h2_starboy_render(
        rotated_star_pixels, WIDTH, HEIGHT, &behavior, NULL, &content_rect);

    const size_t calm_visible = test_count_visible_pixels(
        calm_pixels, WIDTH * HEIGHT);
    const size_t star_visible = test_count_visible_pixels(
        first_star_pixels, WIDTH * HEIGHT);
    assert(star_visible > WIDTH * HEIGHT / 20u);
    assert(star_visible < calm_visible);
    size_t changed_pixels = 0u;
    for (size_t index = 0u; index < WIDTH * HEIGHT; ++index) {
        changed_pixels +=
            first_star_pixels[index] != rotated_star_pixels[index];
    }
    assert(changed_pixels > WIDTH);
}

static void test_rotated_eyes_use_separate_dirty_regions(void) {
    enum { WIDTH = 368, HEIGHT = 448 };
    uint16_t *pixels = malloc(
        (size_t)WIDTH * HEIGHT * sizeof(*pixels));
    assert(pixels != NULL);
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 31u);
    behavior.scene_phase = H2_STARBOY_SCENE_PRESENT;
    behavior.scene_scale_q15 = H2_STARBOY_Q15_ONE;
    behavior.gravity_down_x_q15 = H2_STARBOY_Q15_ONE;
    behavior.gravity_down_y_q15 = 0;

    h2_starboy_render_regions_t first = {0};
    h2_starboy_render_dirty_regions(
        pixels, WIDTH, HEIGHT, &behavior, NULL, &first);
    assert(first.dirty_rect_count == 1u);
    assert(first.dirty_rects[0].width == WIDTH);
    assert(first.dirty_rects[0].height == HEIGHT);

    h2_starboy_render_regions_t second = {0};
    h2_starboy_render_dirty_regions(
        pixels, WIDTH, HEIGHT, &behavior, &first, &second);
    assert(second.dirty_rect_count == H2_STARBOY_EYE_COUNT);
    const uint64_t separate_area =
        (uint64_t)second.dirty_rects[0].width *
            (uint64_t)second.dirty_rects[0].height +
        (uint64_t)second.dirty_rects[1].width *
            (uint64_t)second.dirty_rects[1].height;
    const uint64_t aggregate_area =
        (uint64_t)second.content_rect.width *
        (uint64_t)second.content_rect.height;
    assert(separate_area < aggregate_area);
    free(pixels);
}

static void test_incremental_render_clears_previous_content(void) {
    enum { WIDTH = 100, HEIGHT = 120 };
    uint16_t pixels[WIDTH * HEIGHT];
    uint16_t preview_pixels[WIDTH * HEIGHT];
    h2_starboy_behavior_t behavior;
    h2_starboy_behavior_init(&behavior, 100u, 2u);
    behavior.head_x_q15 = -H2_STARBOY_Q15_ONE;
    behavior.gaze_x_q15 = -H2_STARBOY_Q15_ONE;

    h2_display_rect_t previous_content_rect = {0};
    (void)h2_starboy_render(
        pixels,
        WIDTH,
        HEIGHT,
        &behavior,
        NULL,
        &previous_content_rect);
    behavior.head_x_q15 = H2_STARBOY_Q15_ONE;
    behavior.gaze_x_q15 = H2_STARBOY_Q15_ONE;
    h2_display_rect_t preview_content_rect = {0};
    (void)h2_starboy_render(
        preview_pixels,
        WIDTH,
        HEIGHT,
        &behavior,
        NULL,
        &preview_content_rect);

    const int previous_x2 =
        previous_content_rect.x + previous_content_rect.width;
    const int previous_y2 =
        previous_content_rect.y + previous_content_rect.height;
    const int preview_x2 =
        preview_content_rect.x + preview_content_rect.width;
    const int preview_y2 =
        preview_content_rect.y + preview_content_rect.height;
    size_t poisoned_pixels = 0u;
    for (size_t y = 0u; y < HEIGHT; ++y) {
        for (size_t x = 0u; x < WIDTH; ++x) {
            const int inside_previous =
                x >= (size_t)previous_content_rect.x &&
                x < (size_t)previous_x2 &&
                y >= (size_t)previous_content_rect.y &&
                y < (size_t)previous_y2;
            const int inside_preview =
                x >= (size_t)preview_content_rect.x &&
                x < (size_t)preview_x2 &&
                y >= (size_t)preview_content_rect.y &&
                y < (size_t)preview_y2;
            if (inside_previous && !inside_preview) {
                pixels[y * WIDTH + x] = UINT16_MAX;
                ++poisoned_pixels;
            }
        }
    }
    assert(poisoned_pixels > 0u);

    h2_display_rect_t content_rect = {0};
    const h2_display_rect_t dirty_rect = h2_starboy_render(
        pixels,
        WIDTH,
        HEIGHT,
        &behavior,
        &previous_content_rect,
        &content_rect);
    assert(content_rect.x == preview_content_rect.x);
    assert(content_rect.y == preview_content_rect.y);
    assert(content_rect.width == preview_content_rect.width);
    assert(content_rect.height == preview_content_rect.height);

    const int expected_x = previous_content_rect.x < content_rect.x
        ? previous_content_rect.x
        : content_rect.x;
    const int expected_y = previous_content_rect.y < content_rect.y
        ? previous_content_rect.y
        : content_rect.y;
    const int content_x2 = content_rect.x + content_rect.width;
    const int content_y2 = content_rect.y + content_rect.height;
    const int expected_x2 = previous_x2 > content_x2
        ? previous_x2
        : content_x2;
    const int expected_y2 = previous_y2 > content_y2
        ? previous_y2
        : content_y2;
    assert(dirty_rect.x == expected_x);
    assert(dirty_rect.y == expected_y);
    assert(dirty_rect.width == expected_x2 - expected_x);
    assert(dirty_rect.height == expected_y2 - expected_y);

    size_t cleared_pixels = 0u;
    for (size_t y = 0u; y < HEIGHT; ++y) {
        for (size_t x = 0u; x < WIDTH; ++x) {
            const int inside_previous =
                x >= (size_t)previous_content_rect.x &&
                x < (size_t)previous_x2 &&
                y >= (size_t)previous_content_rect.y &&
                y < (size_t)previous_y2;
            const int inside_current =
                x >= (size_t)content_rect.x &&
                x < (size_t)content_x2 &&
                y >= (size_t)content_rect.y &&
                y < (size_t)content_y2;
            if (inside_previous && !inside_current) {
                assert(pixels[y * WIDTH + x] == 0u);
                ++cleared_pixels;
            }
        }
    }
    assert(cleared_pixels == poisoned_pixels);
}

static void *test_alloc(void *user, size_t size) {
    test_state_t *state = (test_state_t *)user;
    if (state->fail_allocation) {
        return NULL;
    }
    void *allocation = malloc(size);
    if (allocation != NULL) {
        if (state->allocation_count == 0u) {
            state->framebuffer_allocation = allocation;
        }
        ++state->allocation_count;
    }
    return allocation;
}

static void *test_realloc(void *user, void *pointer, size_t size) {
    (void)user;
    return realloc(pointer, size);
}

static void test_free(void *user, void *pointer) {
    test_state_t *state = (test_state_t *)user;
    assert(state->allocation_count > 0u);
    --state->allocation_count;
    free(pointer);
}

static int test_display_open(void *user) {
    ++((test_state_t *)user)->display_open_count;
    return H2_DISPLAY_OK;
}

static int test_display_info(void *user, h2_display_info_t *info) {
    (void)user;
    *info = (h2_display_info_t){64, 80, H2_DISPLAY_PIXEL_RGB565};
    return H2_DISPLAY_OK;
}

static int test_display_draw(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    test_state_t *state = (test_state_t *)user;
    assert(state->draw_count <
           sizeof(state->draw_rects) / sizeof(state->draw_rects[0]));
    assert(rect->x >= 0 && rect->y >= 0);
    assert(rect->width > 0 && rect->height > 0);
    assert(rect->x + rect->width <= 64);
    assert(rect->y + rect->height <= 80);
    assert(stride_bytes == 64u * sizeof(uint16_t));
    assert(format == H2_DISPLAY_PIXEL_RGB565);
    const uint16_t *expected_pixels =
        (const uint16_t *)state->framebuffer_allocation +
        (size_t)rect->y * 64u + (size_t)rect->x;
    assert(pixels == expected_pixels);
    state->draw_rects[state->draw_count] = *rect;
    ++state->draw_count;
    return H2_DISPLAY_OK;
}

static int test_display_present(void *user) {
    ++((test_state_t *)user)->present_count;
    return H2_DISPLAY_OK;
}

static int test_display_brightness(void *user, uint32_t percent) {
    (void)user;
    assert(percent == 90u);
    return H2_DISPLAY_OK;
}

static int test_display_close(void *user) {
    ++((test_state_t *)user)->display_close_count;
    return H2_DISPLAY_OK;
}

static h2_pal_result_t test_touch_open(void *user) {
    test_state_t *state = (test_state_t *)user;
    ++state->touch_open_count;
    return state->touch_unavailable ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
}

static h2_pal_result_t test_touch_info(
    void *user,
    h2_pal_touch_info_t *info) {
    (void)user;
    *info = (h2_pal_touch_info_t){100u, 100u};
    return H2_PAL_OK;
}

static h2_pal_result_t test_touch_poll(
    void *user,
    h2_pal_touch_event_t *event) {
    test_state_t *state = (test_state_t *)user;
    static const h2_pal_touch_event_t events[] = {
        {H2_PAL_TOUCH_EVENT_DOWN, -10, 120},
        {H2_PAL_TOUCH_EVENT_MOVE, 50, 50},
        {H2_PAL_TOUCH_EVENT_UP, 50, 50},
    };
    if (state->touch_fail_poll_once && !state->touch_poll_failed) {
        state->touch_poll_failed = 1;
        return H2_PAL_ERR_TIMEOUT;
    }
    if (state->touch_event_index >= sizeof(events) / sizeof(events[0])) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    *event = events[state->touch_event_index++];
    return H2_PAL_OK;
}

static h2_pal_result_t test_touch_close(void *user) {
    ++((test_state_t *)user)->touch_close_count;
    return H2_PAL_OK;
}

static int test_audio_info(void *user, h2_audio_info_t *info) {
    test_state_t *state = (test_state_t *)user;
    if (state->audio_unavailable) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    *info = (h2_audio_info_t){
        .available = 1,
        .mic_supported = 1,
        .mic_format = {16000u, 4u, 1u, H2_AUDIO_SAMPLE_S16LE},
    };
    return H2_AUDIO_OK;
}

static int test_audio_start(void *user) {
    ++((test_state_t *)user)->audio_start_count;
    return H2_AUDIO_OK;
}

static int test_audio_stop(void *user) {
    ++((test_state_t *)user)->audio_stop_count;
    return H2_AUDIO_OK;
}

static int test_audio_read(
    void *user,
    h2_audio_frame_t *frame,
    uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == 0u);
    assert(frame->capacity >= 4u * sizeof(int16_t));
    int16_t *samples = (int16_t *)frame->data;
    samples[0] = 8000;
    samples[1] = -8000;
    samples[2] = 9000;
    samples[3] = -9000;
    frame->bytes = 4u * sizeof(int16_t);
    return H2_AUDIO_OK;
}

static h2_pal_result_t test_time_get(void *user, uint64_t *out_ms) {
    *out_ms = ((test_state_t *)user)->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t test_time_sleep(void *user, uint32_t milliseconds) {
    test_state_t *state = (test_state_t *)user;
    state->now_ms += milliseconds + state->sleep_extra_ms;
    ++state->stop_count;
    return H2_PAL_OK;
}

static int test_should_stop(void *user) {
    const test_state_t *state = (const test_state_t *)user;
    const unsigned stop_after = state->stop_after_sleeps == 0u
        ? 1u
        : state->stop_after_sleeps;
    return state->stop_count >= stop_after;
}

static h2_pal_result_t test_ready(void *user) {
    test_state_t *state = (test_state_t *)user;
    ++state->ready_count;
    return state->ready_result;
}

static int run_test_app(test_state_t *state) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    static const h2_pal_display_vtable_t display_vtable = {
        .open = test_display_open,
        .get_info = test_display_info,
        .draw_bitmap = test_display_draw,
        .present = test_display_present,
        .set_brightness_percent = test_display_brightness,
        .close = test_display_close,
    };
    static const h2_pal_touch_vtable_t touch_vtable = {
        .open = test_touch_open,
        .get_info = test_touch_info,
        .poll_event = test_touch_poll,
        .close = test_touch_close,
    };
    static const h2_pal_audio_vtable_t audio_vtable = {
        .get_info = test_audio_info,
        .start_mic = test_audio_start,
        .stop_mic = test_audio_stop,
        .mic_read = test_audio_read,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = test_time_get,
        .sleep_ms = test_time_sleep,
    };
    const h2_pal_mem_api_t mem = {state, &mem_vtable};
    const h2_pal_display_api_t display = {state, &display_vtable};
    const h2_pal_touch_api_t touch = {state, &touch_vtable};
    const h2_pal_audio_api_t audio = {state, &audio_vtable};
    const h2_pal_time_api_t time = {state, &time_vtable};
    h2_runtime_t runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.mem = &mem;
    runtime.display = &display;
    runtime.touch = &touch;
    runtime.audio = &audio;
    runtime.time = &time;
    const h2_starboy_config_t config = {
        .frame_interval_ms = 16u,
        .random_seed = 9u,
        .ready = test_ready,
        .ready_user = state,
        .should_stop = test_should_stop,
        .should_stop_user = state,
    };
    return h2_starboy_run(&runtime, &config);
}

static void test_lifecycle_and_optional_capabilities(void) {
    test_state_t state = {.now_ms = 100u};
    assert(run_test_app(&state) == H2_PAL_OK);
    assert(state.display_open_count == 1u);
    assert(state.display_close_count == 1u);
    assert(state.draw_count == 1u);
    assert(state.draw_rects[0].x == 0 && state.draw_rects[0].y == 0);
    assert(state.draw_rects[0].width == 64 &&
           state.draw_rects[0].height == 80);
    assert(state.present_count == 1u);
    assert(state.touch_open_count == 1u);
    assert(state.touch_close_count == 1u);
    assert(state.touch_event_index == 3u);
    assert(state.audio_start_count == 1u);
    assert(state.audio_stop_count == 1u);
    assert(state.ready_count == 1u);
    assert(state.allocation_count == 0u);

    state = (test_state_t){
        .now_ms = 100u,
        .touch_unavailable = 1,
        .audio_unavailable = 1,
    };
    assert(run_test_app(&state) == H2_PAL_OK);
    assert(state.draw_count == 1u);
    assert(state.touch_open_count == 1u);
    assert(state.touch_close_count == 0u);
    assert(state.audio_start_count == 0u);
    assert(state.ready_count == 1u);
    assert(state.display_close_count == 1u);
    assert(state.allocation_count == 0u);
}

static void test_partial_display_updates_after_first_frame(void) {
    test_state_t state = {
        .now_ms = 100u,
        .stop_after_sleeps = 2u,
        .touch_unavailable = 1,
        .audio_unavailable = 1,
    };
    assert(run_test_app(&state) == H2_PAL_OK);
    assert(state.draw_count >= 2u && state.draw_count <= 3u);
    assert(state.present_count == 2u);
    assert(state.draw_rects[0].x == 0 && state.draw_rects[0].y == 0);
    assert(state.draw_rects[0].width == 64 &&
           state.draw_rects[0].height == 80);
    size_t incremental_pixels = 0u;
    for (size_t index = 1u; index < state.draw_count; ++index) {
        incremental_pixels += (size_t)state.draw_rects[index].width *
            (size_t)state.draw_rects[index].height;
    }
    assert(incremental_pixels < 64u * 80u);
    assert(state.allocation_count == 0u);
}

static void test_touch_recovers_after_transient_error(void) {
    test_state_t state = {
        .now_ms = 100u,
        .stop_after_sleeps = 2u,
        .sleep_extra_ms = TEST_TOUCH_RETRY_MS,
        .touch_fail_poll_once = 1,
        .audio_unavailable = 1,
    };
    assert(run_test_app(&state) == H2_PAL_OK);
    assert(state.touch_poll_failed);
    assert(state.touch_open_count == 2u);
    assert(state.touch_close_count == 2u);
    assert(state.touch_event_index == 3u);
    assert(state.draw_count >= 2u && state.draw_count <= 3u);
    assert(state.allocation_count == 0u);
}

static void test_partial_failure_cleanup(void) {
    test_state_t state = {
        .now_ms = 100u,
        .fail_allocation = 1,
    };
    assert(run_test_app(&state) == H2_PAL_ERR_NO_MEMORY);
    assert(state.display_open_count == 1u);
    assert(state.display_close_count == 1u);
    assert(state.touch_open_count == 0u);
    assert(state.ready_count == 0u);
    assert(state.allocation_count == 0u);

    state = (test_state_t){
        .now_ms = 100u,
        .ready_result = H2_PAL_ERR_IO,
    };
    assert(run_test_app(&state) == H2_PAL_ERR_IO);
    assert(state.draw_count == 1u);
    assert(state.present_count == 1u);
    assert(state.ready_count == 1u);
    assert(state.stop_count == 0u);
    assert(state.display_close_count == 1u);
    assert(state.touch_close_count == 1u);
    assert(state.audio_stop_count == 1u);
    assert(state.allocation_count == 0u);
}

int main(void) {
    assert(h2_starboy_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    test_coordinate_normalization();
    test_smooth_pointer_and_idle_gaze();
    test_natural_blink();
    test_idle_pose_changes_smoothly();
    test_audio_hysteresis_and_cooldown();
    test_shake_cycles_pupil_style_with_cooldown();
    test_audio_emotion_preserves_palette_colors();
    test_random_palette_transition_and_contrast();
    test_four_pupil_styles_render_distinctly();
    test_acorn_matches_reference_proportions_and_clipping();
    test_default_eyes_are_subtly_oval_and_wing_tilted();
    test_idle_wing_flutter_is_subtle_and_pointer_cancelled();
    test_scene_entry_and_exit_are_time_based();
    test_shake_streak_accumulation();
    test_two_second_power_hold();
    test_gravity_orientation_eases_and_freezes();
    test_gravity_rotates_complete_eye_geometry();
    test_audio_level_and_render();
    test_anxious_rounded_star_rotates();
    test_rotated_eyes_use_separate_dirty_regions();
    test_incremental_render_clears_previous_content();
    test_lifecycle_and_optional_capabilities();
    test_partial_display_updates_after_first_frame();
    test_touch_recovers_after_transient_error();
    test_partial_failure_cleanup();
    return 0;
}
