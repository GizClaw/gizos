#ifndef BUFFET_DAMAGE_H
#define BUFFET_DAMAGE_H

#include "buffet_game.h"
#include "buffet_scale.h"
#include <string.h>

/* The compact display can overlap a pixel transfer with the next frame.
 * Restrict the number of EMI window setups, especially for small updates. */
enum { BF_DAMAGE_ZONES = 3, BF_DAMAGE_BLOCK = 16 };

static inline uint32_t bf_pixel_pair(const uint16_t* pixels) {
  uint32_t word;
  memcpy(&word, pixels, sizeof(word));
  return word;
}

/* Equality only. All scan boundaries are whole blocks. Four pairs per batch
 * let the 32-bit CPU compare contiguous RGB565 words without aliasing casts. */
static inline bool bf_same_pixels(const uint16_t* a, const uint16_t* b, int n) {
  for (int x = 0; x < n; x += 8) {
    uint32_t differing = (bf_pixel_pair(a + x) ^ bf_pixel_pair(b + x)) |
                         (bf_pixel_pair(a + x + 2) ^ bf_pixel_pair(b + x + 2)) |
                         (bf_pixel_pair(a + x + 4) ^ bf_pixel_pair(b + x + 4)) |
                         (bf_pixel_pair(a + x + 6) ^ bf_pixel_pair(b + x + 6));
    if (differing) return false;
  }
  return true;
}

static inline bf_rect_t bf_damage_band(const uint16_t* frame,
                                       const uint16_t* previous, int start,
                                       int end) {
  const int width = BF_FRAME_W;
  while (start < end &&
         bf_same_pixels(frame + start * width, previous + start * width, width))
    ++start;
  while (start < end && bf_same_pixels(frame + (end - 1) * width,
                                       previous + (end - 1) * width, width))
    --end;
  if (start == end) return (bf_rect_t){0};
  int left = width, right = 0;
  for (int y = start; y < end && (left || right < width); ++y) {
    const uint16_t* a = frame + y * width;
    const uint16_t* b = previous + y * width;
    /* Once an interval is covered, only its two exterior strips can expand
     * it. Keep exact coverage with coarse columns for fewer comparisons. */
    if (left && !bf_same_pixels(a, b, left)) {
      for (int x = 0; x < left; x += BF_DAMAGE_BLOCK) {
        if (!bf_same_pixels(a + x, b + x, BF_DAMAGE_BLOCK)) {
          left = x;
          break;
        }
      }
    }
    if (right < width && !bf_same_pixels(a + right, b + right, width - right)) {
      for (int x = width; x > right; x -= BF_DAMAGE_BLOCK) {
        if (!bf_same_pixels(a + x - BF_DAMAGE_BLOCK, b + x - BF_DAMAGE_BLOCK,
                            BF_DAMAGE_BLOCK)) {
          right = x;
          break;
        }
      }
    }
  }
  return (bf_rect_t){left, start, right - left, end - start};
}

static inline bf_rect_t bf_damage_union(bf_rect_t a, bf_rect_t b) {
  int x = a.x < b.x ? a.x : b.x;
  int end = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  return (bf_rect_t){x, a.y, end - x, b.y + b.h - a.y};
}

static inline unsigned bf_damage_rects(const uint16_t* frame,
                                       const uint16_t* previous,
                                       bf_rect_t rects[BF_DAMAGE_ZONES]) {
  _Static_assert(BF_FRAME_W % BF_DAMAGE_BLOCK == 0, "whole RGB565 blocks");
  /* The order tickets and clock are above the belts, and the controls and
   * gauge are below. A single isolated HUD digit can widen a full-row range. */
  const int edges[] = {0, BF_FRAME_H / 4, BF_FRAME_H * 3 / 4, BF_FRAME_H};
  unsigned n = 0;
  for (unsigned i = 0; i < BF_DAMAGE_ZONES; ++i) {
    bf_rect_t r = bf_damage_band(frame, previous, edges[i], edges[i + 1]);
    if (!r.h) continue;
    if (n) {
      bf_rect_t last = rects[n - 1], both = bf_damage_union(last, r);
      /* Pay for a second address window only when it spares a sizeable DMA
       * and byte-swap. Ten full LCD rows correspond to 9,600 bytes. */
      if (both.w * both.h <= last.w * last.h + r.w * r.h + BF_FRAME_W * 10) {
        rects[n - 1] = both;
        continue;
      }
    }
    rects[n++] = r;
  }
  return n;
}

#endif
