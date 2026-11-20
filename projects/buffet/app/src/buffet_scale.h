#ifndef BUFFET_SCALE_H
#define BUFFET_SCALE_H
#include "h2_buffet.h"

/* The input and simulation use the same 800x480 stage on both displays. */
#ifdef BF_AC791N
enum { BF_SCALE_NUM = 3, BF_SCALE_DEN = 5, BF_LCD_PAD = 16 };
#else
enum { BF_SCALE_NUM = H2_BUFFET_DENSITY, BF_SCALE_DEN = 1, BF_LCD_PAD = 0 };
#endif
enum {
  BF_FRAME_W = H2_BUFFET_WIDTH * BF_SCALE_NUM / BF_SCALE_DEN,
  BF_FRAME_H = H2_BUFFET_HEIGHT * BF_SCALE_NUM / BF_SCALE_DEN
};
static inline int bf_pixel(int p) { return p * BF_SCALE_NUM / BF_SCALE_DEN; }
static inline int bf_logical(int p) {
  return (p * BF_SCALE_DEN + BF_SCALE_NUM / 2) / BF_SCALE_NUM;
}
#endif
