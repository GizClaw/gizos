#ifndef H2_GIZCLAW_AUDIO_PACER_H
#define H2_GIZCLAW_AUDIO_PACER_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

#define H2_GIZCLAW_AUDIO_PERIOD_MS 20u

/* Preserve the previous deadline rather than adding a sleep to encoding time.
 * An overrun resumes immediately, without a burst of missed-frame catch-up. */
static inline h2_pal_result_t
h2_gizclaw_audio_next_deadline(uint64_t deadline, uint64_t completed,
                               uint64_t *out_deadline) {
  if (out_deadline == NULL ||
      deadline > UINT64_MAX - H2_GIZCLAW_AUDIO_PERIOD_MS)
    return H2_PAL_ERR_INVALID_ARG;
  deadline += H2_GIZCLAW_AUDIO_PERIOD_MS;
  *out_deadline = deadline < completed ? completed : deadline;
  return H2_PAL_OK;
}

#endif
