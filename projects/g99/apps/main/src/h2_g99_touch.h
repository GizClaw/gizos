#ifndef H2_G99_TOUCH_H
#define H2_G99_TOUCH_H

/* Touch as Main reads it: one press per finger contact, whatever the source.
 *
 * Compatibility models cover legacy sources between two controller reports
 * and hovering pointers:
 *
 * - The Mosaico CST9220 publishes a report every few milliseconds. A read
 *   between two reports finds an empty frame (driver 0.1.1), which the board
 *   previously reported as a release, or fails (driver 0.1.2). LVGL reads again
 * right after every event, so a finger held on the panel turns into a press and
 *   a release on every read cycle.
 * - The old Web platform reported a mouse moving without a pressed button as
 *   a move, which the old LVGL bridge treated as a pressed pointer.
 *
 * The filter reads its source at most once per H2_G99_TOUCH_READ_MS, counts
 * a release only after H2_G99_TOUCH_RELEASE_MS without contact, completes an
 * expired release before reading the next contact, takes a
 * failed read as no news (a press whose source keeps failing for
 * H2_G99_TOUCH_LOST_MS ends where it was), and drops moves without contact.
 * Pure C; no LVGL. */

#include "hal/h2_pal_touch.h"
#include "os/h2_pal_time.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_TOUCH_READ_MS 10u
#define H2_G99_TOUCH_RELEASE_MS 50u
#define H2_G99_TOUCH_LOST_MS 500u

/** Cumulative counters for diagnostics. */
typedef struct h2_g99_touch_stats {
  /** Source reads, and the ones that failed. */
  uint32_t reads;
  uint32_t failed;
  /** Source releases that a returning contact cancelled. */
  uint32_t rejoined;
  /** Moves without contact that were dropped. */
  uint32_t hovers;
} h2_g99_touch_stats_t;

typedef struct h2_g99_touch_filter {
  const h2_pal_touch_api_t* source;
  const h2_pal_time_api_t* time;
  h2_pal_touch_api_t api;
  /* The source reported a contact down and not up again. */
  bool source_down;
  /* The filter reported a press and not yet its release. */
  bool down;
  int32_t x;
  int32_t y;
  /* A source release waiting for confirmation since release_ms. */
  bool releasing;
  uint64_t release_ms;
  /* Source reads failing during the press since fail_ms. */
  bool failing;
  uint64_t fail_ms;
  /* Time of the last source read. */
  bool read_once;
  uint64_t read_ms;
  bool pending_down;
  h2_pal_touch_event_t pending_contact;
  h2_g99_touch_stats_t stats;
} h2_g99_touch_filter_t;

/** Prepares the filter over `source`; both APIs are borrowed. Without a
 * working clock every read reaches the source and a release counts at the
 * next read without contact. */
void h2_g99_touch_filter_init(h2_g99_touch_filter_t* filter,
                              const h2_pal_touch_api_t* source,
                              const h2_pal_time_api_t* time);

/** The filtered touch API; valid while the filter is. */
const h2_pal_touch_api_t* h2_g99_touch_filter_api(
    h2_g99_touch_filter_t* filter);

#ifdef __cplusplus
}
#endif

#endif
