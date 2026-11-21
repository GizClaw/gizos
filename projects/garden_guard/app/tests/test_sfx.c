/* Garden Guard SFX: every cue starts within 1 ms, stays inside its authored
 * bound, ends within its length, and dense retriggers stay bounded. */
#include "h2_garden_guard_sfx.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #condition);                                             \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

static const unsigned k_rates[] = {8000u, 16000u, 22050u, 44100u, 48000u};
/* Longest audible length of each cue, in ms, including the 1 ms fade. */
static const unsigned k_length_ms[GG_SFX_COUNT] = {60, 200, 180, 120, 60,
                                                   70, 400, 820, 900};

static int magnitude(int v) { return v < 0 ? -v : v; }

static void test_cues(unsigned rate) {
  for (unsigned cue = 0; cue < GG_SFX_COUNT; ++cue) {
    gg_sfx_t sfx;
    gg_sfx_init(&sfx, rate);
    CHECK(!gg_sfx_active(&sfx));
    CHECK(gg_sfx_next(&sfx) == 0);
    gg_sfx_events(&sfx, (uint16_t)(1u << cue));
    CHECK(gg_sfx_active(&sfx));
    const int bound = cue >= GG_SFX_WON     ? GG_SFX_PEAK_FANFARE
                      : cue <= GG_SFX_PLANT ? GG_SFX_PEAK_UI
                                            : GG_SFX_PEAK_COMBAT;
    unsigned frames = 0, first = 0;
    int peak = 0;
    while (gg_sfx_active(&sfx)) {
      const int v = gg_sfx_next(&sfx);
      if (!first && magnitude(v) > 64) first = frames + 1u;
      if (magnitude(v) > peak) peak = magnitude(v);
      CHECK(++frames <= rate * k_length_ms[cue] / 1000u);
    }
    CHECK(first > 0u && first <= rate / 1000u);
    CHECK(peak > 1000 && peak <= bound);
    for (unsigned i = 0; i < rate / 100u; ++i) CHECK(gg_sfx_next(&sfx) == 0);
  }
}

/* Every cue on every 16 ms frame for 3 s: bounded output, and the lanes
 * keep playing instead of restarting on each frame. */
static void test_flood(unsigned rate) {
  gg_sfx_t sfx;
  gg_sfx_init(&sfx, rate);
  int peak = 0;
  unsigned starts = 0;
  uint32_t seen[GG_SFX_LANES] = {0};
  for (unsigned frame = 0; frame < 3000u / 16u; ++frame) {
    gg_sfx_events(&sfx, (uint16_t)((1u << GG_SFX_WON) - 1u));
    for (unsigned l = 0; l < GG_SFX_LANES; ++l)
      if (sfx.lane[l].started != seen[l] || frame == 0u) {
        seen[l] = sfx.lane[l].started;
        ++starts;
      }
    for (unsigned i = 0; i < rate * 16u / 1000u; ++i) {
      const int v = magnitude(gg_sfx_next(&sfx));
      if (v > peak) peak = v;
    }
  }
  CHECK(peak <= GG_SFX_PEAK_UI + GG_SFX_PEAK_COMBAT);
  CHECK(starts < 3u * 3000u / 16u / 2u);
}

/* A fanfare stops the other lanes, ignores later cues, and does not
 * restart while the same result is still published. */
static void test_fanfare(unsigned rate) {
  gg_sfx_t sfx;
  gg_sfx_init(&sfx, rate);
  gg_sfx_events(&sfx, 1u << GG_SFX_CART);
  for (unsigned i = 0; i < rate / 20u; ++i) (void)gg_sfx_next(&sfx);
  gg_sfx_events(&sfx, 1u << GG_SFX_WON);
  const uint32_t started = sfx.lane[0].started;
  CHECK(sfx.lane[2].next == NULL);
  for (unsigned i = 0; i < rate / 10u; ++i) (void)gg_sfx_next(&sfx);
  gg_sfx_events(&sfx, (1u << GG_SFX_WON) | (1u << GG_SFX_SHOT));
  CHECK(sfx.lane[0].started == started && sfx.lane[1].next == NULL);
  gg_sfx_events(&sfx, 1u << GG_SFX_LOST);
  CHECK(sfx.lane[0].cue == GG_SFX_LOST + 1u);
  gg_sfx_clear(&sfx);
  CHECK(!gg_sfx_active(&sfx));
}

int main(void) {
  for (unsigned i = 0; i < sizeof(k_rates) / sizeof(k_rates[0]); ++i) {
    test_cues(k_rates[i]);
    test_flood(k_rates[i]);
    test_fanfare(k_rates[i]);
  }
  puts("garden guard sfx ok");
  return 0;
}
