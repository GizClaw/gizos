#include "h2_starboy_internal.h"

#include <string.h>

#define H2_STARBOY_COLOR_BLACK UINT16_C(0x0000)
/* Geometry uses quarter-pixel coordinates for deterministic 2x2 sampling. */
#define H2_STARBOY_SUBPIXEL_SCALE 4
#define H2_STARBOY_ELLIPSE_VERTEX_COUNT 64
#define H2_STARBOY_STAR_MEAN_RADIUS_Q15 24575
#define H2_STARBOY_STAR_ROTATION_PERIOD_MS 1200u
#define H2_STARBOY_MAX_POLYGON_SPANS 8u
#define H2_STARBOY_EYE_ASPECT_PERCENT 118
#define H2_STARBOY_WING_TILT_Q15 2400

typedef enum starboy_sample_class {
    STARBOY_SAMPLE_BLACK = 0,
    STARBOY_SAMPLE_EYE = 1,
    STARBOY_SAMPLE_PUPIL = 2,
} starboy_sample_class_t;

typedef struct starboy_pupil_style_geometry {
    int32_t pupil_radius_x_percent;
    int32_t pupil_radius_y_percent;
    int32_t pupil_offset_y_percent;
} starboy_pupil_style_geometry_t;

static const starboy_pupil_style_geometry_t
s_pupil_style_geometry[H2_STARBOY_PUPIL_STYLE_COUNT] = {
    [H2_STARBOY_PUPIL_STYLE_DOT] = {10, 10, 0},
    [H2_STARBOY_PUPIL_STYLE_CIRCLE] = {28, 28, 0},
    [H2_STARBOY_PUPIL_STYLE_CAT] = {11, 46, 0},
    [H2_STARBOY_PUPIL_STYLE_ACORN] = {50, 90, 32},
};

typedef struct starboy_eye_geometry {
    int32_t center_x;
    int32_t center_y;
    int32_t radius_x;
    int32_t radius_y;
    int32_t tilt_q15;
    int32_t curvature_q15;
    int32_t right_x_q15;
    int32_t right_y_q15;
    int32_t down_x_q15;
    int32_t down_y_q15;
    int32_t pupil_x;
    int32_t pupil_y;
    int32_t pupil_radius_x;
    int32_t pupil_radius_y;
    int32_t pupil_min_x;
    int32_t pupil_max_x;
    int32_t pupil_min_y;
    int32_t pupil_max_y;
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    int32_t star_mix_q15;
    int32_t star_phase_q8;
    int32_t pupil_inward_sign;
    int32_t pupil_style_mix_q15;
    h2_starboy_pupil_style_t pupil_style;
    h2_starboy_pupil_style_t previous_pupil_style;
} starboy_eye_geometry_t;

typedef enum starboy_shape {
    STARBOY_SHAPE_EYE = 0,
    STARBOY_SHAPE_PUPIL,
} starboy_shape_t;

typedef struct starboy_q_span {
    int32_t first;
    int32_t last;
    int valid;
} starboy_q_span_t;

typedef struct starboy_pixel_span {
    int32_t first;
    int32_t last;
    int valid;
} starboy_pixel_span_t;

typedef struct starboy_q_span_set {
    starboy_q_span_t spans[H2_STARBOY_MAX_POLYGON_SPANS];
    size_t count;
} starboy_q_span_set_t;

typedef struct starboy_pixel_span_set {
    starboy_pixel_span_t spans[H2_STARBOY_MAX_POLYGON_SPANS];
    size_t count;
} starboy_pixel_span_set_t;

typedef struct starboy_point {
    int32_t x;
    int32_t y;
} starboy_point_t;

typedef struct starboy_shape_polygon {
    starboy_point_t points[H2_STARBOY_ELLIPSE_VERTEX_COUNT];
    int top_index;
    int bottom_index;
} starboy_shape_polygon_t;

typedef struct starboy_edge_walker {
    int index;
    int direction;
    int bottom_index;
} starboy_edge_walker_t;

static int32_t starboy_abs_i32(int32_t value) {
    return value < 0 ? -value : value;
}

static int32_t starboy_multiply_q15(int32_t first, int32_t second) {
    return (int32_t)((int64_t)first * second / H2_STARBOY_Q15_ONE);
}

static int32_t starboy_sine_q15(int32_t angle_q15) {
    const int32_t squared = starboy_multiply_q15(angle_q15, angle_q15);
    const int32_t cubed = starboy_multiply_q15(squared, angle_q15);
    return angle_q15 - cubed / 6;
}

static int32_t starboy_cosine_q15(int32_t angle_q15) {
    const int32_t squared = starboy_multiply_q15(angle_q15, angle_q15);
    const int32_t fourth = starboy_multiply_q15(squared, squared);
    return H2_STARBOY_Q15_ONE - squared / 2 + fourth / 24;
}

static int32_t starboy_clamp_i32(
    int32_t value,
    int32_t minimum,
    int32_t maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static int32_t starboy_circle_sine_q15(size_t index) {
    static const int32_t quarter_wave[17] = {
        0, 3212, 6393, 9512, 12540, 15446, 18204, 20787, 23170,
        25330, 27245, 28898, 30273, 31356, 32138, 32610, 32767,
    };
    const size_t wrapped = index % H2_STARBOY_ELLIPSE_VERTEX_COUNT;
    const size_t quadrant = wrapped / 16u;
    const size_t offset = wrapped % 16u;
    if (quadrant == 0u) {
        return quarter_wave[offset];
    }
    if (quadrant == 1u) {
        return quarter_wave[16u - offset];
    }
    if (quadrant == 2u) {
        return -quarter_wave[offset];
    }
    return -quarter_wave[16u - offset];
}

static int32_t starboy_circle_sine_q15_interpolated(int32_t index_q8) {
    const int32_t period_q8 = H2_STARBOY_ELLIPSE_VERTEX_COUNT * 256;
    int32_t wrapped_q8 = index_q8 % period_q8;
    if (wrapped_q8 < 0) {
        wrapped_q8 += period_q8;
    }
    const size_t index = (size_t)(wrapped_q8 / 256);
    const int32_t fraction_q8 = wrapped_q8 % 256;
    const int32_t first = starboy_circle_sine_q15(index);
    const int32_t second = starboy_circle_sine_q15(index + 1u);
    return first + (second - first) * fraction_q8 / 256;
}

static h2_starboy_pupil_style_t starboy_valid_pupil_style(
    h2_starboy_pupil_style_t pupil_style) {
    if ((unsigned int)pupil_style >= H2_STARBOY_PUPIL_STYLE_COUNT) {
        return H2_STARBOY_PUPIL_STYLE_ACORN;
    }
    return pupil_style;
}

static void starboy_build_shape_polygon(
    const starboy_eye_geometry_t *eye,
    starboy_shape_t shape,
    starboy_shape_polygon_t *polygon) {
    const int32_t center_x = shape == STARBOY_SHAPE_EYE
        ? eye->center_x
        : eye->pupil_x;
    const int32_t center_y = shape == STARBOY_SHAPE_EYE
        ? eye->center_y
        : eye->pupil_y;
    const int32_t radius_x = shape == STARBOY_SHAPE_EYE
        ? eye->radius_x
        : eye->pupil_radius_x;
    const int32_t radius_y = shape == STARBOY_SHAPE_EYE
        ? eye->radius_y
        : eye->pupil_radius_y;
    const int32_t curvature_q15 = shape == STARBOY_SHAPE_EYE
        ? eye->curvature_q15
        : eye->curvature_q15 / 2;
    const int32_t tilt_squared_q15 = starboy_multiply_q15(
        eye->tilt_q15, eye->tilt_q15);
    const int32_t determinant_q15 =
        H2_STARBOY_Q15_ONE + tilt_squared_q15;

    for (size_t index = 0u;
         index < H2_STARBOY_ELLIPSE_VERTEX_COUNT;
         ++index) {
        int32_t radius_scale_q15 = H2_STARBOY_Q15_ONE;
        if (shape == STARBOY_SHAPE_EYE && eye->star_mix_q15 > 0) {
            const int32_t star_wave_q15 =
                starboy_circle_sine_q15_interpolated(
                    (int32_t)index * 5 * 256 + 16 * 256 -
                    eye->star_phase_q8);
            const int32_t star_radius_q15 =
                H2_STARBOY_STAR_MEAN_RADIUS_Q15 + (int32_t)(
                    (int64_t)(H2_STARBOY_Q15_ONE -
                              H2_STARBOY_STAR_MEAN_RADIUS_Q15) *
                    star_wave_q15 / H2_STARBOY_Q15_ONE);
            radius_scale_q15 = H2_STARBOY_Q15_ONE - (int32_t)(
                (int64_t)(H2_STARBOY_Q15_ONE - star_radius_q15) *
                eye->star_mix_q15 / H2_STARBOY_Q15_ONE);
        }
        int32_t local_x = (int32_t)(
            (int64_t)radius_x *
            starboy_circle_sine_q15(index + 16u) * radius_scale_q15 /
            H2_STARBOY_Q15_ONE / H2_STARBOY_Q15_ONE);
        const int32_t curved_y = (int32_t)(
            (int64_t)radius_y * starboy_circle_sine_q15(index) *
            radius_scale_q15 /
            H2_STARBOY_Q15_ONE / H2_STARBOY_Q15_ONE);
        if (shape == STARBOY_SHAPE_PUPIL) {
            const int32_t previous_acorn_q15 =
                eye->previous_pupil_style == H2_STARBOY_PUPIL_STYLE_ACORN
                ? H2_STARBOY_Q15_ONE
                : 0;
            const int32_t current_acorn_q15 =
                eye->pupil_style == H2_STARBOY_PUPIL_STYLE_ACORN
                ? H2_STARBOY_Q15_ONE
                : 0;
            const int32_t acorn_mix_q15 = previous_acorn_q15 + (int32_t)(
                (int64_t)(current_acorn_q15 - previous_acorn_q15) *
                eye->pupil_style_mix_q15 / H2_STARBOY_Q15_ONE);
            local_x -= (int32_t)(
                (int64_t)eye->pupil_inward_sign * radius_x *
                starboy_circle_sine_q15(index) * 4200 * acorn_mix_q15 /
                H2_STARBOY_Q15_ONE / H2_STARBOY_Q15_ONE /
                H2_STARBOY_Q15_ONE);
        }
        const int32_t curve_offset = radius_x > 0
            ? (int32_t)((int64_t)curvature_q15 * local_x * local_x /
                        radius_x / H2_STARBOY_Q15_ONE) -
              starboy_multiply_q15(curvature_q15, radius_x) / 3
            : 0;
        const int32_t raw_y = curved_y - curve_offset;
        const int32_t delta_x = (int32_t)(
            ((int64_t)local_x * H2_STARBOY_Q15_ONE -
             (int64_t)eye->tilt_q15 * raw_y) /
            determinant_q15);
        const int32_t delta_y = (int32_t)(
            ((int64_t)raw_y * H2_STARBOY_Q15_ONE +
             (int64_t)eye->tilt_q15 * local_x) /
            determinant_q15);
        const int32_t world_x = (int32_t)(
            ((int64_t)eye->right_x_q15 * delta_x +
             (int64_t)eye->down_x_q15 * delta_y) /
            H2_STARBOY_Q15_ONE);
        const int32_t world_y = (int32_t)(
            ((int64_t)eye->right_y_q15 * delta_x +
             (int64_t)eye->down_y_q15 * delta_y) /
            H2_STARBOY_Q15_ONE);
        polygon->points[index] = (starboy_point_t){
            .x = center_x + world_x,
            .y = center_y + world_y,
        };
    }

    polygon->top_index = 0;
    polygon->bottom_index = 0;
    for (int index = 1;
         index < H2_STARBOY_ELLIPSE_VERTEX_COUNT;
         ++index) {
        if (polygon->points[index].y <
            polygon->points[polygon->top_index].y) {
            polygon->top_index = index;
        }
        if (polygon->points[index].y >
            polygon->points[polygon->bottom_index].y) {
            polygon->bottom_index = index;
        }
    }
}

static uint16_t starboy_blend_color_q15(
    uint16_t first,
    uint16_t second,
    int32_t second_weight_q15) {
    second_weight_q15 = starboy_clamp_i32(
        second_weight_q15, 0, H2_STARBOY_Q15_ONE);
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

static uint16_t starboy_sample_color(
    starboy_sample_class_t sample_class,
    const h2_starboy_behavior_t *behavior) {
    if (sample_class == STARBOY_SAMPLE_BLACK) {
        return H2_STARBOY_COLOR_BLACK;
    }
    const uint16_t previous = sample_class == STARBOY_SAMPLE_PUPIL
        ? behavior->previous_palette.pupil_color
        : behavior->previous_palette.eye_color;
    const uint16_t current = sample_class == STARBOY_SAMPLE_PUPIL
        ? behavior->palette.pupil_color
        : behavior->palette.eye_color;
    return starboy_blend_color_q15(
        previous, current, behavior->palette_mix_q15);
}

static int32_t starboy_pupil_style_percent(
    const h2_starboy_behavior_t *behavior,
    int vertical) {
    const h2_starboy_pupil_style_t previous_pupil_style =
        starboy_valid_pupil_style(behavior->previous_pupil_style);
    const h2_starboy_pupil_style_t pupil_style =
        starboy_valid_pupil_style(behavior->pupil_style);
    const int32_t previous = vertical
        ? s_pupil_style_geometry[previous_pupil_style].pupil_radius_y_percent
        : s_pupil_style_geometry[previous_pupil_style].pupil_radius_x_percent;
    const int32_t current = vertical
        ? s_pupil_style_geometry[pupil_style].pupil_radius_y_percent
        : s_pupil_style_geometry[pupil_style].pupil_radius_x_percent;
    const int32_t mix_q15 = starboy_clamp_i32(
        behavior->pupil_style_mix_q15, 0, H2_STARBOY_Q15_ONE);
    return previous + (int32_t)(
        (int64_t)(current - previous) * mix_q15 /
        H2_STARBOY_Q15_ONE);
}

static int32_t starboy_pupil_offset_y_percent(
    const h2_starboy_behavior_t *behavior) {
    const h2_starboy_pupil_style_t previous_pupil_style =
        starboy_valid_pupil_style(behavior->previous_pupil_style);
    const h2_starboy_pupil_style_t pupil_style =
        starboy_valid_pupil_style(behavior->pupil_style);
    const int32_t previous =
        s_pupil_style_geometry[previous_pupil_style].pupil_offset_y_percent;
    const int32_t current =
        s_pupil_style_geometry[pupil_style].pupil_offset_y_percent;
    const int32_t mix_q15 = starboy_clamp_i32(
        behavior->pupil_style_mix_q15, 0, H2_STARBOY_Q15_ONE);
    return previous + (int32_t)(
        (int64_t)(current - previous) * mix_q15 /
        H2_STARBOY_Q15_ONE);
}

static uint16_t starboy_blend_coverage(
    uint16_t background,
    uint16_t foreground,
    uint32_t coverage) {
    if (coverage == 0u) {
        return background;
    }
    if (coverage >= 4u) {
        return foreground;
    }
    const uint32_t inverse = 4u - coverage;
    const uint32_t red =
        (((background >> 11) & 0x1fu) * inverse +
         ((foreground >> 11) & 0x1fu) * coverage + 2u) / 4u;
    const uint32_t green =
        (((background >> 5) & 0x3fu) * inverse +
         ((foreground >> 5) & 0x3fu) * coverage + 2u) / 4u;
    const uint32_t blue =
        ((background & 0x1fu) * inverse +
         (foreground & 0x1fu) * coverage + 2u) / 4u;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static int starboy_wrapped_vertex_index(int index) {
    if (index < 0) {
        return index + H2_STARBOY_ELLIPSE_VERTEX_COUNT;
    }
    if (index >= H2_STARBOY_ELLIPSE_VERTEX_COUNT) {
        return index - H2_STARBOY_ELLIPSE_VERTEX_COUNT;
    }
    return index;
}

static int starboy_edge_intersection(
    const starboy_shape_polygon_t *polygon,
    starboy_edge_walker_t *walker,
    int32_t y,
    int32_t *out_x) {
    while (walker->index != walker->bottom_index) {
        const int next_index = starboy_wrapped_vertex_index(
            walker->index + walker->direction);
        const starboy_point_t first = polygon->points[walker->index];
        const starboy_point_t second = polygon->points[next_index];
        if (second.y <= first.y || y >= second.y) {
            walker->index = next_index;
            continue;
        }
        if (y < first.y) {
            return 0;
        }
        *out_x = first.x + (int32_t)(
            (int64_t)(second.x - first.x) * (y - first.y) /
            (second.y - first.y));
        return 1;
    }
    return 0;
}

static starboy_q_span_t starboy_polygon_span(
    const starboy_shape_polygon_t *polygon,
    starboy_edge_walker_t walkers[2],
    int32_t y) {
    int32_t intersections[2] = {0};
    if (!starboy_edge_intersection(
            polygon, &walkers[0], y, &intersections[0]) ||
        !starboy_edge_intersection(
            polygon, &walkers[1], y, &intersections[1])) {
        return (starboy_q_span_t){0};
    }
    int32_t first = intersections[0] < intersections[1]
        ? intersections[0]
        : intersections[1];
    int32_t last = intersections[0] > intersections[1]
        ? intersections[0]
        : intersections[1];
    return (starboy_q_span_t){
        .first = first,
        .last = last,
        .valid = first <= last,
    };
}

static void starboy_initialize_edge_walkers(
    const starboy_shape_polygon_t *polygon,
    starboy_edge_walker_t walkers[2][2]) {
    for (size_t sample_y = 0u; sample_y < 2u; ++sample_y) {
        walkers[sample_y][0] = (starboy_edge_walker_t){
            .index = polygon->top_index,
            .direction = 1,
            .bottom_index = polygon->bottom_index,
        };
        walkers[sample_y][1] = (starboy_edge_walker_t){
            .index = polygon->top_index,
            .direction = -1,
            .bottom_index = polygon->bottom_index,
        };
    }
}

static int32_t starboy_floor_divide(int32_t numerator, int32_t denominator) {
    if (numerator >= 0) {
        return numerator / denominator;
    }
    return -(-numerator + denominator - 1) / denominator;
}

static int32_t starboy_ceil_divide(int32_t numerator, int32_t denominator) {
    if (numerator >= 0) {
        return (numerator + denominator - 1) / denominator;
    }
    return -(-numerator / denominator);
}

static starboy_pixel_span_t starboy_q_span_to_pixels(
    const starboy_q_span_t *span,
    int32_t sample_offset,
    int32_t width) {
    if (!span->valid) {
        return (starboy_pixel_span_t){0};
    }
    int32_t first = starboy_ceil_divide(
        span->first - sample_offset, H2_STARBOY_SUBPIXEL_SCALE);
    int32_t last = starboy_floor_divide(
        span->last - sample_offset, H2_STARBOY_SUBPIXEL_SCALE);
    if (last < 0 || first >= width) {
        return (starboy_pixel_span_t){0};
    }
    first = starboy_clamp_i32(first, 0, width - 1);
    last = starboy_clamp_i32(last, 0, width - 1);
    return (starboy_pixel_span_t){
        .first = first,
        .last = last,
        .valid = first <= last,
    };
}

static starboy_q_span_set_t starboy_polygon_span_set(
    const starboy_shape_polygon_t *polygon,
    int32_t y) {
    int32_t intersections[H2_STARBOY_ELLIPSE_VERTEX_COUNT] = {0};
    size_t intersection_count = 0u;
    for (size_t index = 0u;
         index < H2_STARBOY_ELLIPSE_VERTEX_COUNT;
         ++index) {
        const starboy_point_t first = polygon->points[index];
        const starboy_point_t second = polygon->points[
            (index + 1u) % H2_STARBOY_ELLIPSE_VERTEX_COUNT];
        const int crosses =
            (first.y <= y && y < second.y) ||
            (second.y <= y && y < first.y);
        if (!crosses) {
            continue;
        }
        const int32_t x = first.x + (int32_t)(
            (int64_t)(second.x - first.x) * (y - first.y) /
            (second.y - first.y));
        size_t insert_at = intersection_count;
        while (insert_at > 0u && intersections[insert_at - 1u] > x) {
            intersections[insert_at] = intersections[insert_at - 1u];
            --insert_at;
        }
        intersections[insert_at] = x;
        ++intersection_count;
    }

    starboy_q_span_set_t result = {0};
    for (size_t index = 0u;
         index + 1u < intersection_count &&
         result.count < H2_STARBOY_MAX_POLYGON_SPANS;
         index += 2u) {
        result.spans[result.count++] = (starboy_q_span_t){
            .first = intersections[index],
            .last = intersections[index + 1u],
            .valid = intersections[index] <= intersections[index + 1u],
        };
    }
    return result;
}

static starboy_q_span_set_t starboy_intersect_span_sets(
    const starboy_q_span_set_t *first,
    const starboy_q_span_set_t *second) {
    starboy_q_span_set_t result = {0};
    for (size_t first_index = 0u;
         first_index < first->count;
         ++first_index) {
        for (size_t second_index = 0u;
             second_index < second->count;
             ++second_index) {
            const int32_t start =
                first->spans[first_index].first >
                    second->spans[second_index].first
                ? first->spans[first_index].first
                : second->spans[second_index].first;
            const int32_t end =
                first->spans[first_index].last <
                    second->spans[second_index].last
                ? first->spans[first_index].last
                : second->spans[second_index].last;
            if (start > end) {
                continue;
            }
            if (result.count >= H2_STARBOY_MAX_POLYGON_SPANS) {
                return result;
            }
            result.spans[result.count++] = (starboy_q_span_t){
                .first = start,
                .last = end,
                .valid = 1,
            };
        }
    }
    return result;
}

static starboy_pixel_span_set_t starboy_span_set_to_pixels(
    const starboy_q_span_set_t *spans,
    int32_t sample_offset,
    int32_t width) {
    starboy_pixel_span_set_t result = {0};
    for (size_t index = 0u;
         index < spans->count;
         ++index) {
        const starboy_pixel_span_t pixel_span = starboy_q_span_to_pixels(
            &spans->spans[index], sample_offset, width);
        if (pixel_span.valid) {
            result.spans[result.count++] = pixel_span;
        }
    }
    return result;
}

static int starboy_pixel_span_set_contains(
    const starboy_pixel_span_set_t *spans,
    int32_t x) {
    for (size_t index = 0u; index < spans->count; ++index) {
        if (x >= spans->spans[index].first &&
            x <= spans->spans[index].last) {
            return 1;
        }
    }
    return 0;
}

static void starboy_render_star_shape(
    uint16_t *pixels,
    uint32_t width,
    const starboy_eye_geometry_t *eye,
    starboy_shape_t shape,
    uint16_t color,
    int32_t minimum_y,
    int32_t maximum_y) {
    starboy_shape_polygon_t polygon = {0};
    starboy_build_shape_polygon(eye, shape, &polygon);
    starboy_shape_polygon_t clip_polygon = {0};
    if (shape == STARBOY_SHAPE_PUPIL) {
        starboy_build_shape_polygon(
            eye, STARBOY_SHAPE_EYE, &clip_polygon);
    }
    const int32_t sample_offsets[2] = {1, 3};
    for (int32_t y = minimum_y; y <= maximum_y; ++y) {
        starboy_pixel_span_set_t sample_spans[4] = {0};
        int32_t union_first = (int32_t)width;
        int32_t union_last = -1;
        for (size_t sample_y = 0u; sample_y < 2u; ++sample_y) {
            const int32_t sample_y_q =
                y * H2_STARBOY_SUBPIXEL_SCALE + sample_offsets[sample_y];
            starboy_q_span_set_t q_spans = starboy_polygon_span_set(
                &polygon, sample_y_q);
            if (shape == STARBOY_SHAPE_PUPIL) {
                const starboy_q_span_set_t clip_spans =
                    starboy_polygon_span_set(&clip_polygon, sample_y_q);
                q_spans = starboy_intersect_span_sets(
                    &q_spans, &clip_spans);
            }
            for (size_t sample_x = 0u; sample_x < 2u; ++sample_x) {
                const size_t sample_index = sample_y * 2u + sample_x;
                sample_spans[sample_index] = starboy_span_set_to_pixels(
                    &q_spans,
                    sample_offsets[sample_x],
                    (int32_t)width);
                for (size_t span_index = 0u;
                     span_index < sample_spans[sample_index].count;
                     ++span_index) {
                    const starboy_pixel_span_t *span =
                        &sample_spans[sample_index].spans[span_index];
                    if (span->first < union_first) {
                        union_first = span->first;
                    }
                    if (span->last > union_last) {
                        union_last = span->last;
                    }
                }
            }
        }
        if (union_first > union_last) {
            continue;
        }
        uint16_t *row = pixels + (size_t)y * width;
        for (int32_t x = union_first; x <= union_last; ++x) {
            uint32_t coverage = 0u;
            for (size_t sample_index = 0u;
                 sample_index < 4u;
                 ++sample_index) {
                coverage += starboy_pixel_span_set_contains(
                    &sample_spans[sample_index], x);
            }
            row[x] = starboy_blend_coverage(row[x], color, coverage);
        }
    }
}

static uint32_t starboy_sample_coverage(
    const starboy_pixel_span_t sample_spans[4],
    int32_t x) {
    uint32_t coverage = 0u;
    for (size_t sample_index = 0u;
         sample_index < 4u;
         ++sample_index) {
        coverage += sample_spans[sample_index].valid &&
            x >= sample_spans[sample_index].first &&
            x <= sample_spans[sample_index].last;
    }
    return coverage;
}

static void starboy_render_shape(
    uint16_t *pixels,
    uint32_t width,
    uint32_t height,
    const starboy_eye_geometry_t *eye,
    starboy_shape_t shape,
    uint16_t color) {
    int32_t minimum_x = shape == STARBOY_SHAPE_EYE
        ? eye->min_x
        : eye->pupil_min_x;
    int32_t maximum_x = shape == STARBOY_SHAPE_EYE
        ? eye->max_x
        : eye->pupil_max_x;
    int32_t minimum_y = shape == STARBOY_SHAPE_EYE
        ? eye->min_y
        : eye->pupil_min_y;
    int32_t maximum_y = shape == STARBOY_SHAPE_EYE
        ? eye->max_y
        : eye->pupil_max_y;
    minimum_x = starboy_clamp_i32(minimum_x, 0, (int32_t)width - 1);
    maximum_x = starboy_clamp_i32(maximum_x, 0, (int32_t)width - 1);
    minimum_y = starboy_clamp_i32(minimum_y, 0, (int32_t)height - 1);
    maximum_y = starboy_clamp_i32(maximum_y, 0, (int32_t)height - 1);
    if (minimum_x > maximum_x || minimum_y > maximum_y) {
        return;
    }

    if (eye->star_mix_q15 > 0) {
        starboy_render_star_shape(
            pixels,
            width,
            eye,
            shape,
            color,
            minimum_y,
            maximum_y);
        return;
    }

    starboy_shape_polygon_t polygon = {0};
    starboy_build_shape_polygon(eye, shape, &polygon);
    starboy_shape_polygon_t clip_polygon = {0};
    if (shape == STARBOY_SHAPE_PUPIL) {
        starboy_build_shape_polygon(
            eye, STARBOY_SHAPE_EYE, &clip_polygon);
    }
    const int32_t sample_offsets[2] = {1, 3};
    starboy_edge_walker_t walkers[2][2] = {0};
    starboy_edge_walker_t clip_walkers[2][2] = {0};
    starboy_initialize_edge_walkers(&polygon, walkers);
    if (shape == STARBOY_SHAPE_PUPIL) {
        starboy_initialize_edge_walkers(&clip_polygon, clip_walkers);
    }
    for (int32_t y = minimum_y; y <= maximum_y; ++y) {
        starboy_pixel_span_t sample_spans[4] = {0};
        int32_t union_first = (int32_t)width;
        int32_t union_last = -1;
        for (size_t sample_y = 0u; sample_y < 2u; ++sample_y) {
            const int32_t sample_y_q =
                y * H2_STARBOY_SUBPIXEL_SCALE +
                sample_offsets[sample_y];
            starboy_q_span_t q_span = starboy_polygon_span(
                &polygon,
                walkers[sample_y],
                sample_y_q);
            if (shape == STARBOY_SHAPE_PUPIL) {
                const starboy_q_span_t clip_span = starboy_polygon_span(
                    &clip_polygon,
                    clip_walkers[sample_y],
                    sample_y_q);
                if (!clip_span.valid) {
                    q_span.valid = 0;
                } else {
                    if (q_span.first < clip_span.first) {
                        q_span.first = clip_span.first;
                    }
                    if (q_span.last > clip_span.last) {
                        q_span.last = clip_span.last;
                    }
                    q_span.valid = q_span.valid &&
                        q_span.first <= q_span.last;
                }
            }
            if (!q_span.valid) {
                continue;
            }
            for (size_t sample_x = 0u; sample_x < 2u; ++sample_x) {
                const size_t sample_index = sample_y * 2u + sample_x;
                sample_spans[sample_index] = starboy_q_span_to_pixels(
                    &q_span,
                    sample_offsets[sample_x],
                    (int32_t)width);
                if (!sample_spans[sample_index].valid) {
                    continue;
                }
                if (sample_spans[sample_index].first < union_first) {
                    union_first = sample_spans[sample_index].first;
                }
                if (sample_spans[sample_index].last > union_last) {
                    union_last = sample_spans[sample_index].last;
                }
            }
        }
        if (union_first > union_last) {
            continue;
        }

        uint16_t *row = pixels + (size_t)y * width;
        int all_samples_valid = 1;
        int32_t solid_first = 0;
        int32_t solid_last = (int32_t)width - 1;
        for (size_t sample_index = 0u;
             sample_index < 4u;
             ++sample_index) {
            if (!sample_spans[sample_index].valid) {
                all_samples_valid = 0;
                break;
            }
            if (sample_spans[sample_index].first > solid_first) {
                solid_first = sample_spans[sample_index].first;
            }
            if (sample_spans[sample_index].last < solid_last) {
                solid_last = sample_spans[sample_index].last;
            }
        }
        if (!all_samples_valid || solid_first > solid_last) {
            for (int32_t x = union_first; x <= union_last; ++x) {
                row[x] = starboy_blend_coverage(
                    row[x],
                    color,
                    starboy_sample_coverage(sample_spans, x));
            }
            continue;
        }

        for (int32_t x = union_first; x < solid_first; ++x) {
            row[x] = starboy_blend_coverage(
                row[x],
                color,
                starboy_sample_coverage(sample_spans, x));
        }
        for (int32_t x = solid_first; x <= solid_last; ++x) {
            row[x] = color;
        }
        for (int32_t x = solid_last + 1; x <= union_last; ++x) {
            row[x] = starboy_blend_coverage(
                row[x],
                color,
                starboy_sample_coverage(sample_spans, x));
        }
    }
}

static h2_display_rect_t starboy_union_rect(
    const h2_display_rect_t *first,
    const h2_display_rect_t *second) {
    const int x1 = first->x < second->x ? first->x : second->x;
    const int y1 = first->y < second->y ? first->y : second->y;
    const int first_x2 = first->x + first->width;
    const int second_x2 = second->x + second->width;
    const int first_y2 = first->y + first->height;
    const int second_y2 = second->y + second->height;
    const int x2 = first_x2 > second_x2 ? first_x2 : second_x2;
    const int y2 = first_y2 > second_y2 ? first_y2 : second_y2;
    return (h2_display_rect_t){
        .x = x1,
        .y = y1,
        .width = x2 - x1,
        .height = y2 - y1,
    };
}

static int starboy_rects_overlap(
    const h2_display_rect_t *first,
    const h2_display_rect_t *second) {
    return first->x < second->x + second->width &&
        second->x < first->x + first->width &&
        first->y < second->y + second->height &&
        second->y < first->y + first->height;
}

static void starboy_clear_rect(
    uint16_t *pixels,
    uint32_t width,
    const h2_display_rect_t *rect) {
    for (int row = 0; row < rect->height; ++row) {
        uint16_t *row_pixels = pixels +
            (size_t)(rect->y + row) * width + (size_t)rect->x;
        memset(row_pixels, 0, (size_t)rect->width * sizeof(*pixels));
    }
}

void h2_starboy_render_dirty_regions(
    uint16_t *pixels,
    uint32_t width,
    uint32_t height,
    const h2_starboy_behavior_t *behavior,
    const h2_starboy_render_regions_t *previous_regions,
    h2_starboy_render_regions_t *out_regions) {
    if (out_regions != NULL) {
        *out_regions = (h2_starboy_render_regions_t){0};
    }
    if (pixels == NULL || behavior == NULL || out_regions == NULL ||
        width == 0u || height == 0u) {
        return;
    }

    const int32_t scene_scale_q15 =
        behavior->scene_phase == H2_STARBOY_SCENE_GONE
        ? 0
        : starboy_clamp_i32(
              behavior->scene_scale_q15 > 0
                  ? behavior->scene_scale_q15
                  : H2_STARBOY_Q15_ONE,
              0,
              H2_STARBOY_Q15_ONE);
    if (scene_scale_q15 == 0) {
        const h2_display_rect_t full_frame_rect = {
            .x = 0,
            .y = 0,
            .width = (int)width,
            .height = (int)height,
        };
        if (previous_regions == NULL) {
            out_regions->dirty_rects[0] = full_frame_rect;
            out_regions->dirty_rect_count = 1u;
        } else {
            out_regions->dirty_rects[0] =
                previous_regions->eye_content_rects[0];
            out_regions->dirty_rects[1] =
                previous_regions->eye_content_rects[1];
            out_regions->dirty_rect_count = H2_STARBOY_EYE_COUNT;
            if (starboy_rects_overlap(
                    &out_regions->dirty_rects[0],
                    &out_regions->dirty_rects[1])) {
                out_regions->dirty_rects[0] = starboy_union_rect(
                    &out_regions->dirty_rects[0],
                    &out_regions->dirty_rects[1]);
                out_regions->dirty_rects[1] = (h2_display_rect_t){0};
                out_regions->dirty_rect_count = 1u;
            }
        }
        for (size_t index = 0u;
             index < out_regions->dirty_rect_count;
             ++index) {
            starboy_clear_rect(pixels, width, &out_regions->dirty_rects[index]);
        }
        return;
    }

    const int32_t base_radius_x[2] = {
        (int32_t)(
            (int64_t)(width * 35u / 200u) * scene_scale_q15 /
            H2_STARBOY_Q15_ONE),
        (int32_t)(
            (int64_t)(width * 35u / 200u) * scene_scale_q15 /
            H2_STARBOY_Q15_ONE),
    };
    const int32_t base_radius_y[2] = {
        base_radius_x[0] * H2_STARBOY_EYE_ASPECT_PERCENT / 100,
        base_radius_x[1] * H2_STARBOY_EYE_ASPECT_PERCENT / 100,
    };
    const int32_t anxious_mix_q15 =
        behavior->emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS];
    int32_t openness_q15 = H2_STARBOY_Q15_ONE - behavior->blink_q15 -
        anxious_mix_q15 / 10;
    if (openness_q15 < H2_STARBOY_Q15_ONE / 40) {
        openness_q15 = H2_STARBOY_Q15_ONE / 40;
    }
    int32_t pose_scale_q15 =
        H2_STARBOY_Q15_ONE + behavior->pose_squash_q15;
    if (pose_scale_q15 < H2_STARBOY_Q15_ONE / 2) {
        pose_scale_q15 = H2_STARBOY_Q15_ONE / 2;
    }
    const int32_t idle_pose_weight_q15 = H2_STARBOY_Q15_ONE -
        (int32_t)((int64_t)behavior->tracking_q15 * 3 / 4);
    const int32_t local_pupil_x_q15 = behavior->gaze_x_q15 -
        (int32_t)((int64_t)behavior->head_x_q15 * 3 / 5);
    const int32_t local_pupil_y_q15 = behavior->gaze_y_q15 -
        (int32_t)((int64_t)behavior->head_y_q15 * 3 / 5);
    const int32_t yaw_q15 = (int32_t)(
        (int64_t)behavior->head_x_q15 * 5200 /
        H2_STARBOY_Q15_ONE);
    const int32_t pitch_q15 = (int32_t)(
        -(int64_t)behavior->head_y_q15 * 4200 /
        H2_STARBOY_Q15_ONE);
    const int32_t yaw_sine_q15 = starboy_sine_q15(yaw_q15);
    const int32_t yaw_cosine_q15 = starboy_cosine_q15(yaw_q15);
    const int32_t pitch_sine_q15 = starboy_sine_q15(pitch_q15);
    const int32_t pitch_cosine_q15 = starboy_cosine_q15(pitch_q15);
    const int32_t anchor_x_q15[2] = {
        (int32_t)(-(int64_t)13500 * scene_scale_q15 /
                  H2_STARBOY_Q15_ONE),
        (int32_t)((int64_t)13500 * scene_scale_q15 /
                  H2_STARBOY_Q15_ONE),
    };
    const int32_t anchor_y_q15[2] = {
        (int32_t)(-(int64_t)320 * scene_scale_q15 /
                  H2_STARBOY_Q15_ONE),
        (int32_t)((int64_t)320 * scene_scale_q15 /
                  H2_STARBOY_Q15_ONE),
    };
    const int32_t anchor_z_q15 = 30300;
    const int32_t pupil_radius_x_percent =
        starboy_pupil_style_percent(behavior, 0);
    const int32_t pupil_radius_y_percent =
        starboy_pupil_style_percent(behavior, 1);
    const int32_t pupil_offset_y_percent =
        starboy_pupil_offset_y_percent(behavior);
    int32_t down_x_q15 = behavior->gravity_down_x_q15;
    int32_t down_y_q15 = behavior->gravity_down_y_q15;
    if (down_x_q15 == 0 && down_y_q15 == 0) {
        down_y_q15 = H2_STARBOY_Q15_ONE;
    }
    const int32_t right_x_q15 = down_y_q15;
    const int32_t right_y_q15 = -down_x_q15;
    const int32_t screen_center_x = (int32_t)(width / 2u);
    const int32_t screen_center_y = (int32_t)(height / 2u);
    const int32_t screen_center_x_q2 =
        screen_center_x * H2_STARBOY_SUBPIXEL_SCALE;
    const int32_t screen_center_y_q2 =
        screen_center_y * H2_STARBOY_SUBPIXEL_SCALE;
    const int32_t star_mix_q15 =
        behavior->emotion_mix_q15[H2_STARBOY_EMOTION_ANXIOUS];
    const int32_t star_phase_q8 = (int32_t)(
        (behavior->last_update_ms % H2_STARBOY_STAR_ROTATION_PERIOD_MS) *
        (H2_STARBOY_ELLIPSE_VERTEX_COUNT * 256u) /
        H2_STARBOY_STAR_ROTATION_PERIOD_MS);

    starboy_eye_geometry_t eyes[2] = {0};
    for (int eye_index = 0; eye_index < 2; ++eye_index) {
        const int32_t tracked_asymmetry = (int32_t)(
            (int64_t)behavior->pose_asymmetry_q15 * idle_pose_weight_q15 /
            H2_STARBOY_Q15_ONE);
        const int32_t asymmetry = eye_index == 0
            ? tracked_asymmetry
            : -tracked_asymmetry;
        const int32_t yawed_x_q15 =
            starboy_multiply_q15(
                yaw_cosine_q15, anchor_x_q15[eye_index]) +
            starboy_multiply_q15(yaw_sine_q15, anchor_z_q15);
        const int32_t yawed_z_q15 =
            starboy_multiply_q15(yaw_cosine_q15, anchor_z_q15) -
            starboy_multiply_q15(
                yaw_sine_q15, anchor_x_q15[eye_index]);
        const int32_t projected_y_q15 =
            starboy_multiply_q15(
                pitch_cosine_q15, anchor_y_q15[eye_index]) -
            starboy_multiply_q15(pitch_sine_q15, yawed_z_q15);
        const int32_t projected_z_q15 =
            starboy_multiply_q15(
                pitch_sine_q15, anchor_y_q15[eye_index]) +
            starboy_multiply_q15(pitch_cosine_q15, yawed_z_q15);
        const int32_t perspective_scale_q15 = starboy_clamp_i32(
            (int32_t)((int64_t)projected_z_q15 *
                      H2_STARBOY_Q15_ONE / anchor_z_q15),
            H2_STARBOY_Q15_ONE * 3 / 4,
            H2_STARBOY_Q15_ONE * 5 / 4);
        const int32_t eye_radius_x = (int32_t)(
            (int64_t)base_radius_x[eye_index] * perspective_scale_q15 /
            H2_STARBOY_Q15_ONE);
        int32_t eye_radius_y = (int32_t)(
            (int64_t)base_radius_y[eye_index] * pose_scale_q15 /
            H2_STARBOY_Q15_ONE);
        eye_radius_y = (int32_t)(
            (int64_t)eye_radius_y * (H2_STARBOY_Q15_ONE + asymmetry) /
            H2_STARBOY_Q15_ONE);
        eye_radius_y = (int32_t)(
            (int64_t)eye_radius_y * openness_q15 /
            H2_STARBOY_Q15_ONE);
        eye_radius_y = (int32_t)(
            (int64_t)eye_radius_y * perspective_scale_q15 /
            H2_STARBOY_Q15_ONE);
        if (eye_radius_y < 2) {
            eye_radius_y = 2;
        }

        const int32_t local_center_x_q2 = screen_center_x_q2 +
            (int32_t)((int64_t)yawed_x_q15 * (int32_t)(width / 2u) *
                      H2_STARBOY_SUBPIXEL_SCALE /
                      H2_STARBOY_Q15_ONE) +
            behavior->motion_offset_x_q2;
        int32_t local_center_y_q2 = screen_center_y_q2 +
            (int32_t)((int64_t)projected_y_q15 *
                      (int32_t)(height / 2u) *
                      H2_STARBOY_SUBPIXEL_SCALE /
                      H2_STARBOY_Q15_ONE) +
            behavior->motion_offset_y_q2;
        local_center_y_q2 += (int32_t)(
            (int64_t)behavior->scene_bob_q15 *
            (int32_t)(height * 3u / 100u) *
            H2_STARBOY_SUBPIXEL_SCALE / H2_STARBOY_Q15_ONE);
        local_center_y_q2 += (int32_t)(
            (int64_t)asymmetry * (int32_t)(height * 2u / 100u) *
            H2_STARBOY_SUBPIXEL_SCALE / H2_STARBOY_Q15_ONE);
        const int32_t local_center_dx_q2 =
            local_center_x_q2 - screen_center_x_q2;
        const int32_t local_center_dy_q2 =
            local_center_y_q2 - screen_center_y_q2;
        const int32_t center_x_q2 = screen_center_x_q2 + (int32_t)(
            ((int64_t)right_x_q15 * local_center_dx_q2 +
             (int64_t)down_x_q15 * local_center_dy_q2) /
            H2_STARBOY_Q15_ONE);
        const int32_t center_y_q2 = screen_center_y_q2 + (int32_t)(
            ((int64_t)right_y_q15 * local_center_dx_q2 +
             (int64_t)down_y_q15 * local_center_dy_q2) /
            H2_STARBOY_Q15_ONE);
        const int32_t center_x =
            (center_x_q2 + H2_STARBOY_SUBPIXEL_SCALE / 2) /
            H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t center_y =
            (center_y_q2 + H2_STARBOY_SUBPIXEL_SCALE / 2) /
            H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t horizontal_tangent_x_q15 =
            starboy_multiply_q15(yaw_cosine_q15, anchor_z_q15) -
            starboy_multiply_q15(
                yaw_sine_q15, anchor_x_q15[eye_index]);
        const int32_t horizontal_tangent_z_q15 =
            -starboy_multiply_q15(yaw_sine_q15, anchor_z_q15) -
            starboy_multiply_q15(
                yaw_cosine_q15, anchor_x_q15[eye_index]);
        const int32_t horizontal_tangent_y_q15 =
            -starboy_multiply_q15(
                pitch_sine_q15, horizontal_tangent_z_q15);
        int32_t tilt_q15 = (int32_t)(
            (int64_t)behavior->pose_tilt_q15 * idle_pose_weight_q15 /
            H2_STARBOY_Q15_ONE);
        const int32_t wing_tilt_q15 =
            H2_STARBOY_WING_TILT_Q15 + behavior->wing_flutter_q15;
        tilt_q15 += eye_index == 0
            ? -wing_tilt_q15
            : wing_tilt_q15;
        if (starboy_abs_i32(horizontal_tangent_x_q15) > 1000) {
            tilt_q15 += (int32_t)(
                (int64_t)horizontal_tangent_y_q15 *
                H2_STARBOY_Q15_ONE / horizontal_tangent_x_q15);
        }
        tilt_q15 += eye_index == 0 ? asymmetry / 3 : -asymmetry / 4;
        tilt_q15 = starboy_clamp_i32(tilt_q15, -14000, 14000);
        const int32_t curvature_q15 = (int32_t)(
            -(int64_t)yawed_x_q15 * 5000 / H2_STARBOY_Q15_ONE);
        int32_t pupil_radius_x =
            eye_radius_x * pupil_radius_x_percent / 100;
        int32_t pupil_radius_y =
            eye_radius_y * pupil_radius_y_percent / 100;
        const int32_t anxious_pupil_scale_q15 =
            H2_STARBOY_Q15_ONE - anxious_mix_q15 / 4;
        pupil_radius_x = (int32_t)(
            (int64_t)pupil_radius_x * anxious_pupil_scale_q15 /
            H2_STARBOY_Q15_ONE);
        pupil_radius_y = (int32_t)(
            (int64_t)pupil_radius_y * anxious_pupil_scale_q15 /
            H2_STARBOY_Q15_ONE);
        const int32_t inward_offset_x = eye_radius_x * 21 / 100;
        int32_t pupil_travel_x =
            eye_radius_x * 92 / 100 - pupil_radius_x - inward_offset_x;
        pupil_travel_x = starboy_clamp_i32(
            pupil_travel_x, eye_radius_x * 4 / 100,
            eye_radius_x * 44 / 100);
        const int32_t pupil_offset_x = (int32_t)(
            (int64_t)local_pupil_x_q15 * pupil_travel_x /
            H2_STARBOY_Q15_ONE) +
            (eye_index == 0 ? inward_offset_x : -inward_offset_x);
        int32_t pupil_travel_y =
            eye_radius_y * 90 / 100 - pupil_radius_y;
        pupil_travel_y = starboy_clamp_i32(
            pupil_travel_y, eye_radius_y * 4 / 100,
            eye_radius_y * 40 / 100);
        const int32_t pupil_offset_y = (int32_t)(
            (int64_t)local_pupil_y_q15 * pupil_travel_y /
            H2_STARBOY_Q15_ONE) +
            eye_radius_y * pupil_offset_y_percent / 100;
        const int32_t eye_radius_x_scaled =
            eye_radius_x * H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t eye_radius_y_scaled =
            eye_radius_y * H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t pupil_radius_x_scaled =
            pupil_radius_x * H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t pupil_radius_y_scaled = (int32_t)(
            (int64_t)pupil_radius_y * openness_q15 /
            H2_STARBOY_Q15_ONE) * H2_STARBOY_SUBPIXEL_SCALE;

        const int32_t extent_x = eye_radius_x +
            (int32_t)((int64_t)starboy_abs_i32(tilt_q15) * eye_radius_y /
                      H2_STARBOY_Q15_ONE) + 2;
        const int32_t extent_y = eye_radius_y +
            (int32_t)((int64_t)starboy_abs_i32(tilt_q15) * eye_radius_x /
                      H2_STARBOY_Q15_ONE) +
            (int32_t)((int64_t)starboy_abs_i32(curvature_q15) *
                      eye_radius_x / H2_STARBOY_Q15_ONE) + 2;
        const int32_t pupil_extent_x = pupil_radius_x +
            (int32_t)((int64_t)starboy_abs_i32(tilt_q15) *
                      pupil_radius_y / H2_STARBOY_Q15_ONE) + 2;
        const int32_t pupil_extent_y = pupil_radius_y +
            (int32_t)((int64_t)starboy_abs_i32(tilt_q15) *
                      pupil_radius_x / H2_STARBOY_Q15_ONE) +
            (int32_t)((int64_t)starboy_abs_i32(curvature_q15 / 2) *
                      pupil_radius_x / H2_STARBOY_Q15_ONE) + 2;
        const int32_t pupil_center_x_q2 = center_x_q2 + (int32_t)(
            ((int64_t)right_x_q15 * pupil_offset_x +
             (int64_t)down_x_q15 * pupil_offset_y) *
            H2_STARBOY_SUBPIXEL_SCALE / H2_STARBOY_Q15_ONE);
        const int32_t pupil_center_y_q2 = center_y_q2 + (int32_t)(
            ((int64_t)right_y_q15 * pupil_offset_x +
             (int64_t)down_y_q15 * pupil_offset_y) *
            H2_STARBOY_SUBPIXEL_SCALE / H2_STARBOY_Q15_ONE);
        const int32_t pupil_center_x =
            (pupil_center_x_q2 + H2_STARBOY_SUBPIXEL_SCALE / 2) /
            H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t pupil_center_y =
            (pupil_center_y_q2 + H2_STARBOY_SUBPIXEL_SCALE / 2) /
            H2_STARBOY_SUBPIXEL_SCALE;
        const int32_t world_extent_x = (int32_t)(
            ((int64_t)starboy_abs_i32(right_x_q15) * extent_x +
             (int64_t)starboy_abs_i32(down_x_q15) * extent_y) /
            H2_STARBOY_Q15_ONE) + 2;
        const int32_t world_extent_y = (int32_t)(
            ((int64_t)starboy_abs_i32(right_y_q15) * extent_x +
             (int64_t)starboy_abs_i32(down_y_q15) * extent_y) /
            H2_STARBOY_Q15_ONE) + 2;
        const int32_t world_pupil_extent_x = (int32_t)(
            ((int64_t)starboy_abs_i32(right_x_q15) * pupil_extent_x +
             (int64_t)starboy_abs_i32(down_x_q15) * pupil_extent_y) /
            H2_STARBOY_Q15_ONE) + 2;
        const int32_t world_pupil_extent_y = (int32_t)(
            ((int64_t)starboy_abs_i32(right_y_q15) * pupil_extent_x +
             (int64_t)starboy_abs_i32(down_y_q15) * pupil_extent_y) /
            H2_STARBOY_Q15_ONE) + 2;
        eyes[eye_index] = (starboy_eye_geometry_t){
            .center_x = center_x_q2,
            .center_y = center_y_q2,
            .radius_x = eye_radius_x_scaled,
            .radius_y = eye_radius_y_scaled,
            .tilt_q15 = tilt_q15,
            .curvature_q15 = curvature_q15,
            .right_x_q15 = right_x_q15,
            .right_y_q15 = right_y_q15,
            .down_x_q15 = down_x_q15,
            .down_y_q15 = down_y_q15,
            .pupil_x = pupil_center_x_q2,
            .pupil_y = pupil_center_y_q2,
            .pupil_radius_x = pupil_radius_x_scaled,
            .pupil_radius_y = pupil_radius_y_scaled,
            .pupil_min_x = pupil_center_x - world_pupil_extent_x,
            .pupil_max_x = pupil_center_x + world_pupil_extent_x,
            .pupil_min_y = pupil_center_y - world_pupil_extent_y,
            .pupil_max_y = pupil_center_y + world_pupil_extent_y,
            .min_x = center_x - world_extent_x,
            .max_x = center_x + world_extent_x,
            .min_y = center_y - world_extent_y,
            .max_y = center_y + world_extent_y,
            .star_mix_q15 = star_mix_q15,
            .star_phase_q8 = star_phase_q8,
            .pupil_inward_sign = eye_index == 0 ? 1 : -1,
            .pupil_style_mix_q15 = behavior->pupil_style_mix_q15,
            .pupil_style = behavior->pupil_style,
            .previous_pupil_style = behavior->previous_pupil_style,
        };
    }

    const int show_pupil = openness_q15 > H2_STARBOY_Q15_ONE / 5;
    int32_t min_x = eyes[0].min_x < eyes[1].min_x
        ? eyes[0].min_x
        : eyes[1].min_x;
    int32_t max_x = eyes[0].max_x > eyes[1].max_x
        ? eyes[0].max_x
        : eyes[1].max_x;
    int32_t min_y = eyes[0].min_y < eyes[1].min_y
        ? eyes[0].min_y
        : eyes[1].min_y;
    int32_t max_y = eyes[0].max_y > eyes[1].max_y
        ? eyes[0].max_y
        : eyes[1].max_y;
    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= (int32_t)width) {
        max_x = (int32_t)width - 1;
    }
    if (max_y >= (int32_t)height) {
        max_y = (int32_t)height - 1;
    }

    const h2_display_rect_t content_rect = {
        .x = min_x,
        .y = min_y,
        .width = max_x - min_x + 1,
        .height = max_y - min_y + 1,
    };
    const h2_display_rect_t full_frame_rect = {
        .x = 0,
        .y = 0,
        .width = (int)width,
        .height = (int)height,
    };
    for (size_t eye_index = 0u;
         eye_index < H2_STARBOY_EYE_COUNT;
         ++eye_index) {
        int32_t eye_min_x = starboy_clamp_i32(
            eyes[eye_index].min_x, 0, (int32_t)width - 1);
        int32_t eye_min_y = starboy_clamp_i32(
            eyes[eye_index].min_y, 0, (int32_t)height - 1);
        int32_t eye_max_x = starboy_clamp_i32(
            eyes[eye_index].max_x, 0, (int32_t)width - 1);
        int32_t eye_max_y = starboy_clamp_i32(
            eyes[eye_index].max_y, 0, (int32_t)height - 1);
        out_regions->eye_content_rects[eye_index] = (h2_display_rect_t){
            .x = eye_min_x,
            .y = eye_min_y,
            .width = eye_max_x - eye_min_x + 1,
            .height = eye_max_y - eye_min_y + 1,
        };
    }
    out_regions->content_rect = content_rect;
    if (previous_regions == NULL) {
        out_regions->dirty_rects[0] = full_frame_rect;
        out_regions->dirty_rect_count = 1u;
    } else {
        for (size_t eye_index = 0u;
             eye_index < H2_STARBOY_EYE_COUNT;
             ++eye_index) {
            out_regions->dirty_rects[eye_index] = starboy_union_rect(
                &previous_regions->eye_content_rects[eye_index],
                &out_regions->eye_content_rects[eye_index]);
        }
        out_regions->dirty_rect_count = H2_STARBOY_EYE_COUNT;
        const h2_display_rect_t merged = starboy_union_rect(
            &out_regions->dirty_rects[0],
            &out_regions->dirty_rects[1]);
        if (starboy_rects_overlap(
                &out_regions->dirty_rects[0],
                &out_regions->dirty_rects[1])) {
            out_regions->dirty_rects[0] = merged;
            out_regions->dirty_rects[1] = (h2_display_rect_t){0};
            out_regions->dirty_rect_count = 1u;
        }
    }
    for (size_t dirty_index = 0u;
         dirty_index < out_regions->dirty_rect_count;
         ++dirty_index) {
        starboy_clear_rect(
            pixels, width, &out_regions->dirty_rects[dirty_index]);
    }

    const uint16_t eye_color = starboy_sample_color(
        STARBOY_SAMPLE_EYE, behavior);
    const uint16_t pupil_color = starboy_sample_color(
        STARBOY_SAMPLE_PUPIL, behavior);
    for (int eye_index = 0; eye_index < 2; ++eye_index) {
        starboy_render_shape(
            pixels,
            width,
            height,
            &eyes[eye_index],
            STARBOY_SHAPE_EYE,
            eye_color);
    }
    if (show_pupil) {
        for (int eye_index = 0; eye_index < 2; ++eye_index) {
            starboy_render_shape(
                pixels,
                width,
                height,
                &eyes[eye_index],
                STARBOY_SHAPE_PUPIL,
                pupil_color);
        }
    }

}

h2_display_rect_t h2_starboy_render(
    uint16_t *pixels,
    uint32_t width,
    uint32_t height,
    const h2_starboy_behavior_t *behavior,
    const h2_display_rect_t *previous_content_rect,
    h2_display_rect_t *out_content_rect) {
    if (out_content_rect == NULL) {
        return (h2_display_rect_t){0};
    }
    h2_starboy_render_regions_t previous_regions = {0};
    const h2_starboy_render_regions_t *previous_regions_pointer = NULL;
    if (previous_content_rect != NULL) {
        previous_regions.content_rect = *previous_content_rect;
        for (size_t eye_index = 0u;
             eye_index < H2_STARBOY_EYE_COUNT;
             ++eye_index) {
            previous_regions.eye_content_rects[eye_index] =
                *previous_content_rect;
        }
        previous_regions_pointer = &previous_regions;
    }
    h2_starboy_render_regions_t regions = {0};
    h2_starboy_render_dirty_regions(
        pixels,
        width,
        height,
        behavior,
        previous_regions_pointer,
        &regions);
    if (out_content_rect != NULL) {
        *out_content_rect = regions.content_rect;
    }
    if (regions.dirty_rect_count == 0u) {
        return (h2_display_rect_t){0};
    }
    h2_display_rect_t dirty_rect = regions.dirty_rects[0];
    for (size_t index = 1u; index < regions.dirty_rect_count; ++index) {
        dirty_rect = starboy_union_rect(
            &dirty_rect, &regions.dirty_rects[index]);
    }
    return dirty_rect;
}
