#ifndef H2_GIZCLAW_RESAMPLE_INTERNAL_H
#define H2_GIZCLAW_RESAMPLE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mono PCM16 at 8..48 kHz onto the player's 16 kHz grid. Output sample n of
 * a run that starts at source sample s0 sits at source position
 * (phase + n * rate) / 16000 past s0, stepped exactly in integers so a long
 * track never drifts. A Kaiser-windowed sinc with its cutoff at 7/8 of the
 * lower Nyquist band-limits both ways; 16 kHz input passes through untouched.
 * The state is one fixed struct: no allocation, no libm. */
#define H2_GIZCLAW_RESAMPLE_OUTPUT_RATE 16000u
#define H2_GIZCLAW_RESAMPLE_RATE_MIN 8000u
#define H2_GIZCLAW_RESAMPLE_RATE_MAX 48000u
/* Largest single push: one MPEG-1 Layer III frame. */
#define H2_GIZCLAW_RESAMPLE_INPUT_MAX 1152u
/* Taps on each side of the output position at the highest rate. */
#define H2_GIZCLAW_RESAMPLE_HALF_MAX 48u

typedef struct h2_gizclaw_resample {
  uint32_t rate;
  uint32_t half;
  float step; /* Kernel table index advance per source sample. */
  /* Next output at buf[whole] + num / 16000; buf[0..len) holds the source,
   * with half zeros in front of the first sample. */
  size_t whole, len;
  uint32_t num;
  float buf[H2_GIZCLAW_RESAMPLE_INPUT_MAX + 2u * H2_GIZCLAW_RESAMPLE_HALF_MAX +
            8u];
} h2_gizclaw_resample_t;

/* Start a run at `rate` whose first output lies phase / 16000 source samples
 * after the first source sample pushed; phase < rate. False for a rate
 * outside MIN..MAX or a phase out of range. */
bool h2_gizclaw_resample_reset(h2_gizclaw_resample_t *r, uint32_t rate,
                               uint32_t phase);
/* Output samples a push of `count` source samples can produce at most. */
size_t h2_gizclaw_resample_capacity(const h2_gizclaw_resample_t *r,
                                    size_t count);
/* Consume count <= INPUT_MAX source samples and write every output whose
 * filter window is now complete. out must hold capacity(count) samples. */
size_t h2_gizclaw_resample_push(h2_gizclaw_resample_t *r, const int16_t *in,
                                size_t count, int16_t *out);
/* End of the run: write the outputs that lie before the end of the source,
 * zero-padding their windows. At most capacity(0) samples. */
size_t h2_gizclaw_resample_flush(h2_gizclaw_resample_t *r, int16_t *out);

#endif
