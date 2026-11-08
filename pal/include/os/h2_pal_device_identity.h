#ifndef H2_PAL_DEVICE_IDENTITY_H
#define H2_PAL_DEVICE_IDENTITY_H

#include "core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_DEVICE_IDENTITY_MAX 32u

/** Public device identifier, stable across reboot and transport
 * reconfiguration. The provider chooses a factory or persistently provisioned
 * identity. These bytes are not a credential and need not be the current
 * transport address.
 */
typedef struct h2_pal_device_identity {
  uint8_t bytes[H2_PAL_DEVICE_IDENTITY_MAX];
  size_t size;
} h2_pal_device_identity_t;

typedef struct h2_pal_device_identity_vtable {
  /** Synchronously copy the identifier into caller-owned storage. */
  h2_pal_result_t (*get)(void* user, h2_pal_device_identity_t* out_identity);
} h2_pal_device_identity_vtable_t;

typedef struct h2_pal_device_identity_api {
  void* user;
  const h2_pal_device_identity_vtable_t* vtable;
} h2_pal_device_identity_api_t;

/** Output is cleared on every failure. No provider buffer is retained. */
static inline h2_pal_result_t h2_pal_device_identity_get(
    const h2_pal_device_identity_api_t* api,
    h2_pal_device_identity_t* out_identity) {
  h2_pal_result_t rc;
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  if (api == NULL) return H2_PAL_ERR_UNSUPPORTED;
  if (api->vtable == NULL || api->vtable->get == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  rc = api->vtable->get(api->user, out_identity);
  if (rc == H2_PAL_OK && out_identity->size == 0u)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && out_identity->size > sizeof(out_identity->bytes))
    rc = H2_PAL_ERR_TRUNCATED;
  if (rc != H2_PAL_OK) memset(out_identity, 0, sizeof(*out_identity));
  return rc;
}

#ifdef __cplusplus
}
#endif

#endif
