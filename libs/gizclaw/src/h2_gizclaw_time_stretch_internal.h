#ifndef H2_GIZCLAW_TIME_STRETCH_INTERNAL_H
#define H2_GIZCLAW_TIME_STRETCH_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Pitch-preserving tempo change for 16 kHz mono S16 speech (SOLA). Each step
 * emits one sequence less its overlap and advances the source by that many
 * samples times the rate; the start of the sequence is searched within the
 * seek window for the best match (sum of absolute differences) with the
 * carried overlap, then crossfaded linearly. Everything is integer. */
#define H2_GIZCLAW_STRETCH_SEQUENCE 640u /* 40 ms */
#define H2_GIZCLAW_STRETCH_OVERLAP 128u  /* 8 ms */
#define H2_GIZCLAW_STRETCH_SEEK 240u     /* 15 ms */
#define H2_GIZCLAW_STRETCH_STEP                                                \
  (H2_GIZCLAW_STRETCH_SEQUENCE - H2_GIZCLAW_STRETCH_OVERLAP)
#define H2_GIZCLAW_STRETCH_INPUT 2048u
#define H2_GIZCLAW_STRETCH_RATE_MIN 500u
#define H2_GIZCLAW_STRETCH_RATE_MAX 2000u

/* Caller-allocated, fixed size (about 5.5 KiB); nothing allocated inside.
 * The input keeps room in front for the overlap so a flush is built in
 * place. */
typedef struct h2_gizclaw_stretch {
  int16_t buffer[H2_GIZCLAW_STRETCH_OVERLAP + H2_GIZCLAW_STRETCH_INPUT];
  int16_t overlap[H2_GIZCLAW_STRETCH_OVERLAP];
  int16_t output[H2_GIZCLAW_STRETCH_STEP];
  size_t count;
  /* The source sample after the carried overlap, relative to the first
   * buffered sample; negative once a fast advance has moved past it. */
  int64_t resume;
  uint32_t carry;
  bool primed;
  /* Source samples pushed, and those already accounted for by output. */
  uint64_t pushed, reported;
} h2_gizclaw_stretch_t;

void h2_gizclaw_stretch_reset(h2_gizclaw_stretch_t *s);
/* Buffers up to n samples; returns how many were taken (0 when full). */
size_t h2_gizclaw_stretch_push(h2_gizclaw_stretch_t *s, const int16_t *in,
                               size_t n);
/* One step at rate_permille (clamped to MIN..MAX) when enough input is
 * buffered. On true, *out points at *out_count samples valid until the next
 * call and *source is how many source samples they stand for. */
bool h2_gizclaw_stretch_step(h2_gizclaw_stretch_t *s, uint32_t rate_permille,
                             const int16_t **out, size_t *out_count,
                             uint64_t *source);
/* Ends stretching: the carried overlap, then the buffered source after it,
 * unchanged, so playback continues seamlessly at the recorded speed. *source
 * settles every pushed sample not yet reported. Leaves s reset; *out stays
 * valid until the next push. */
void h2_gizclaw_stretch_flush(h2_gizclaw_stretch_t *s, const int16_t **out,
                              size_t *out_count, uint64_t *source);
#ifdef __cplusplus
}
#endif
#endif
