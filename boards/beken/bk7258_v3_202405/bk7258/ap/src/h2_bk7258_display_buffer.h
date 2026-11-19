#ifndef H2_BK7258_DISPLAY_BUFFER_H
#define H2_BK7258_DISPLAY_BUFFER_H

#include "hal/h2_pal_display.h"
#include "h2_atomic.h"
#include <string.h>

/* Dirty rows are tracked per 16-row band, each with its own row and column
 * range, so separate changes on one frame never merge into one large copy.
 * Rows at or beyond the last band fold into it, which stays correct for any
 * height and only loses precision there. */
#define H2_BK7258_DISPLAY_BAND_ROWS 16
#define H2_BK7258_DISPLAY_MAX_BANDS 64u

/* Half-open row and column range of the changes inside one band. */
typedef struct h2_bk7258_display_band {
  int16_t x0, x1, y0, y1;
} h2_bk7258_display_band_t;

/* The board retains each pixel allocation until Display close. The SDK owns
 * only its immutable submission interval; its release callback returns the
 * slot to the single Display writer. Atomic backing lives in internal RAM. */
typedef struct h2_bk7258_display_buffer {
  uint16_t* pixels;
  h2_atomic_bool_t* available;
  uint64_t dirty_bands;
  h2_bk7258_display_band_t bands[H2_BK7258_DISPLAY_MAX_BANDS];
} h2_bk7258_display_buffer_t;

/* Record a clipped, in-bounds change. */
static inline void h2_bk7258_display_buffer_dirty(
    h2_bk7258_display_buffer_t* buffer, const h2_display_rect_t* rect) {
  if (rect->width <= 0 || rect->height <= 0) return;
  const int32_t bottom = rect->y + rect->height;
  const int16_t x0 = (int16_t)rect->x;
  const int16_t x1 = (int16_t)(rect->x + rect->width);
  for (int32_t y = rect->y; y < bottom;) {
    uint32_t index = (uint32_t)y / H2_BK7258_DISPLAY_BAND_ROWS;
    int32_t end = bottom;
    if (index >= H2_BK7258_DISPLAY_MAX_BANDS - 1u) {
      index = H2_BK7258_DISPLAY_MAX_BANDS - 1u;
    } else if ((int32_t)(index + 1u) * H2_BK7258_DISPLAY_BAND_ROWS < bottom) {
      end = (int32_t)(index + 1u) * H2_BK7258_DISPLAY_BAND_ROWS;
    }
    h2_bk7258_display_band_t* band = &buffer->bands[index];
    const uint64_t bit = (uint64_t)1u << index;
    if ((buffer->dirty_bands & bit) == 0u) {
      *band = (h2_bk7258_display_band_t){x0, x1, (int16_t)y, (int16_t)end};
      buffer->dirty_bands |= bit;
    } else {
      if (x0 < band->x0) band->x0 = x0;
      if (x1 > band->x1) band->x1 = x1;
      if (y < band->y0) band->y0 = (int16_t)y;
      if (end > band->y1) band->y1 = (int16_t)end;
    }
    y = end;
  }
}

static inline void h2_bk7258_display_buffer_init(
    h2_bk7258_display_buffer_t* buffer, uint16_t* pixels,
    h2_atomic_bool_t* available, int32_t width, int32_t height) {
  buffer->pixels = pixels;
  buffer->available = available;
  buffer->dirty_bands = 0u;
  const h2_display_rect_t full = {0, 0, width, height};
  h2_bk7258_display_buffer_dirty(buffer, &full);
  h2_atomic_bool_store(available, true, H2_ATOMIC_RELEASE);
}

static inline bool h2_bk7258_display_buffer_claim(
    h2_bk7258_display_buffer_t* buffer) {
  bool expected = true;
  return h2_atomic_bool_compare_exchange(buffer->available, &expected, false,
                                         H2_ATOMIC_ACQUIRE, H2_ATOMIC_RELAXED);
}

static inline void h2_bk7258_display_buffer_release(
    h2_bk7258_display_buffer_t* buffer) {
  h2_atomic_bool_store(buffer->available, true, H2_ATOMIC_RELEASE);
}

/* The serialized Display writer owns this stripe in internal SRAM. The LCD
 * and its release callback only access the retained scanout frames. */
typedef struct h2_bk7258_display_copy_scratch {
  uint32_t words[128];
} h2_bk7258_display_copy_scratch_t;

static inline void h2_bk7258_display_copy_aligned(void* dst, const void* src,
                                                  size_t bytes) {
#if defined(__GNUC__)
  enum { block_bytes = 32 };
  while (bytes >= block_bytes) {
    __builtin_memcpy(__builtin_assume_aligned(dst, 4),
                     __builtin_assume_aligned(src, 4), block_bytes);
    dst = (uint8_t*)dst + block_bytes;
    src = (const uint8_t*)src + block_bytes;
    bytes -= block_bytes;
  }
#endif
  if (bytes != 0u) memcpy(dst, src, bytes);
}

/* The two pixel arrays can share the uncached PSRAM bus. Stage whole
 * stripes so reads from and writes to PSRAM run in separate bursts. */
static inline void h2_bk7258_display_copy_pixels(
    h2_bk7258_display_copy_scratch_t* scratch, void* dst, const void* src,
    size_t bytes) {
  if (scratch == NULL || (((uintptr_t)dst | (uintptr_t)src) & 3u) != 0u) {
    memcpy(dst, src, bytes);
    return;
  }
  while (bytes >= sizeof(*scratch)) {
    h2_bk7258_display_copy_aligned(scratch, src, sizeof(*scratch));
    h2_bk7258_display_copy_aligned(dst, scratch, sizeof(*scratch));
    dst = (uint8_t*)dst + sizeof(*scratch);
    src = (const uint8_t*)src + sizeof(*scratch);
    bytes -= sizeof(*scratch);
  }
  h2_bk7258_display_copy_aligned(dst, src, bytes);
}

/* Only the successful claimant may write pixels. Dirty bands accumulate
 * separately for both slots, including changes while a slot is scanning.
 * Returns the copied byte count. */
static inline size_t h2_bk7258_display_buffer_copy(
    h2_bk7258_display_buffer_t* buffer, const uint16_t* shadow, size_t width,
    h2_bk7258_display_copy_scratch_t* scratch) {
  size_t total = 0u;
  uint64_t pending = buffer->dirty_bands;
  for (uint32_t index = 0u; pending != 0u; ++index, pending >>= 1) {
    if ((pending & 1u) == 0u) continue;
    const h2_bk7258_display_band_t* band = &buffer->bands[index];
    const size_t bytes = (size_t)(band->x1 - band->x0) * sizeof(uint16_t);
    for (int32_t y = band->y0; y < band->y1; ++y) {
      const size_t offset = (size_t)y * width + (size_t)band->x0;
      h2_bk7258_display_copy_pixels(scratch, buffer->pixels + offset,
                                    shadow + offset, bytes);
    }
    total += bytes * (size_t)(band->y1 - band->y0);
  }
  buffer->dirty_bands = 0u;
  return total;
}

#endif
