#ifndef H2_GIZCLAW_MHS_H
#define H2_GIZCLAW_MHS_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_MHS_KEY_MAX 64u
#define H2_GIZCLAW_MHS_STRING_MAX 256u

typedef enum h2_gizclaw_mhs_kind {
  H2_GIZCLAW_MHS_BOOL = 1,
  H2_GIZCLAW_MHS_INT = 2,
  H2_GIZCLAW_MHS_DOUBLE = 3,
  H2_GIZCLAW_MHS_STRING = 4,
} h2_gizclaw_mhs_kind_t;

/** Owned inline value. Integers use the JSON safe range; doubles must be
 * finite. Strings (including enum values) are NUL-terminated UTF-8, at most
 * 256 bytes without embedded NUL. Empty string, zero and false are values. */
typedef struct h2_gizclaw_mhs_value {
  h2_gizclaw_mhs_kind_t kind;
  union {
    bool b;
    int64_t i;
    double d;
    char s[H2_GIZCLAW_MHS_STRING_MAX + 1u];
  } value;
} h2_gizclaw_mhs_value_t;

/** Immutable state registration borrowed until Client/Service deinit.
 * Keys match [a-z][a-z0-9]*([.-][a-z0-9]+)* and are at most 64 bytes.
 * out_value starts with the registered kind and a zero value. read is required;
 * write == NULL means read-only. Callbacks run synchronously on the RPC owner,
 * return promptly, and must not stop or destroy the Service. All keys, types
 * and check callbacks are validated before any write. check must not mutate
 * state. write returns the actual applied value in out_value. A failed write
 * does not roll back earlier hardware changes; read again to determine the
 * resulting state. Callback values are copied, never retained. Products own
 * alignment with their RuntimeProfile manifest. */
typedef struct h2_gizclaw_mhs_state {
  const char *device_id;
  const char *state;
  h2_gizclaw_mhs_kind_t kind;
  h2_pal_result_t (*read)(void *user, h2_gizclaw_mhs_value_t *out_value);
  h2_pal_result_t (*check)(void *user, const h2_gizclaw_mhs_value_t *value);
  h2_pal_result_t (*write)(void *user, const h2_gizclaw_mhs_value_t *value,
                           h2_gizclaw_mhs_value_t *out_value);
  void *user;
} h2_gizclaw_mhs_state_t;

#ifdef __cplusplus
}
#endif
#endif
