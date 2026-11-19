#ifndef BUFFET_SPRITE_H
#define BUFFET_SPRITE_H
#include <stdint.h>

typedef struct {
  int w, h;
  const uint8_t* pixels;
  const uint16_t* color;
  const uint8_t* alpha;
} bf_sprite_t;
extern const bf_sprite_t bf_room, bf_counter, bf_cloth, bf_mark, bf_cast,
    bf_otter, bf_rest;
extern const bf_sprite_t bf_salmon_cleared, bf_tamago_cleared, bf_tuna_cleared,
    bf_shrimp_cleared, bf_maki_cleared, bf_inari_cleared;
extern const bf_sprite_t bf_salmon, bf_tamago, bf_tuna, bf_shrimp, bf_maki,
    bf_inari, bf_empty;
extern const bf_sprite_t bf_salmon_belt, bf_tamago_belt, bf_tuna_belt,
    bf_shrimp_belt, bf_maki_belt, bf_inari_belt, bf_empty_belt;
#endif
