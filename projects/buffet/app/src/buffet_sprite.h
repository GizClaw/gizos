#ifndef BUFFET_SPRITE_H
#define BUFFET_SPRITE_H
#include <stdint.h>

typedef struct {
  int w, h;
  const uint8_t* pixels;
  const uint16_t* color;
  const uint8_t* alpha;
} bf_sprite_t;
struct bf_paint {
  int w, h;
  const uint16_t* rgb;
  const uint8_t* alpha;
};
typedef struct bf_paint bf_paint_t;
extern const uint16_t bf_floor_pixels[1600 * 960];
extern const uint16_t bf_belt_runs[];
extern const unsigned bf_belt_run_count;
extern const bf_paint_t bf_counter, bf_counter_shade, bf_mat, bf_booth, bf_yuzu,
    bf_condiments, bf_tea, bf_steel, bf_plate_shade;
extern const bf_sprite_t bf_cloth, bf_mark, bf_cast, bf_guest, bf_rest,
    bf_gauge, bf_gauge_full, bf_card_back, bf_card_bite, bf_card_close,
    bf_card_rest;
extern const bf_paint_t bf_salmon_cleared, bf_tamago_cleared, bf_tuna_cleared,
    bf_shrimp_cleared, bf_maki_cleared, bf_inari_cleared;
extern const bf_paint_t bf_salmon, bf_tamago, bf_tuna, bf_shrimp, bf_maki,
    bf_inari, bf_empty;
extern const bf_paint_t bf_salmon_bite, bf_tamago_bite, bf_tuna_bite,
    bf_shrimp_bite, bf_maki_bite, bf_inari_bite;
extern const bf_paint_t bf_salmon_belt, bf_tamago_belt, bf_tuna_belt,
    bf_shrimp_belt, bf_maki_belt, bf_inari_belt;
#endif
