#ifndef H2_BK7258_DISPLAY_BUFFER_H
#define H2_BK7258_DISPLAY_BUFFER_H

#include "h2/pal/hal/h2_pal_display.h"
#include "h2_atomic.h"
#include <string.h>

/* The board retains each pixel allocation until Display close. The SDK owns
 * only its immutable submission interval; its release callback returns the
 * slot to the single Display writer. Atomic backing lives in internal RAM. */
typedef struct h2_bk7258_display_buffer {
    uint16_t *pixels;
    h2_atomic_bool_t *available;
    h2_display_rect_t dirty;
    bool dirty_valid;
} h2_bk7258_display_buffer_t;

static inline void h2_bk7258_display_buffer_init(
    h2_bk7258_display_buffer_t *buffer, uint16_t *pixels,
    h2_atomic_bool_t *available, int32_t width, int32_t height) {
    buffer->pixels = pixels;
    buffer->available = available;
    buffer->dirty = (h2_display_rect_t){0, 0, width, height};
    buffer->dirty_valid = true;
    h2_atomic_bool_store(available, true, H2_ATOMIC_RELEASE);
}

static inline void h2_bk7258_display_buffer_dirty(
    h2_bk7258_display_buffer_t *buffer, const h2_display_rect_t *rect) {
    if (!buffer->dirty_valid) {
        buffer->dirty = *rect;
        buffer->dirty_valid = true;
        return;
    }
    int32_t right = buffer->dirty.x + buffer->dirty.width;
    int32_t bottom = buffer->dirty.y + buffer->dirty.height;
    if (rect->x + rect->width > right) right = rect->x + rect->width;
    if (rect->y + rect->height > bottom) bottom = rect->y + rect->height;
    if (rect->x < buffer->dirty.x) buffer->dirty.x = rect->x;
    if (rect->y < buffer->dirty.y) buffer->dirty.y = rect->y;
    buffer->dirty.width = right - buffer->dirty.x;
    buffer->dirty.height = bottom - buffer->dirty.y;
}

static inline bool h2_bk7258_display_buffer_claim(
    h2_bk7258_display_buffer_t *buffer) {
    bool expected = true;
    return h2_atomic_bool_compare_exchange(buffer->available, &expected, false,
                                           H2_ATOMIC_ACQUIRE, H2_ATOMIC_RELAXED);
}

static inline void h2_bk7258_display_buffer_release(
    h2_bk7258_display_buffer_t *buffer) {
    h2_atomic_bool_store(buffer->available, true, H2_ATOMIC_RELEASE);
}

/* Only the successful claimant may write pixels. Dirty regions accumulate
 * separately for both slots, including changes while a slot is scanning. */
static inline size_t h2_bk7258_display_buffer_copy(
    h2_bk7258_display_buffer_t *buffer, const uint16_t *shadow, size_t width) {
    if (!buffer->dirty_valid) return 0u;
    const h2_display_rect_t *rect = &buffer->dirty;
    size_t bytes = (size_t)rect->width * sizeof(uint16_t);
    for (int32_t y = rect->y; y < rect->y + rect->height; ++y) {
        size_t offset = (size_t)y * width + (size_t)rect->x;
        memcpy(buffer->pixels + offset, shadow + offset, bytes);
    }
    buffer->dirty_valid = false;
    return bytes * (size_t)rect->height;
}

#endif
