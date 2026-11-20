#include "buffet_audio.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int count_edge(void* user, h2_pal_log_level_t level, const char* scope,
                      const char* message) {
  (void)level;
  (void)scope;
  int* counts = user;
  if (strcmp(message, "red") == 0) ++counts[0];
  if (strcmp(message, "recovery") == 0) ++counts[1];
  if (strcmp(message, "plate") == 0) ++counts[2];
  if (strcmp(message, "serving") == 0) ++counts[3];
  return H2_PAL_OK;
}
static void alarm_edges(void) {
  int counts[4] = {0};
  const h2_pal_log_vtable_t vt = {.write = count_edge};
  const h2_pal_log_api_t log = {counts, &vt};
  h2_runtime_t rt = {.log = &log};
  bf_audio_t audio = {.rt = &rt,
                      .available = true,
                      .mix = {.rate = 16000},
                      .retry_after = UINT64_MAX};
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  g.fullness_milli = 120010;
  g.red_ms = 6000;
  g.player.pos = bf_place(&g, 4).station;
  bf_sound_state_t before = bf_sound_state(&g);
  bf_step(&g, 33, 0, 0);
  assert(g.fullness_milli == 119944 && g.red_ms == 0);
  bf_action(&g, H2_BUFFET_CONFIRM);
  bf_audio_update(&audio, before, &g, 33, false);
  assert(g.fullness_milli == 142944 && g.red_ms == 0 && counts[0] == 1 &&
         counts[1] == 0 && counts[2] == 1);
  before = bf_sound_state(&g);
  bf_step(&g, 33, 0, 0);
  bf_audio_update(&audio, before, &g, 66, false);
  assert(counts[0] == 1 && counts[1] == 0);
  g.fullness_milli = 120010;
  before = bf_sound_state(&g);
  bf_step(&g, 33, 0, 0);
  bf_audio_update(&audio, before, &g, 99, false);
  assert(counts[0] == 1 && counts[1] == 1);
  before = bf_sound_state(&g);
  bf_step(&g, 33, 0, 0);
  bf_audio_update(&audio, before, &g, 132, false);
  assert(counts[0] == 1 && counts[1] == 1);
  before = bf_sound_state(&g);
  bf_step(&g, 3000, 0, 0);
  bf_audio_update(&audio, before, &g, 3132, false);
  assert(counts[3] == 1);
}
int main(void) {
  alarm_edges();
  assert(bf_tracks[BF_MUSIC].count == 409600);
  bf_mix_t m = {.rate = 16000, .music_on = true, .envelope = 1};
  const uint32_t n = bf_tracks[BF_MUSIC].count;
  m.song_pos = (uint64_t)(n - 2) << 16;
  int16_t stereo[8] = {0};
  bf_mix_frame(&m, stereo, 4, 2);
  assert(m.song_pos == (2u << 16));
  for (int i = 0; i < 4; i++) {
    int index = (n - 2 + i) % n;
    int wanted = (int)(bf_tracks[BF_MUSIC].samples[index] * .5f) * 85 / 100;
    assert(stereo[i * 2] == wanted && stereo[i * 2 + 1] == wanted);
  }
  m = (bf_mix_t){.rate = 48000};
  bf_mix_emit(&m, BF_BITE, 185);
  int16_t frame[960];
  for (int i = 0; i < 9; i++) {
    bf_mix_frame(&m, frame, 960, 1);
    for (int j = 0; j < 960; j++) assert(!frame[j]);
  }
  bf_mix_frame(&m, frame, 960, 1);
  int peak = 0;
  for (int j = 0; j < 960; j++) {
    if (j < 240) assert(!frame[j]);
    int v = abs(frame[j]);
    if (v > peak) peak = v;
  }
  assert(peak > 60);
  memset(m.voices, 0, sizeof(m.voices));
  bf_mix_emit(&m, BF_ADMIT, 0);
  bf_mix_emit(&m, BF_PLATE, 0);
  bf_mix_emit(&m, BF_BITE, 0);
  bf_mix_emit(&m, BF_RED, 0);
  bf_mix_emit(&m, BF_RED, 0);
  int alarms = 0;
  for (int j = 0; j < 3; j++) alarms += m.voices[j].cue == BF_RED;
  assert(alarms == 1);
  m.music_on = true;
  for (int i = 0; i < 12; i++) bf_mix_frame(&m, frame, 960, 1);
  assert(m.envelope > .99f);
  m.music_on = false;
  for (int i = 0; i < 40; i++) bf_mix_frame(&m, frame, 960, 1);
  assert(m.envelope == 0);
  for (int i = 0; i < 960; i++) assert(frame[i] == 0);
  bf_game_t g;
  bf_init(&g, true);
  bf_tap(&g, 552, 27);
  assert(g.muted);
  bf_action(&g, H2_BUFFET_CONFIRM);
  assert(g.muted && g.phase == BF_DINING);
  bf_rect_t toggle = bf_sound_bounds(&g);
  bf_tap(&g, toggle.x + 10, toggle.y + 10);
  assert(!g.muted && !g.eaten);
  return 0;
}
