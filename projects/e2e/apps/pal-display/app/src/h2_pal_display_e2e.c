#include "h2_pal_display_e2e.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct state {
    h2_runtime_t *runtime;
    const h2_pal_mem_api_t *mem;
    const h2_pal_display_e2e_config_t *config;
    h2_pal_display_e2e_result_t *result;
    h2_display_info_t info;
    uint16_t *expected;
    const char *case_id;
    uint32_t brightness;
} state_t;
#define REQUIRE(expr)                                                          \
    do {                                                                       \
        if (!(expr))                                                           \
            return H2_DISPLAY_ERR_IO;                                          \
    } while (0)
#define OK(expr)                                                               \
    do {                                                                       \
        int rc_ = (expr);                                                      \
        if (rc_)                                                               \
            return rc_;                                                        \
    } while (0)
static const uint16_t palette[] = {0xf800, 0x07e0, 0x001f, 0xffff};
static const char *ids[] = {
#define H2_PAL_DISPLAY_CASE(id, fn) id,
#include "h2_pal_display_cases.inc"
#undef H2_PAL_DISPLAY_CASE
};
static uint16_t pattern(int x, int y, int width, int height) {
    if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
        return 0xffff;
    return palette[(x >= width / 2) + 2 * (y >= height / 2)];
}
static int observe(state_t *s) {
    OK(h2_pal_display_present(s->runtime->display));
    OK(s->config->observe(s->config->user, s->expected, s->info.width,
                          s->info.height, s->brightness, s->case_id));
    ++s->result->observations;
    return 0;
}
static int interface_inventory(state_t *s) {
    const h2_pal_display_vtable_t *v = s->runtime->display->vtable;
    REQUIRE(v && v->open && v->get_info && v->draw_bitmap && v->present &&
            v->set_brightness_percent && v->close);
    REQUIRE(s->config->supported_formats & (1u << H2_DISPLAY_PIXEL_RGB565));
    return 0;
}
static int null_arguments(state_t *s) {
    h2_display_info_t info;
    uint16_t pixel = 0;
    h2_display_rect_t rect = {0, 0, 1, 1};
    REQUIRE(h2_pal_display_open(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_get_info(NULL, &info) == H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_get_info(s->runtime->display, NULL) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_draw_bitmap(NULL, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, NULL, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, NULL, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_present(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_set_brightness_percent(NULL, 100) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_close(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
    return 0;
}
static int closed_state(state_t *s) {
    h2_display_info_t info = {0};
    h2_display_rect_t rect = {0, 0, 1, 1};
    uint16_t pixel = 0;
    REQUIRE(h2_pal_display_get_info(s->runtime->display, &info) ==
            H2_DISPLAY_ERR_INVALID_STATE);
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_STATE);
    REQUIRE(h2_pal_display_present(s->runtime->display) ==
            H2_DISPLAY_ERR_INVALID_STATE);
    REQUIRE(h2_pal_display_set_brightness_percent(s->runtime->display, 100) ==
            H2_DISPLAY_ERR_INVALID_STATE);
    return 0;
}
static int open_idempotent(state_t *s) {
    OK(h2_pal_display_open(s->runtime->display));
    return h2_pal_display_open(s->runtime->display);
}
static int native_info(state_t *s) {
    OK(h2_pal_display_get_info(s->runtime->display, &s->info));
    REQUIRE(s->info.width >= 16 && s->info.height >= 16 &&
            s->info.width <= 4096 && s->info.height <= 4096 &&
            s->info.native_format == H2_DISPLAY_PIXEL_RGB565);
    size_t count = (size_t)s->info.width * (size_t)s->info.height;
    s->expected = h2_pal_mem_alloc(s->mem, count * sizeof(uint16_t));
    REQUIRE(s->expected != NULL);
    for (int y = 0; y < s->info.height; ++y)
        for (int x = 0; x < s->info.width; ++x)
            s->expected[(size_t)y * s->info.width + x] =
                pattern(x, y, s->info.width, s->info.height);
    s->brightness = 100;
    return h2_pal_display_set_brightness_percent(s->runtime->display, 100);
}
static int full_frame_borrowed(state_t *s) {
    size_t bytes = (size_t)s->info.width * s->info.height * sizeof(uint16_t);
    void *input = h2_pal_mem_alloc(s->mem, bytes);
    REQUIRE(input != NULL);
    memcpy(input, s->expected, bytes);
    h2_display_rect_t rect = {0, 0, s->info.width, s->info.height};
    int rc = h2_pal_display_draw_bitmap(s->runtime->display, &rect, input,
                                        (size_t)s->info.width * 2,
                                        H2_DISPLAY_PIXEL_RGB565);
    /* Release input before present: borrowed memory cannot be retained. */
    memset(input, 0, bytes);
    h2_pal_mem_free(s->mem, input);
    OK(rc);
    return observe(s);
}
static int patch(state_t *s, h2_display_pixel_format_t format, size_t padding) {
    const h2_display_rect_t rect = {2, 3, 4, 2};
    const size_t pixel_size = format == H2_DISPLAY_PIXEL_RGB888 ? 3 : 2;
    const size_t stride = 4 * pixel_size + padding;
    uint8_t storage[64];
    memset(storage, 0xa5, sizeof(storage));
    uint8_t *input = storage + 1; /* explicit unaligned borrowed source */
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t *p = input + (size_t)y * stride + (size_t)x * pixel_size;
            if (format == H2_DISPLAY_PIXEL_RGB888) {
                p[0] = x == 0 || x == 3 ? 255 : 0;
                p[1] = x == 1 || x == 3 ? 255 : 0;
                p[2] = x == 2 || x == 3 ? 255 : 0;
            } else {
                uint16_t pixel =
                    format == H2_DISPLAY_PIXEL_RGB444
                        ? (uint16_t[]){0x0f00, 0x00f0, 0x000f, 0x0fff}[x]
                        : palette[x];
                memcpy(p, &pixel, 2);
            }
        }
    int rc = h2_pal_display_draw_bitmap(s->runtime->display, &rect, input,
                                        stride, format);
    if (!(s->config->supported_formats & (1u << format))) {
        REQUIRE(rc == H2_DISPLAY_ERR_UNSUPPORTED);
    } else {
        OK(rc);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
                s->expected[(size_t)(3 + y) * s->info.width + 2 + x] =
                    palette[x];
    }
    memset(storage, 0, sizeof(storage));
    return observe(s);
}
static int partial_rgb565(state_t *s) {
    return patch(s, H2_DISPLAY_PIXEL_RGB565, 0);
}
static int padded_unaligned_stride(state_t *s) {
    return patch(s, H2_DISPLAY_PIXEL_RGB565, 5);
}
static int rgb888(state_t *s) { return patch(s, H2_DISPLAY_PIXEL_RGB888, 3); }
static int rgb444(state_t *s) { return patch(s, H2_DISPLAY_PIXEL_RGB444, 3); }
static int unknown_format(state_t *s) {
    uint16_t pixel = 0;
    h2_display_rect_t rect = {0, 0, 1, 1};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       (h2_display_pixel_format_t)99) ==
            H2_DISPLAY_ERR_UNSUPPORTED);
    return observe(s);
}
static int empty_rectangles(state_t *s) {
    uint16_t pixel = 0;
    h2_display_rect_t rect = {0, 0, 0, 1};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    rect.width = 1;
    rect.height = -1;
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int short_stride(state_t *s) {
    uint16_t pixels[2] = {0};
    h2_display_rect_t rect = {0, 0, 2, 1};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, pixels, 3,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int outside_rectangles(state_t *s) {
    uint16_t pixel = 0;
    h2_display_rect_t rect = {s->info.width, 0, 1, 1};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    rect.x = -1;
    rect.y = -1;
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel, 2,
                                       H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int partial_bounds(state_t *s) {
    uint16_t pixels[4] = {0, 0xf800, 0x07e0, 0xffff};
    h2_display_rect_t rect = {-1, 0, 2, 2};
    int rc = h2_pal_display_draw_bitmap(s->runtime->display, &rect, pixels, 4,
                                        H2_DISPLAY_PIXEL_RGB565);
    if (s->config->clips_rectangles) {
        OK(rc);
        s->expected[0] = 0xf800;
        s->expected[s->info.width] = 0xffff;
    } else
        REQUIRE(rc == H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int coordinate_overflow(state_t *s) {
    uint16_t pixel = 0;
    h2_display_rect_t rect = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel,
                                       SIZE_MAX, H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int span_overflow(state_t *s) {
    uint16_t pixel = 0;
    h2_display_rect_t rect = {0, 0, 1, 3};
    REQUIRE(h2_pal_display_draw_bitmap(s->runtime->display, &rect, &pixel,
                                       SIZE_MAX, H2_DISPLAY_PIXEL_RGB565) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int repeated_present(state_t *s) {
    for (int i = 0; i < 5; ++i)
        OK(observe(s));
    return 0;
}
static int brightness(state_t *s, uint32_t value) {
    OK(h2_pal_display_set_brightness_percent(s->runtime->display, value));
    s->brightness = value;
    return observe(s);
}
static int brightness_zero(state_t *s) { return brightness(s, 0); }
static int brightness_half(state_t *s) { return brightness(s, 50); }
static int brightness_full(state_t *s) { return brightness(s, 100); }
static int brightness_invalid(state_t *s) {
    REQUIRE(h2_pal_display_set_brightness_percent(s->runtime->display, 101) ==
            H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(h2_pal_display_set_brightness_percent(
                s->runtime->display, UINT32_MAX) == H2_DISPLAY_ERR_INVALID_ARG);
    REQUIRE(s->runtime->display->vtable->set_brightness_percent(
                s->runtime->display->user, 101) == H2_DISPLAY_ERR_INVALID_ARG);
    return observe(s);
}
static int close_idempotent(state_t *s) {
    OK(h2_pal_display_close(s->runtime->display));
    OK(closed_state(s));
    return h2_pal_display_close(s->runtime->display);
}
static int reopen(state_t *s) {
    for (int i = 0; i < 2; ++i) {
        OK(open_idempotent(s));
        h2_display_info_t info = {0};
        OK(h2_pal_display_get_info(s->runtime->display, &info));
        REQUIRE(info.width == s->info.width && info.height == s->info.height &&
                info.native_format == s->info.native_format);
        /* Restore independent expected image, not a previous surface cache. */
        OK(h2_pal_display_set_brightness_percent(s->runtime->display, 100));
        OK(full_frame_borrowed(s));
        OK(h2_pal_display_close(s->runtime->display));
        OK(closed_state(s));
    }
    return 0;
}
static int (*const functions[])(state_t *) = {
#define H2_PAL_DISPLAY_CASE(id, fn) fn,
#include "h2_pal_display_cases.inc"
#undef H2_PAL_DISPLAY_CASE
};
int h2_pal_display_e2e_run(h2_runtime_t *runtime,
                           const h2_pal_display_e2e_config_t *config,
                           h2_pal_display_e2e_result_t *result) {
    if (!result)
        return H2_DISPLAY_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    for (size_t i = 0; i < H2_PAL_DISPLAY_E2E_CASE_COUNT; ++i)
        result->cases[i].id = ids[i];
    result->not_run = H2_PAL_DISPLAY_E2E_CASE_COUNT;
    if (!runtime || !runtime->mem || !runtime->display || !config ||
        !config->observe)
        return H2_DISPLAY_ERR_INVALID_ARG;
    state_t s = {.runtime = runtime,
                 .mem =
                     config->working_mem ? config->working_mem : runtime->mem,
                 .config = config,
                 .result = result};
    int rc = 0;
    for (size_t i = 0; i < H2_PAL_DISPLAY_E2E_CASE_COUNT; ++i) {
        s.case_id = ids[i];
        rc = functions[i](&s);
        result->cases[i].ran = 1;
        result->cases[i].result = rc;
        --result->not_run;
        if (rc) {
            ++result->failed;
            break;
        }
        ++result->passed;
    }
    result->cleanup = h2_pal_display_close(runtime->display);
    h2_pal_mem_free(s.mem, s.expected);
    result->complete = result->not_run == 0;
    result->qualified = result->complete && !result->failed && !result->cleanup;
    return rc ? rc : result->cleanup;
}
void h2_pal_display_e2e_print(const h2_pal_display_e2e_result_t *r,
                              const char *platform, int rc, int teardown) {
    for (size_t i = 0; i < H2_PAL_DISPLAY_E2E_CASE_COUNT; ++i)
        printf("H2_DISPLAY_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n",
               r->cases[i].id ? r->cases[i].id : ids[i],
               r->cases[i].ran ? (r->cases[i].result ? "FAIL" : "PASS")
                               : "NOT_RUN",
               r->cases[i].result);
    printf(
        "H2_DISPLAY_REPORT "
        "{\"platform\":\"%s\",\"contract\":1,\"operations\":6,\"passed\":%zu,"
        "\"failed\":%zu,\"not_run\":%zu,\"observations\":%zu,\"complete\":%d,"
        "\"qualified\":%d,\"rc\":%d,\"cleanup\":%d,\"teardown\":%d}\n",
        platform, r->passed, r->failed, r->not_run, r->observations,
        r->complete, r->qualified, rc, r->cleanup, teardown);
}
int h2_pal_display_e2e_compare(const uint8_t *actual, size_t stride, int bgr,
                               const uint16_t *expected, int width, int height,
                               uint32_t brightness, unsigned tolerance) {
    if (!actual || !expected || width < 1 || height < 1 || brightness > 100 ||
        stride < (size_t)width * 4)
        return H2_DISPLAY_ERR_INVALID_ARG;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint16_t p = expected[(size_t)y * width + x];
            unsigned rgb[3] = {((p >> 11) & 31) * 255u / 31u,
                               ((p >> 5) & 63) * 255u / 63u,
                               (p & 31) * 255u / 31u};
            const uint8_t *a = actual + (size_t)y * stride + (size_t)x * 4;
            for (int c = 0; c < 3; ++c) {
                int diff =
                    (int)a[bgr ? 2 - c : c] - (int)(rgb[c] * brightness / 100);
                if (diff < 0)
                    diff = -diff;
                if ((unsigned)diff > tolerance) {
                    printf("H2_DISPLAY_PIXEL_FAIL x=%d y=%d channel=%d "
                           "actual=%u expected=%u brightness=%u\n",
                           x, y, c, a[bgr ? 2 - c : c],
                           (unsigned)(rgb[c] * brightness / 100),
                           (unsigned)brightness);
                    return H2_DISPLAY_ERR_IO;
                }
            }
        }
    return 0;
}
