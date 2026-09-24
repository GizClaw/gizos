#include "h2_gizclaw_time_stretch_internal.h"

#include <string.h>

#define SEQUENCE H2_GIZCLAW_STRETCH_SEQUENCE
#define OVERLAP H2_GIZCLAW_STRETCH_OVERLAP
#define SEEK H2_GIZCLAW_STRETCH_SEEK
#define STEP H2_GIZCLAW_STRETCH_STEP

/* Buffered source starts after the room reserved for a flush's overlap. */
static int16_t *input(h2_gizclaw_stretch_t *s) { return s->buffer + OVERLAP; }

void h2_gizclaw_stretch_reset(h2_gizclaw_stretch_t *s) {
  s->count = 0;
  s->resume = 0;
  s->carry = 0;
  s->primed = false;
  s->pushed = s->reported = 0;
}

size_t h2_gizclaw_stretch_push(h2_gizclaw_stretch_t *s, const int16_t *in,
                               size_t n) {
  const size_t space = H2_GIZCLAW_STRETCH_INPUT - s->count;
  if (n > space)
    n = space;
  memcpy(input(s) + s->count, in, n * sizeof(*in));
  s->count += n;
  s->pushed += n;
  return n;
}

/* The offset in [0, SEEK) whose first OVERLAP samples are closest to the
 * carried overlap. A candidate stops summing once it cannot win. */
static size_t best_offset(const int16_t *candidates, const int16_t *overlap) {
  size_t best = 0;
  uint32_t best_sad = UINT32_MAX;
  for (size_t offset = 0; offset < SEEK; ++offset) {
    const int16_t *p = candidates + offset;
    uint32_t sad = 0;
    for (size_t i = 0; i < OVERLAP && sad < best_sad; i += 16u) {
      for (size_t j = i; j < i + 16u; ++j) {
        const int32_t diff = (int32_t)p[j] - (int32_t)overlap[j];
        sad += (uint32_t)(diff < 0 ? -diff : diff);
      }
    }
    if (sad < best_sad) {
      best_sad = sad;
      best = offset;
    }
  }
  return best;
}

bool h2_gizclaw_stretch_step(h2_gizclaw_stretch_t *s, uint32_t rate_permille,
                             const int16_t **out, size_t *out_count,
                             uint64_t *source) {
  if (rate_permille < H2_GIZCLAW_STRETCH_RATE_MIN)
    rate_permille = H2_GIZCLAW_STRETCH_RATE_MIN;
  if (rate_permille > H2_GIZCLAW_STRETCH_RATE_MAX)
    rate_permille = H2_GIZCLAW_STRETCH_RATE_MAX;
  const uint32_t total = STEP * rate_permille + s->carry;
  const size_t advance = total / 1000u;
  size_t need = SEEK + SEQUENCE;
  if (advance > need)
    need = advance;
  if (s->count < need)
    return false;
  int16_t *in = input(s);
  /* The first overlap is the next source itself, so the first crossfade is
   * between identical samples and output starts exactly where it left off. */
  if (!s->primed) {
    memcpy(s->overlap, in, sizeof(s->overlap));
    s->primed = true;
  }
  const size_t offset = best_offset(in, s->overlap);
  const int16_t *segment = in + offset;
  for (size_t i = 0; i < OVERLAP; ++i)
    s->output[i] = (int16_t)(((int32_t)s->overlap[i] * (int32_t)(OVERLAP - i) +
                              (int32_t)segment[i] * (int32_t)i) /
                             (int32_t)OVERLAP);
  memcpy(s->output + OVERLAP, segment + OVERLAP,
         (SEQUENCE - 2u * OVERLAP) * sizeof(*segment));
  memcpy(s->overlap, segment + SEQUENCE - OVERLAP, sizeof(s->overlap));
  s->resume = (int64_t)(offset + SEQUENCE) - (int64_t)advance;
  memmove(in, in + advance, (s->count - advance) * sizeof(*in));
  s->count -= advance;
  s->carry = total % 1000u;
  s->reported += advance;
  *out = s->output;
  *out_count = STEP;
  *source = advance;
  return true;
}

void h2_gizclaw_stretch_flush(h2_gizclaw_stretch_t *s, const int16_t **out,
                              size_t *out_count, uint64_t *source) {
  int16_t *in = input(s);
  if (!s->primed) {
    *out = in;
    *out_count = s->count;
  } else {
    size_t from = s->resume > 0 ? (size_t)s->resume : 0u;
    if (from > s->count)
      from = s->count;
    const size_t tail = s->count - from;
    memmove(s->buffer + OVERLAP, in + from, tail * sizeof(*in));
    memcpy(s->buffer, s->overlap, sizeof(s->overlap));
    *out = s->buffer;
    *out_count = OVERLAP + tail;
  }
  *source = s->pushed - s->reported;
  h2_gizclaw_stretch_reset(s);
}
