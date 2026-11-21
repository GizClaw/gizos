#include "h2_garden_guard_audio.h"
#include "h2_garden_guard_score.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                      \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      exit(1);                                        \
    }                                                 \
  } while (0)

static void music_is_seamless(unsigned rate) {
  gg_score_t score;
  gg_score_init(&score, rate);
  int16_t initial[256], pcm[256];
  CHECK(gg_score_render(&score, true, initial, 256, 1) > 1000u);
  unsigned left = 256u * (rate / 8u) - 256u;
  while (left > 0u) {
    const unsigned count = left < 256u ? left : 256u;
    CHECK(gg_score_render(&score, true, pcm, count, 1) <= 7000u);
    left -= count;
  }
  CHECK(pcm[255] == 0);
  CHECK(gg_score_render(&score, false, pcm, 256, 1) == 0u);
  CHECK(gg_score_render(&score, true, pcm, 256, 1) > 1000u);
  CHECK(memcmp(pcm, initial, sizeof(initial)) == 0);
}

static void bounded_and_distinct_effects(unsigned rate) {
  int16_t pcm[256];
  uint32_t fingerprint[GG_SOUND_COUNT];
  for (unsigned event = 0; event < GG_SOUND_COUNT; ++event) {
    gg_score_t score;
    gg_score_init(&score, rate);
    gg_score_events(&score, (uint16_t)(1u << event));
    unsigned frames = 0, peak = 0;
    uint32_t hash = 2166136261u;
    while (gg_score_has_effect(&score) && frames < rate) {
      const unsigned level = gg_score_render(&score, false, pcm, 128, 2);
      if (level > peak) peak = level;
      for (unsigned i = 0; i < 256; i += 2) {
        CHECK(pcm[i] == pcm[i + 1u]);
        hash = (hash ^ (uint16_t)pcm[i]) * 16777619u;
      }
      frames += 128u;
    }
    CHECK(frames < rate && peak > 1400u && peak <= 11000u);
    CHECK(gg_score_render(&score, false, pcm, 128, 2) == 0u);
    for (unsigned prior = 0; prior < event; ++prior)
      CHECK(hash != fingerprint[prior]);
    fingerprint[event] = hash;
  }
}

static void dense_events_are_bounded(void) {
  gg_score_t score;
  gg_score_init(&score, 16000u);
  int16_t pcm[320];
  unsigned peak = 0;
  for (unsigned i = 0; i < 500; ++i) {
    const uint16_t events = (uint16_t)(1u << (i % GG_SOUND_WON)) |
                            (1u << GG_SOUND_SHOT) | (1u << GG_SOUND_HIT);
    gg_score_events(&score, events);
    const unsigned level = gg_score_render(&score, true, pcm, 320, 1);
    if (level > peak) peak = level;
  }
  CHECK(peak > 3600u && peak < 24000u);
  gg_score_clear_effects(&score);
  CHECK(!gg_score_has_effect(&score));
  CHECK(gg_score_render(&score, false, pcm, 320, 1) == 0u);
}

int main(void) {
  music_is_seamless(16000u);
  music_is_seamless(44100u);
  bounded_and_distinct_effects(8000u);
  bounded_and_distinct_effects(16000u);
  bounded_and_distinct_effects(48000u);
  dense_events_are_bounded();
  return 0;
}
