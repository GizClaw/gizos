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
extern const bf_paint_t bf_wave, bf_leaf, bf_sun, bf_table_shadow,
    bf_plate_shade;
extern const bf_paint_t bf_hud_next, bf_hud_fullness, bf_hud_clock_bg,
    bf_hud_clock, bf_hud_plates, bf_hud_arrival, bf_hud_berth, bf_hud_selected,
    bf_hud_wave, bf_hud_leaf, bf_hud_sun;
extern const bf_sprite_t bf_cloth, bf_mark, bf_cast, bf_guest, bf_rest,
    bf_gauge, bf_gauge_full, bf_card_back, bf_card_bite, bf_card_close,
    bf_card_rest;
extern const bf_paint_t bf_salmon, bf_tamago, bf_tuna, bf_shrimp, bf_maki,
    bf_inari, bf_empty;
extern const bf_paint_t bf_salmon_bite, bf_tamago_bite, bf_tuna_bite,
    bf_shrimp_bite, bf_maki_bite, bf_inari_bite;
extern const bf_paint_t bf_salmon_table, bf_tamago_table, bf_tuna_table,
    bf_shrimp_table, bf_maki_table, bf_inari_table;
#endif
