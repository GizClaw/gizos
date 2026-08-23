#ifndef H2_GAME_TEXT_H
#define H2_GAME_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GAME_TEXT_OK 0
#define H2_GAME_TEXT_ERR_INVALID_ARGUMENT -1
#define H2_GAME_TEXT_ERR_INVALID_UTF8 -2
#define H2_GAME_TEXT_ERR_UNSUPPORTED_GLYPH -3
#define H2_GAME_TEXT_ERR_OVERFLOW -4
#define H2_GAME_TEXT_ERR_PROVIDER -5

/** A borrowed, length-delimited UTF-8 string. It need not be NUL-terminated. */
typedef struct h2_game_text_span {
    const char *data;
    size_t byte_len;
} h2_game_text_span_t;

#define H2_GAME_TEXT_LITERAL(value) { (value), sizeof(value) - 1u }

/** Text measurements in pixels. The origin used by draw is the top-left corner. */
typedef struct h2_game_text_metrics {
    int32_t width_px;
    int32_t advance_px;
    int32_t height_px;
    int32_t baseline_px;
} h2_game_text_metrics_t;

typedef struct h2_game_text_style {
    uint16_t color_rgb565;
    uint16_t line_height_px;
} h2_game_text_style_t;

/** Borrowed RGB565 target. pixel_capacity protects providers from invalid geometry. */
typedef struct h2_game_text_surface {
    uint16_t *pixels;
    size_t width_px;
    size_t height_px;
    size_t stride_pixels;
    size_t pixel_capacity;
} h2_game_text_surface_t;

typedef struct h2_game_text_vtable {
    int (*measure)(
        void *user,
        h2_game_text_span_t text,
        uint16_t line_height_px,
        h2_game_text_metrics_t *out_metrics);
    int (*draw)(
        void *user,
        const h2_game_text_surface_t *surface,
        h2_game_text_span_t text,
        int32_t x,
        int32_t y,
        h2_game_text_style_t style);
} h2_game_text_vtable_t;

/**
 * Borrowed synchronous provider. The host owns user and all provider resources.
 * Calls are made only from the game render thread and never retained.
 */
typedef struct h2_game_text_api {
    void *user;
    const h2_game_text_vtable_t *vtable;
} h2_game_text_api_t;

int h2_game_text_validate_utf8(h2_game_text_span_t text);
int h2_game_text_measure(
    const h2_game_text_api_t *api,
    h2_game_text_span_t text,
    uint16_t line_height_px,
    h2_game_text_metrics_t *out_metrics);
int h2_game_text_draw(
    const h2_game_text_api_t *api,
    const h2_game_text_surface_t *surface,
    h2_game_text_span_t text,
    int32_t x,
    int32_t y,
    h2_game_text_style_t style);

/** Stateless ASCII provider matching PixelRoot32's existing 5x7 font. */
h2_game_text_api_t h2_game_text_builtin_5x7(void);

#ifdef __cplusplus
}
#endif

#endif
