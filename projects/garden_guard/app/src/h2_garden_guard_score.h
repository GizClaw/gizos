#ifndef H2_GARDEN_GUARD_SCORE_H
#define H2_GARDEN_GUARD_SCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h2_garden_guard_sfx.h"

typedef struct gg_note {
  uint32_t phase, step;
  int32_t sweep, envelope, decay;
  uint16_t age;
  uint8_t timbre;
} gg_note_t;

typedef struct gg_score {
  gg_note_t keys[3];
  gg_sfx_t sfx;
  uint32_t rate, tick_left;
  uint16_t tick;
} gg_score_t;

void gg_score_init(gg_score_t* score, uint32_t rate);
void gg_score_events(gg_score_t* score, uint16_t events);
void gg_score_clear_effects(gg_score_t* score);
bool gg_score_has_effect(const gg_score_t* score);
bool gg_score_has_fanfare(const gg_score_t* score);
unsigned gg_score_render(gg_score_t* score, bool music, int16_t* output,
                         unsigned frames, unsigned channels);

#endif
