#include "h2_garden_guard_sfx.h"

#include <stddef.h>

/* Waveforms. WOOD adds the octave, a marimba-like knock. BELL adds a
 * partial at 2.75 times the pitch, a dewy glass chime. NOISE is filtered
 * at the pitch frequency; RATTLE rings that noise against a tone. */
enum { SINE, WOOD, BELL, NOISE, RATTLE, TRI };

#define CURVE 0x40u /* Squared decay: percussive. Otherwise linear. */
#define LAST 0x80u
/* One six-byte step. Times are in 4 ms units, amplitude in units of 32;
 * end 0 holds the pitch. wait counts from the previous step of the cue. */
#define STEP(wait, voice, shape, flags, note, end, ms, amp) \
  (wait) / 4, (voice) | (shape) << 3 | (flags), note, end, (ms) / 4, (amp) / 32

static const uint8_t k_steps[] = {
    /* TAP: a short wooden tock with a dry tick on top. */
    STEP(0, 0, WOOD, CURVE, 86, 81, 40, 3200),
    STEP(0, 1, NOISE, CURVE | LAST, 100, 0, 12, 1600),
    /* DENIED: two soft hollow steps down, "nuh-uh". */
    STEP(0, 0, TRI, CURVE, 57, 55, 64, 3808),
    STEP(88, 0, TRI, CURVE | LAST, 53, 50, 96, 3808),
    /* SUN: a dew chime and its fifth; quick pickups climb a scale. */
    STEP(0, 0, BELL, CURVE, 86, 0, 148, 4000),
    STEP(36, 1, BELL, CURVE | LAST, 93, 0, 124, 2592),
    /* PLANT: a soft knock into soil, a puff of earth, a leaf rustle. */
    STEP(0, 0, WOOD, CURVE, 62, 48, 100, 4608),
    STEP(0, 1, NOISE, CURVE, 79, 67, 72, 2688),
    STEP(44, 1, NOISE, CURVE | LAST, 96, 91, 56, 1408),
    /* SHOT: a round little pop, falling a ninth. */
    STEP(0, 2, SINE, CURVE | LAST, 79, 64, 48, 1888),
    /* HIT: a light crack of the shell over a hollow knock. */
    STEP(0, 3, NOISE, CURVE, 98, 86, 36, 2784),
    STEP(0, 4, WOOD, CURVE | LAST, 64, 52, 52, 2208),
    /* CART: a tin clank, a rattling roll and a rising rush of air. */
    STEP(0, 4, BELL, CURVE, 76, 74, 40, 2208),
    STEP(0, 3, RATTLE, 0, 50, 57, 380, 2784),
    STEP(52, 4, NOISE, LAST, 72, 88, 320, 2208),
    /* WON: sol, do, la, then a bright bloom over C. */
    STEP(0, 0, WOOD, CURVE, 79, 0, 124, 3584),
    STEP(0, 3, TRI, CURVE, 55, 0, 124, 1600),
    STEP(112, 0, WOOD, CURVE, 84, 0, 124, 3584),
    STEP(112, 0, WOOD, CURVE, 81, 0, 112, 3584),
    STEP(108, 0, WOOD, 0, 88, 0, 460, 3584),
    STEP(0, 4, WOOD, 0, 84, 0, 460, 1792),
    STEP(0, 3, TRI, 0, 60, 0, 460, 1600),
    STEP(0, 1, BELL, CURVE | LAST, 96, 0, 460, 2208),
    /* LOST: a wilting minor walk down and a sigh. */
    STEP(0, 0, TRI, CURVE, 72, 0, 152, 3392),
    STEP(0, 3, SINE, CURVE, 60, 0, 152, 1600),
    STEP(160, 0, TRI, CURVE, 70, 0, 152, 3392),
    STEP(160, 0, TRI, CURVE, 67, 0, 152, 3392),
    STEP(160, 0, TRI, 0, 63, 58, 400, 3392),
    STEP(0, 3, SINE, 0, 51, 46, 400, 1600),
    STEP(0, 1, NOISE, LAST, 70, 58, 360, 1408),
};

/* First step, lane, rank and minimum restart gap (4 ms) of each cue. A cue
 * starts when its lane is idle, outranks the running cue, or matches its
 * rank after the gap. Within one block, each lane takes its first cue in
 * k_order; fanfares preempt everything. */
static const struct {
  uint8_t first, lane, rank, gap;
} k_cues[GG_SFX_COUNT] = {
    [GG_SFX_TAP] = {0, 0, 1, 32 / 4},    [GG_SFX_DENIED] = {2, 0, 2, 80 / 4},
    [GG_SFX_SUN] = {4, 0, 3, 40 / 4},    [GG_SFX_PLANT] = {6, 0, 3, 60 / 4},
    [GG_SFX_SHOT] = {9, 1, 1, 72 / 4},   [GG_SFX_HIT] = {10, 2, 1, 48 / 4},
    [GG_SFX_CART] = {12, 2, 2, 200 / 4}, [GG_SFX_WON] = {15, 0, 7, 0},
    [GG_SFX_LOST] = {23, 0, 7, 0},
};
static const uint8_t k_order[] = {GG_SFX_CART, GG_SFX_HIT,   GG_SFX_SHOT,
                                  GG_SFX_SUN,  GG_SFX_PLANT, GG_SFX_DENIED,
                                  GG_SFX_TAP};
static const uint8_t k_lane_voices[GG_SFX_LANES][2] = {{0, 2}, {2, 3}, {3, 5}};
static const int8_t k_sun_run[] = {0, 2, 4, 7};
static const int8_t k_shot_turn[] = {0, 2, -1};

/* Step at the sample rate for MIDI notes 36..100, in a 32-bit cycle. The
 * octave table is in hundredths of a hertz. */
static uint32_t pitch(uint32_t rate, int note) {
  static const uint16_t k_hz[] = {6541, 6930, 7342,  7778,  8241,  8731,
                                  9250, 9800, 10383, 11000, 11654, 12347};
  if (note < 36) note = 36;
  if (note > 100) note = 100;
  note -= 36;
  return ((((uint32_t)k_hz[note % 12] << (note / 12)) * (0x1000000u / rate)) /
          100u)
         << 8;
}

void gg_sfx_init(gg_sfx_t* sfx, uint32_t rate) {
  if (rate < 8000u) rate = 8000u;
  if (rate > 48000u) rate = 48000u;
  *sfx = (gg_sfx_t){.rate = rate,
                    .tick = rate / 250u,
                    .lift = 16384000u / rate, /* 2 ms attack */
                    .noise = 0x5eed1u};
  while ((rate >> sfx->fade) > 1000u) ++sfx->fade; /* about 1 ms */
}

void gg_sfx_clear(gg_sfx_t* sfx) {
  for (unsigned i = 0; i < GG_SFX_VOICES; ++i) sfx->voice[i].level = 0;
  for (unsigned i = 0; i < GG_SFX_LANES; ++i) sfx->lane[i].next = NULL;
  sfx->residual = 0;
}

static bool voices_active(const gg_sfx_t* sfx, unsigned from, unsigned to) {
  for (unsigned i = from; i < to; ++i)
    if (sfx->voice[i].level > 0) return true;
  return false;
}

bool gg_sfx_fanfare(const gg_sfx_t* sfx) {
  return sfx->lane[0].cue > GG_SFX_WON && gg_sfx_active(sfx);
}

bool gg_sfx_active(const gg_sfx_t* sfx) {
  if (sfx->residual) return true;
  for (unsigned i = 0; i < GG_SFX_LANES; ++i)
    if (sfx->lane[i].next) return true;
  return voices_active(sfx, 0, GG_SFX_VOICES);
}

static void begin(gg_sfx_t* sfx, const uint8_t* s, int transpose) {
  gg_sfx_voice_t* v = &sfx->voice[s[1] & 7u];
  /* A cut voice hands its last output to a fast fade instead of clicking. */
  if (v->level > 0) sfx->residual += v->last;
  const int32_t frames = (int32_t)(s[4] * sfx->tick);
  const uint32_t step = pitch(sfx->rate, s[2] + transpose);
  *v = (gg_sfx_voice_t){
      .step = step,
      .sweep =
          s[3] ? ((int32_t)pitch(sfx->rate, s[3] + transpose) - (int32_t)step) /
                     frames
               : 0,
      .level = 1 << 24,
      .fall = (1 << 24) / frames + 1,
      .amp = (uint16_t)(s[5] * 32u),
      .shape = (uint8_t)((s[1] >> 3) & 7u),
      .curve = (uint8_t)(s[1] & CURVE)};
}

/* Plays every step that is due, then schedules the next one. */
static void fire(gg_sfx_t* sfx, gg_sfx_lane_t* lane) {
  while (lane->next && lane->wait == 0u) {
    const uint8_t* s = lane->next;
    begin(sfx, s, lane->transpose);
    if (s[1] & LAST) {
      lane->next = NULL;
    } else {
      lane->next = s + 6;
      lane->wait = lane->next[0] * sfx->tick;
    }
  }
}

static void start(gg_sfx_t* sfx, unsigned cue, int transpose) {
  gg_sfx_lane_t* lane = &sfx->lane[k_cues[cue].lane];
  *lane = (gg_sfx_lane_t){.next = &k_steps[k_cues[cue].first * 6u],
                          .started = sfx->clock,
                          .transpose = (int8_t)transpose,
                          .cue = (uint8_t)(cue + 1u)};
  fire(sfx, lane);
}

static bool admits(const gg_sfx_t* sfx, unsigned cue) {
  const unsigned index = k_cues[cue].lane;
  const gg_sfx_lane_t* lane = &sfx->lane[index];
  if (!lane->next &&
      !voices_active(sfx, k_lane_voices[index][0], k_lane_voices[index][1]))
    return true;
  if (lane->cue == 0u) return true;
  const unsigned rank = k_cues[lane->cue - 1u].rank;
  if (k_cues[cue].rank != rank) return k_cues[cue].rank > rank;
  return sfx->clock - lane->started >= k_cues[cue].gap * sfx->tick;
}

#define BIT(cue) (1u << GG_SFX_##cue)

void gg_sfx_events(gg_sfx_t* sfx, uint16_t events) {
  const gg_sfx_lane_t* ui = &sfx->lane[0];
  const bool fanfare = gg_sfx_fanfare(sfx);
  if (events & (BIT(WON) | BIT(LOST))) {
    const unsigned cue = events & BIT(WON) ? GG_SFX_WON : GG_SFX_LOST;
    if (fanfare && ui->cue == cue + 1u) return; /* Already sounding. */
    gg_sfx_clear(sfx);
    start(sfx, cue, 0);
    return;
  }
  if (fanfare) return;
  unsigned lanes = 0;
  for (unsigned i = 0; i < sizeof(k_order); ++i) {
    const unsigned cue = k_order[i];
    if (!(events & (1u << cue)) || (lanes & (1u << k_cues[cue].lane))) continue;
    lanes |= 1u << k_cues[cue].lane;
    if (!admits(sfx, cue)) continue;
    int transpose = 0;
    if (cue == GG_SFX_SUN) {
      const bool streak = sfx->clock - sfx->sun_at < sfx->rate / 2u;
      if (!streak)
        sfx->sun_run = 0;
      else if (sfx->sun_run + 1u < sizeof(k_sun_run))
        ++sfx->sun_run;
      sfx->sun_at = sfx->clock;
      transpose = k_sun_run[sfx->sun_run];
    } else if (cue == GG_SFX_SHOT) {
      transpose = k_shot_turn[sfx->shot_turn];
      sfx->shot_turn = (uint8_t)((sfx->shot_turn + 1u) % sizeof(k_shot_turn));
    }
    start(sfx, cue, transpose);
  }
}

/* A parabola through the sine's zeros and peaks, Q14 from a 32-bit phase. */
static int32_t sine(uint32_t phase) {
  const int32_t x = (int32_t)((phase >> 16) ^ 0x8000u) - 32768;
  return x * (32768 - (x < 0 ? -x : x)) >> 14;
}

static int32_t voice(gg_sfx_voice_t* v, uint32_t lift, int32_t noise) {
  const uint32_t p = v->phase;
  int32_t w;
  switch (v->shape) {
    case SINE:
      w = sine(p);
      break;
    case WOOD:
      w = (3 * sine(p) + sine(p << 1)) >> 2;
      break;
    case BELL: {
      const uint32_t partial = (v->step << 1) + (v->step >> 1) + (v->step >> 2);
      /* Drop the partial above Nyquist, as at 8 kHz. */
      w = v->step < 0x2e8ba2e8u ? (sine(p) + sine(v->phase2)) >> 1 : sine(p);
      v->phase2 += partial;
      break;
    }
    case TRI: {
      const int32_t x = (int32_t)((p >> 16) ^ 0x8000u) - 32768;
      w = 16384 - (x < 0 ? -x : x);
      break;
    }
    default: { /* NOISE, RATTLE: one pole at the pitch, 2 pi f / rate. */
      int32_t a = (int32_t)(((v->step >> 13) * 201u) >> 9);
      if (a > 32767) a = 32767;
      /* Two poles: 12 dB per octave keeps the hiss soft under music. */
      v->low += ((noise - v->low) * a) >> 15;
      v->low2 += ((v->low - v->low2) * a) >> 15;
      /* Lift darker noise toward the level of brighter noise. */
      w = v->low2 * (a < 1024 ? 8 : a < 4096 ? 4 : a < 12288 ? 2 : 1);
      if (w > 16384) w = 16384;
      if (w < -16384) w = -16384;
      if (v->shape == RATTLE) w = sine(p) * w >> 14;
      break;
    }
  }
  v->phase = p + v->step;
  v->step += (uint32_t)v->sweep;
  int32_t gain = v->level >> 9; /* Q15 */
  if (v->curve) gain = gain * gain >> 15;
  if (v->rise < 32768) {
    gain = gain * v->rise >> 15;
    v->rise += (int32_t)lift;
  }
  v->level -= v->fall;
  v->last = ((w * gain) >> 15) * v->amp >> 14;
  return v->last;
}

int gg_sfx_next(gg_sfx_t* sfx) {
  ++sfx->clock;
  for (unsigned i = 0; i < GG_SFX_LANES; ++i) {
    gg_sfx_lane_t* lane = &sfx->lane[i];
    if (lane->next && --lane->wait == 0u) fire(sfx, lane);
  }
  sfx->noise = 1664525u * sfx->noise + 1013904223u;
  const int32_t noise = (int32_t)(sfx->noise >> 17) - 16384;
  int32_t mixed = sfx->residual;
  sfx->residual -= sfx->residual >> sfx->fade;
  if (sfx->residual > -(1 << sfx->fade) && sfx->residual < (1 << sfx->fade))
    sfx->residual = 0;
  for (unsigned i = 0; i < GG_SFX_VOICES; ++i)
    if (sfx->voice[i].level > 0)
      mixed += voice(&sfx->voice[i], sfx->lift, noise);
  return (int)mixed;
}
