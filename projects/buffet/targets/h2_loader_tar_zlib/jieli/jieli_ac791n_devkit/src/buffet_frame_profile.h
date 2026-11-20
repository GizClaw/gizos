#ifndef TIDEWHEEL_AC791N_FRAME_PROFILE_H
#define TIDEWHEEL_AC791N_FRAME_PROFILE_H

#include "h2_buffet.h"
#include "h2_jieli_ac791n_devkit.h"

/* Single game-task writer, including synchronous BSP calls. Sample one live
 * frame in eight. Intervals use the same monotonic TIMER5 behind the App clock.
 * Each accepted sample spans two consecutive live frame entries. */
enum profile_part {
  PF_GAME,
  PF_TOUCH,
  PF_AUDIO,
  PF_MIX,
  PF_WRITE,
  PF_COPY,
  PF_SCENE,
  PF_DIFF,
  PF_LCD_OTHER,
  PF_LCD_DRAIN,
  PF_LCD_WINDOW,
  PF_LCD_PACK,
  PF_LCD_SUBMIT,
  PF_WAIT,
  PF_REST,
  PF_OTHER,
  PF_PARTS
};
typedef struct {
  unsigned sequence, samples, failed;
  bool active;
  enum profile_part part;
  uint64_t last_us;
  uint32_t current[PF_PARTS], total[PF_PARTS];
} buffet_frame_profile_t;

static inline void profile_change(buffet_frame_profile_t* p,
                                  const h2_pal_time_api_t* clock,
                                  enum profile_part next) {
  if (!p->active || p->part == next) return;
  uint64_t now = 0;
  if (h2_pal_time_get_monotonic_us(clock, &now) != H2_PAL_OK ||
      now < p->last_us || now - p->last_us > UINT32_MAX) {
    p->active = false;
    ++p->failed;
    return;
  }
  p->current[p->part] += (uint32_t)(now - p->last_us);
  p->part = next;
  p->last_us = now;
}

static inline void profile_frame(buffet_frame_profile_t* p,
                                 const h2_pal_time_api_t* clock, bool live) {
  if (p->active && live) {
    /* A unique sentinel ends the last interval, even if its part was OTHER. */
    profile_change(p, clock, PF_PARTS);
    if (p->active) {
      for (unsigned i = 0; i < PF_PARTS; ++i) p->total[i] += p->current[i];
      ++p->samples;
    }
  }
  p->active = false;
  if (live && (++p->sequence % 8u == 0u)) {
    if (h2_pal_time_get_monotonic_us(clock, &p->last_us) != H2_PAL_OK) {
      ++p->failed;
      return;
    }
    for (unsigned i = 0; i < PF_PARTS; ++i) p->current[i] = 0;
    p->active = true;
    p->part = PF_GAME;
  }
}

static inline void profile_stage(buffet_frame_profile_t* p,
                                 const h2_pal_time_api_t* clock,
                                 h2_buffet_stage_t stage) {
  if (stage == H2_BUFFET_STAGE_FRAME || stage == H2_BUFFET_STAGE_DINING_FRAME) {
    profile_frame(p, clock, stage == H2_BUFFET_STAGE_DINING_FRAME);
    return;
  }
  if (!p->active) return;
  enum profile_part next;
  switch (stage) {
    case H2_BUFFET_STAGE_STEP:
      next = PF_GAME;
      break;
    case H2_BUFFET_STAGE_TOUCH:
      next = PF_TOUCH;
      break;
    case H2_BUFFET_STAGE_TRACK_OPEN:
    case H2_BUFFET_STAGE_TRACK_STOP:
    case H2_BUFFET_STAGE_AUDIO:
      next = PF_AUDIO;
      break;
    case H2_BUFFET_STAGE_MIX:
      next = PF_MIX;
      break;
    case H2_BUFFET_STAGE_TRACK_WRITE:
      next = PF_WRITE;
      break;
    case H2_BUFFET_STAGE_COPY:
      next = PF_COPY;
      break;
    case H2_BUFFET_STAGE_GROUND:
    case H2_BUFFET_STAGE_SCENE:
      next = PF_SCENE;
      break;
    case H2_BUFFET_STAGE_COMPARE:
      next = PF_DIFF;
      break;
    case H2_BUFFET_STAGE_BITMAP:
    case H2_BUFFET_STAGE_PRESENT:
      next = PF_LCD_OTHER;
      break;
    case H2_BUFFET_STAGE_REST:
      next = PF_REST;
      break;
    case H2_BUFFET_STAGE_LATE:
    case H2_BUFFET_STAGE_WAIT:
    case H2_BUFFET_STAGE_REST_DONE:
      next = PF_WAIT;
      break;
    default:
      next = PF_OTHER;
      break;
  }
  profile_change(p, clock, next);
}

#endif
