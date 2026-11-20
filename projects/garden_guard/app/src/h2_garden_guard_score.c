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

static int sample(gg_note_t* voice, int noise) {
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
    case 3:
      wave = (wave + noise) >> 1;
      break;
  }
  int gain = voice->envelope >> 16;
  if (voice->age < 64u) gain = (gain * voice->age++) >> 6;
  voice->phase += voice->step;
  voice->step += (uint32_t)voice->sweep;
  voice->envelope -= voice->decay;
  return (wave * gain) >> 7;
}

void gg_score_init(gg_score_t* score, uint32_t rate) {
  *score = (gg_score_t){.rate = rate, .noise = 0x2948a3u};
}

void gg_score_clear_effects(gg_score_t* score) {
  score->garden.envelope = score->combat.envelope = 0;
  score->fanfare = 0;
}

bool gg_score_has_effect(const gg_score_t* score) {
  return score->fanfare || score->garden.envelope > 0 ||
         score->combat.envelope > 0;
}

#define SOUND(x) (1u << GG_SOUND_##x)

void gg_score_events(gg_score_t* score, uint16_t events) {
  const uint32_t rate = score->rate;
  if (events & (SOUND(WON) | SOUND(LOST))) {
    gg_score_clear_effects(score);
    score->fanfare = events & SOUND(WON) ? 1u : 2u;
    score->fanfare_note = 0;
    score->effect_left = 0;
    return;
  }
  if (events & SOUND(SUN))
    begin(&score->garden, rate, 76, 145, 6500, 2, 91);
  else if (events & SOUND(PLANT))
    begin(&score->garden, rate, 48, 145, 7300, 3, 37);
  else if (events & SOUND(DENIED))
    begin(&score->garden, rate, 58, 95, 3750, 1, 49);
  else if ((events & SOUND(TAP)) && score->garden.envelope <= 0)
    begin(&score->garden, rate, 79, 58, 3400, 1, 68);
  /* Let a short combat transient finish. Bursts never stack. */
  if (score->combat.envelope > 0) return;
  if (events & SOUND(CART))
    begin(&score->combat, rate, 47, 385, 6900, 3, 39);
  else if (events & SOUND(HIT))
    begin(&score->combat, rate, 59, 76, 3700, 3, 42);
  else if (events & SOUND(SHOT))
    begin(&score->combat, rate, 68, 62, 2700, 1, 51);
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

static void next_fanfare(gg_score_t* score) {
  static const uint8_t notes[][4] = {{72, 76, 79, 84}, {69, 65, 62, 57}};
  const unsigned index = score->fanfare_note++;
  if (index == 4u) {
    score->fanfare = 0;
    return;
  }
  const unsigned ms = index == 3u ? 380u : 126u;
  const unsigned note = notes[score->fanfare - 1u][index];
  begin(&score->garden, score->rate, note, ms, 6900, 2, 0);
  begin(&score->combat, score->rate, note - 12u, ms, 3000, 0, 0);
  score->effect_left = score->rate * ms / 1000u;
}

unsigned gg_score_render(gg_score_t* score, bool music, int16_t* output,
                         unsigned frames, unsigned channels) {
  unsigned peak = 0;
  for (unsigned i = 0; i < frames; ++i) {
    int mixed = 0;
    if (music) {
      if (score->tick_left == 0u) next_tick(score);
      --score->tick_left;
      mixed = sample(&score->keys[0], 0) + sample(&score->keys[1], 0) +
              sample(&score->keys[2], 0);
    }
    if (score->fanfare) {
      if (score->effect_left == 0u) next_fanfare(score);
      if (score->effect_left) --score->effect_left;
    }
    if (score->garden.envelope > 0 || score->combat.envelope > 0) {
      score->noise = 1664525u * score->noise + 1013904223u;
      const int noise = (int)(score->noise >> 24u) - 128;
      mixed += sample(&score->garden, noise) + sample(&score->combat, noise);
    }
    /* The authored per-voice maxima sum to less than 22,000. */
    const unsigned magnitude = (unsigned)(mixed < 0 ? -mixed : mixed);
    if (magnitude > peak) peak = magnitude;
    *output++ = (int16_t)mixed;
    if (channels == 2u) *output++ = (int16_t)mixed;
  }
  return peak;
}
