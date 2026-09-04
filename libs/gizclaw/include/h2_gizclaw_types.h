#ifndef H2_GIZCLAW_TYPES_H
#define H2_GIZCLAW_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_client h2_gizclaw_client_t;

/** Caller-owned storage for variable-sized parsed responses.
 * Initialize with a byte buffer, its capacity, and used=0. Parsed pointers
 * remain valid until the buffer is freed/reused, independently of the request
 * and service. Multiple responses may share it; resetting used invalidates
 * previous results. Parsers roll back used and clear outputs on failure.
 * No GizClaw deinit function is needed; the caller owns the buffer itself.
 * This object is not safe for concurrent parsing without caller locking. */
typedef struct h2_gizclaw_resp_storage {
  uint8_t *data;
  size_t capacity;
  size_t used;
} h2_gizclaw_resp_storage_t;

#ifdef __cplusplus
}
#endif

#endif
