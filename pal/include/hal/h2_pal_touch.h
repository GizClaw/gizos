#ifndef H2_PAL_TOUCH_H
#define H2_PAL_TOUCH_H

#include "core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_touch_event_kind {
  H2_PAL_TOUCH_EVENT_DOWN = 1,
  H2_PAL_TOUCH_EVENT_MOVE,
  H2_PAL_TOUCH_EVENT_UP,
  /** The platform ended the contact without a release (e.g. lost capture).
   * Consumers discard the gesture without committing a click or final move. */
  H2_PAL_TOUCH_EVENT_CANCEL,
} h2_pal_touch_event_kind_t;

/** Logical viewport exposed by one single-pointer Touch provider. */
typedef struct h2_pal_touch_info {
  uint32_t width;
  uint32_t height;
} h2_pal_touch_info_t;

/** One calibrated raw pointer edge. */
typedef struct h2_pal_touch_event {
  h2_pal_touch_event_kind_t kind;
  int32_t x;
  int32_t y;
} h2_pal_touch_event_t;

/** Result of a loss-aware poll. Flags default to false. */
typedef struct h2_pal_touch_sample {
  h2_pal_touch_event_t event;
  /** A successful DOWN after a failed poll in this same session starts a
   * verified new contact. The provider may have consumed the prior lift. */
  bool fresh_down;
  /** On error, the provider knows this session cannot acquire again until
   * closed and reopened. The poll still returns the original device error. */
  bool needs_reopen;
} h2_pal_touch_sample_t;

/**
 * Touch provider lifecycle and nonblocking event source.
 *
 * get_info() is valid after open(). poll_event() returns
 * H2_PAL_ERR_WOULD_BLOCK when no complete event is available. close() releases
 * provider resources and must tolerate a successfully opened session.
 */
typedef struct h2_pal_touch_vtable {
  h2_pal_result_t (*open)(void* user);
  h2_pal_result_t (*get_info)(void* user, h2_pal_touch_info_t* out_info);
  h2_pal_result_t (*poll_event)(void* user, h2_pal_touch_event_t* out_event);
  h2_pal_result_t (*close)(void* user);
  /** Optional loss-aware view of the same stream, for gesture recognition.
   * Reports capacity loss before later samples and distinguishes CANCEL from
   * a physical UP. A session uses just one poll method and reader. */
  h2_pal_result_t (*poll_pointer)(void* user, h2_pal_touch_sample_t* sample);
  /** Optional acquisition for providers whose failed open can retain resources.
   * On failure set needs_close only if this call acquired resources whose
   * rollback failed. BUSY from an existing owner transfers no ownership. */
  h2_pal_result_t (*open_pointer)(void* user, bool* needs_close);
} h2_pal_touch_vtable_t;

typedef struct h2_pal_touch_api {
  void* user;
  const h2_pal_touch_vtable_t* vtable;
} h2_pal_touch_api_t;

/** Open the borrowed Touch provider. */
static inline h2_pal_result_t h2_pal_touch_open(const h2_pal_touch_api_t* api) {
  if (api == NULL || api->vtable == NULL || api->vtable->open == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->open(api->user);
}

/** Acquire a pointer session. Every successful open requires close. An
 * extended provider may also transfer a cleanup obligation on failure. */
static inline h2_pal_result_t h2_pal_touch_open_pointer(
    const h2_pal_touch_api_t* api, bool* needs_close) {
  if (needs_close == NULL) return H2_PAL_ERR_INVALID_ARG;
  *needs_close = false;
  h2_pal_result_t rc = api && api->vtable && api->vtable->open_pointer
                           ? api->vtable->open_pointer(api->user, needs_close)
                           : h2_pal_touch_open(api);
  if (rc == H2_PAL_OK) *needs_close = true;
  return rc;
}

/** Read logical viewport metadata for an open provider. */
static inline h2_pal_result_t h2_pal_touch_get_info(
    const h2_pal_touch_api_t* api, h2_pal_touch_info_t* out_info) {
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL || api->vtable->get_info == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_info(api->user, out_info);
}

/** Poll one complete down, move, or up event without blocking. */
static inline h2_pal_result_t h2_pal_touch_poll_event(
    const h2_pal_touch_api_t* api, h2_pal_touch_event_t* out_event) {
  if (out_event == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL || api->vtable->poll_event == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->poll_event(api->user, out_event);
}

/** Poll the provider's loss-aware stream, or its native edge stream on a
 * device without a separate compatibility projection. Nonblocking. */
static inline h2_pal_result_t h2_pal_touch_poll_pointer(
    const h2_pal_touch_api_t* api, h2_pal_touch_sample_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  out->fresh_down = out->needs_reopen = false;
  if (api && api->vtable && api->vtable->poll_pointer)
    return api->vtable->poll_pointer(api->user, out);
  return h2_pal_touch_poll_event(api, &out->event);
}

/** Close the Touch provider and release its session resources. */
static inline h2_pal_result_t h2_pal_touch_close(
    const h2_pal_touch_api_t* api) {
  if (api == NULL || api->vtable == NULL || api->vtable->close == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->close(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
