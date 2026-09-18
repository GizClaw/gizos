#include "h2_gizclaw_time_stretch_internal.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_SAMPLES 64000u /* 4 s at 16 kHz */
#define PERIOD 40u            /* 400 Hz */
#define AMPLITUDE 8000

static int16_t s_source[SOURCE_SAMPLES];
static int16_t s_output[SOURCE_SAMPLES * 3u];
static h2_gizclaw_stretch_t s_stretch;

/* A 400 Hz triangle: smooth enough for a crossfade, and its zero crossings
 * give the pitch without floating point. */
static void build_source(void) {
  for (size_t i = 0; i < SOURCE_SAMPLES; ++i) {
    const int32_t phase = (int32_t)(i % PERIOD);
    const int32_t half = (int32_t)PERIOD / 2;
    const int32_t ramp = phase < half ? phase : (int32_t)PERIOD - phase;
    s_source[i] = (int16_t)(ramp * 4 * AMPLITUDE / (int32_t)PERIOD - AMPLITUDE);
  }
}

typedef struct run {
  size_t output;
  uint64_t source;
} run_t;

static void append(run_t *run, const int16_t *out, size_t count,
                   uint64_t source) {
  assert(run->output + count <= sizeof(s_output) / sizeof(s_output[0]));
  memcpy(s_output + run->output, out, count * sizeof(*out));
  run->output += count;
  run->source += source;
}

/* Feeds [first, last) of the source in decoder-sized chunks at rate. */
static void stretch_range(run_t *run, size_t first, size_t last,
                          uint32_t rate) {
  for (size_t done = first; done < last;) {
    size_t chunk = last - done < 1920u ? last - done : 1920u;
    done += h2_gizclaw_stretch_push(&s_stretch, s_source + done, chunk);
    const int16_t *out = NULL;
    size_t count = 0;
    uint64_t source = 0;
    while (h2_gizclaw_stretch_step(&s_stretch, rate, &out, &count, &source)) {
      assert(count == H2_GIZCLAW_STRETCH_STEP);
      append(run, out, count, source);
    }
  }
}

static void flush(run_t *run) {
  const int16_t *out = NULL;
  size_t count = 0;
  uint64_t source = 0;
  h2_gizclaw_stretch_flush(&s_stretch, &out, &count, &source);
  append(run, out, count, source);
}

static run_t stretch_all(uint32_t rate) {
  run_t run = {0};
  h2_gizclaw_stretch_reset(&s_stretch);
  stretch_range(&run, 0, SOURCE_SAMPLES, rate);
  flush(&run);
  return run;
}

/* Rising zero crossings per 1000 samples, over [first, last). */
static unsigned crossings_per_1000(size_t first, size_t last) {
  unsigned crossings = 0;
  for (size_t i = first + 1u; i < last; ++i)
    crossings += s_output[i - 1u] < 0 && s_output[i] >= 0;
  return (unsigned)((uint64_t)crossings * 1000u / (last - first));
}

static int32_t largest_step(size_t count) {
  int32_t largest = 0;
  for (size_t i = 1; i < count; ++i) {
    int32_t diff = (int32_t)s_output[i] - (int32_t)s_output[i - 1u];
    if (diff < 0)
      diff = -diff;
    if (diff > largest)
      largest = diff;
  }
  return largest;
}

/* The source's own steepest step is 4 * AMPLITUDE / PERIOD. */
static const int32_t SLOPE = 4 * AMPLITUDE / (int32_t)PERIOD;

static void test_recorded_speed_is_identity(void) {
  /* At 1000 the best match is always offset 0 and the crossfade is between
   * identical samples: the output is the source, and the flush continues it
   * exactly. */
  const run_t run = stretch_all(1000u);
  assert(run.output == SOURCE_SAMPLES);
  assert(run.source == SOURCE_SAMPLES);
  assert(!memcmp(s_output, s_source, sizeof(s_source)));
}

static void test_rate_scales_length_and_keeps_pitch(void) {
  const uint32_t rates[] = {500u, 800u, 900u, 1250u, 2000u};
  const unsigned source_pitch = 25u; /* 1000 / PERIOD */
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
    const run_t run = stretch_all(rates[i]);
    /* Every source sample is accounted for exactly once. */
    assert(run.source == SOURCE_SAMPLES);
    const uint64_t expected = (uint64_t)SOURCE_SAMPLES * 1000u / rates[i];
    const uint64_t slack = 2u * H2_GIZCLAW_STRETCH_SEQUENCE;
    assert(run.output + slack >= expected && run.output <= expected + slack);
    /* Same pitch: the crossing rate of the output matches the source's. */
    const unsigned pitch = crossings_per_1000(1000u, run.output - 1000u);
    assert(pitch + 1u >= source_pitch && pitch <= source_pitch + 1u);
    /* Crossfades between aligned periods add no clicks. */
    assert(largest_step(run.output) <= 2 * SLOPE);
  }
}

static void test_switching_rates_is_seamless(void) {
  /* Recorded speed -> 0.8x -> recorded speed, the way the player switches:
   * the first step starts exactly at the next source sample and the flush
   * continues exactly after the carried overlap. */
  run_t run = {0};
  const size_t a = 8000u, b = 40000u;
  append(&run, s_source, a, a);
  h2_gizclaw_stretch_reset(&s_stretch);
  stretch_range(&run, a, b, 800u);
  assert(!memcmp(s_output + a, s_source + a,
                 H2_GIZCLAW_STRETCH_STEP * sizeof(int16_t)));
  flush(&run);
  append(&run, s_source + b, SOURCE_SAMPLES - b, SOURCE_SAMPLES - b);
  assert(run.source == SOURCE_SAMPLES);
  assert(run.output > SOURCE_SAMPLES);
  assert(largest_step(run.output) <= 2 * SLOPE);
  /* The rate may also change between steps without a flush. */
  run = (run_t){0};
  h2_gizclaw_stretch_reset(&s_stretch);
  stretch_range(&run, 0, 20000u, 800u);
  stretch_range(&run, 20000u, 40000u, 1500u);
  stretch_range(&run, 40000u, SOURCE_SAMPLES, 600u);
  flush(&run);
  assert(run.source == SOURCE_SAMPLES);
  assert(largest_step(run.output) <= 2 * SLOPE);
}

static void test_bounds(void) {
  /* Nothing pushed: an empty flush settles nothing. */
  run_t run = {0};
  h2_gizclaw_stretch_reset(&s_stretch);
  flush(&run);
  assert(run.output == 0 && run.source == 0);
  /* Too little input for a step: the flush returns it unchanged. */
  h2_gizclaw_stretch_reset(&s_stretch);
  assert(h2_gizclaw_stretch_push(&s_stretch, s_source, 100u) == 100u);
  const int16_t *out = NULL;
  size_t count = 0;
  uint64_t source = 0;
  assert(!h2_gizclaw_stretch_step(&s_stretch, 800u, &out, &count, &source));
  flush(&run);
  assert(run.output == 100u && run.source == 100u);
  assert(!memcmp(s_output, s_source, 100u * sizeof(int16_t)));
  /* The input is bounded: a full buffer takes nothing more. */
  h2_gizclaw_stretch_reset(&s_stretch);
  assert(h2_gizclaw_stretch_push(&s_stretch, s_source, SOURCE_SAMPLES) ==
         H2_GIZCLAW_STRETCH_INPUT);
  assert(h2_gizclaw_stretch_push(&s_stretch, s_source, 1u) == 0u);
  /* Rates outside the supported range are clamped, not trusted. */
  assert(h2_gizclaw_stretch_step(&s_stretch, 1u, &out, &count, &source));
  assert(source == H2_GIZCLAW_STRETCH_STEP * H2_GIZCLAW_STRETCH_RATE_MIN / 1000u);
  assert(h2_gizclaw_stretch_step(&s_stretch, 100000u, &out, &count, &source));
  assert(source == H2_GIZCLAW_STRETCH_STEP * H2_GIZCLAW_STRETCH_RATE_MAX / 1000u);
}

int main(void) {
  build_source();
  test_recorded_speed_is_identity();
  test_rate_scales_length_and_keeps_pitch();
  test_switching_rates_is_seamless();
  test_bounds();
  puts("time stretch tests passed");
  return 0;
}
