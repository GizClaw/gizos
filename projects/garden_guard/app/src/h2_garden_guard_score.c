#include "h2_garden_guard_score.h"

#include "h2_garden_guard_audio.h"

/* Morning Rounds: an original 32-second score in sixteenths, at 120 BPM.
 * Each bar gives a bass root, the third of its chord, and a picked melody.
 * Zero is a rest. The phrase stays in C major with a G turnaround. */
static const uint8_t k_bass[] = {48, 45, 41, 43, 48, 45, 41, 43,
                                 52, 45, 41, 43, 48, 46, 41, 43};
static const uint8_t k_third[] = {4, 3, 4, 4, 4, 3, 4, 4,
                                  3, 3, 4, 4, 4, 4, 4, 4};
static const uint8_t k_melody[][8] = {
    {76, 0, 79, 81, 79, 76, 74, 0}, {72, 0, 76, 79, 76, 72, 69, 0},
    {72, 0, 77, 81, 84, 0, 81, 77}, {79, 0, 74, 71, 74, 0, 79, 0},
    {76, 0, 79, 84, 81, 79, 76, 0}, {72, 0, 76, 81, 79, 76, 72, 0},
    {77, 0, 81, 84, 81, 77, 74, 0}, {74, 0, 79, 0, 76, 74, 71, 0},
    {67, 0, 76, 79, 83, 0, 79, 0},  {81, 0, 76, 72, 76, 0, 81, 0},
    {84, 0, 81, 77, 81, 0, 77, 74}, {79, 0, 74, 71, 74, 0, 79, 0},
    {84, 0, 79, 76, 79, 0, 76, 0},  {82, 0, 77, 74, 77, 0, 74, 0},
    {81, 0, 77, 72, 74, 77, 81, 0}, {79, 0, 74, 0, 71, 74, 72, 0},
};

static const int8_t k_sine[] = {
    0,    12,   25,   37,   49,   60,   71,   81,   90,   98,   106,
    112,  117,  122,  125,  126,  127,  126,  125,  122,  117,  112,
    106,  98,   90,   81,   71,   60,   49,   37,   25,   12,   0,
    -12,  -25,  -37,  -49,  -60,  -71,  -81,  -90,  -98,  -106, -112,
    -117, -122, -125, -126, -127, -126, -125, -122, -117, -112, -106,
    -98,  -90,  -81,  -71,  -60,  -49,  -37,  -25,  -12,
};

/* Increment at the supplied sample rate, using notes 36..96 and a 24-bit
 * cycle. The cents-of-Hz table and the short interpolated sine table keep
 * the per-sample path free of floating point and division. */
static uint32_t phase_step(unsigned rate, unsigned note) {
  static const uint16_t frequency[] = {6541, 6930, 7342,  7778,  8241,  8731,
                                       9250, 9800, 10383, 11000, 11654, 12347};
  note -= 36u;
  return (((uint32_t)frequency[note % 12u] << (note / 12u)) *
          (0x1000000u / rate)) /
         100u;
}

static void begin(gg_note_t* voice, uint32_t rate, unsigned note, unsigned ms,
                  unsigned amplitude, uint8_t timbre, unsigned final_note) {
  const uint32_t samples = rate * ms / 1000u;
  const uint32_t step = phase_step(rate, note);
  const int32_t envelope = (int32_t)(amplitude << 16u);
  *voice = (gg_note_t){
      .step = step,
      .sweep = final_note
                   ? ((int32_t)phase_step(rate, final_note) - (int32_t)step) /
                         (int32_t)samples
                   : 0,
      .envelope = envelope,
      .decay = envelope / (int32_t)samples,
      .timbre = timbre};
}

static int sine(uint32_t phase) {
  const unsigned index = (phase >> 18u) & 63u;
  const int a = k_sine[index], b = k_sine[(index + 1u) & 63u];
  return a + ((b - a) * (int)((phase >> 10u) & 255u) >> 8);
}

static int sample(gg_note_t* voice) {
  if (voice->envelope <= 0) return 0;
  const uint32_t p = voice->phase;
  int wave = sine(p);
  switch (voice->timbre) {
    case 1:
      wave = (wave * 3 + sine(p * 2u)) >> 2;
      break;
    case 2:
      wave = (wave * 3 + sine(p * 3u)) >> 2;
      break;
  }
  int gain = voice->envelope >> 16;
  if (voice->age < 64u) gain = (gain * voice->age++) >> 6;
  voice->phase += voice->step;
  voice->step += (uint32_t)voice->sweep;
  voice->envelope -= voice->decay;
  return (wave * gain) >> 7;
}

#define SAME_CUE(name) ((int)GG_SFX_##name == (int)GG_SOUND_##name)
_Static_assert(SAME_CUE(TAP) && SAME_CUE(DENIED) && SAME_CUE(SUN) &&
                   SAME_CUE(PLANT) && SAME_CUE(SHOT) && SAME_CUE(HIT) &&
                   SAME_CUE(CART) && SAME_CUE(WON) && SAME_CUE(LOST) &&
                   SAME_CUE(COUNT),
               "sound cue order");
#undef SAME_CUE

void gg_score_init(gg_score_t* score, uint32_t rate) {
  *score = (gg_score_t){.rate = rate};
  gg_sfx_init(&score->sfx, rate);
}

void gg_score_clear_effects(gg_score_t* score) { gg_sfx_clear(&score->sfx); }

bool gg_score_has_effect(const gg_score_t* score) {
  return gg_sfx_active(&score->sfx);
}

bool gg_score_has_fanfare(const gg_score_t* score) {
  return gg_sfx_fanfare(&score->sfx);
}

/* Cues and their priorities live in h2_garden_guard_sfx.c. */
void gg_score_events(gg_score_t* score, uint16_t events) {
  gg_sfx_events(&score->sfx, events);
}

static void next_tick(gg_score_t* score) {
  const unsigned bar = score->tick >> 4u, beat = score->tick & 15u;
  const unsigned root = k_bass[bar];
  const uint32_t rate = score->rate;
  if (beat == 0u || beat == 7u || beat == 8u)
    begin(&score->keys[0], rate, root + (beat == 8u ? 7u : 0u),
          beat == 7u ? 100 : 360, beat == 7u ? 1500 : 2550, 0, 0);
  if (beat == 2u || beat == 6u || beat == 10u || beat == 14u)
    begin(&score->keys[1], rate, root + 12u + ((beat & 4u) ? 7u : k_third[bar]),
          175, 1400, 1, 0);
  if ((beat & 1u) == 0u && k_melody[bar][beat >> 1u])
    begin(&score->keys[2], rate, k_melody[bar][beat >> 1u], 242, 3000, 2, 0);
  score->tick = (score->tick + 1u) & 255u;
  score->tick_left = rate / 8u;
}

unsigned gg_score_render(gg_score_t* score, bool music, int16_t* output,
                         unsigned frames, unsigned channels) {
  unsigned peak = 0;
  for (unsigned i = 0; i < frames; ++i) {
    int mixed = 0;
    if (music) {
      if (score->tick_left == 0u) next_tick(score);
      --score->tick_left;
      mixed = sample(&score->keys[0]) + sample(&score->keys[1]) +
              sample(&score->keys[2]);
    }
    /* Music <= 6950 plus GG_SFX_PEAK_UI + GG_SFX_PEAK_COMBAT; fanfares
     * (<= GG_SFX_PEAK_FANFARE) sound without music. Saturate regardless. */
    mixed += gg_sfx_next(&score->sfx);
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32767) mixed = -32767;
    const unsigned magnitude = (unsigned)(mixed < 0 ? -mixed : mixed);
    if (magnitude > peak) peak = magnitude;
    *output++ = (int16_t)mixed;
    if (channels == 2u) *output++ = (int16_t)mixed;
  }
  return peak;
}
