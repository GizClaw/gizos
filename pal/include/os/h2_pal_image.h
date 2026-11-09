#ifndef H2_PAL_IMAGE_H
#define H2_PAL_IMAGE_H

#include "core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** STORED is the exact artifact byte stream accepted by begin/write and used
 * for destination verification. It includes storage encoding when present.
 * NATIVE is an optional decoded executable view with its own logical offsets
 * and capacity. Neither view is implicitly interchangeable with Disk bytes.
 */
typedef enum h2_pal_image_view {
  H2_PAL_IMAGE_VIEW_STORED = 0,
  H2_PAL_IMAGE_VIEW_NATIVE = 1,
} h2_pal_image_view_t;

typedef enum h2_pal_image_flag {
  H2_PAL_IMAGE_FLAG_READABLE = 1u << 0,
  H2_PAL_IMAGE_FLAG_WRITABLE = 1u << 1,
  H2_PAL_IMAGE_FLAG_SIZE_KNOWN = 1u << 2,
  H2_PAL_IMAGE_FLAG_PREPARED = 1u << 3,
  H2_PAL_IMAGE_FLAG_EXECUTION_PARTITION_KNOWN = 1u << 4,
} h2_pal_image_flag_t;

typedef struct h2_pal_image_info {
  uint64_t capacity;
  /** Exact artifact/executable length only when SIZE_KNOWN is set. */
  uint64_t image_size;
  uint32_t flags;
  /** Native-derived link window, only with EXECUTION_PARTITION_KNOWN. */
  uint32_t execution_partition_id;
} h2_pal_image_info_t;

/** Destination storage and requested native execution window are independent.
 * The execution constraint is not evidence about the input: the provider must
 * validate the native image against it before successful finish. Zero requests
 * automatic placement only where the provider safely supports it. Native
 * range, execution-window and protection guards precede destination mutation.
 */
typedef struct h2_pal_image_write_request {
  uint32_t partition_id;
  uint32_t execution_partition_id;
  uint64_t image_size;
} h2_pal_image_write_request_t;

typedef struct h2_pal_image_vtable {
  h2_pal_result_t (*get_info)(void* user, uint32_t partition_id,
                              h2_pal_image_view_t view,
                              h2_pal_image_info_t* out_info);
  h2_pal_result_t (*read)(void* user, uint32_t partition_id,
                          h2_pal_image_view_t view, uint64_t offset, void* data,
                          size_t len);
  h2_pal_result_t (*begin)(void* user,
                           const h2_pal_image_write_request_t* request);
  h2_pal_result_t (*write)(void* user, const void* data, size_t len);
  h2_pal_result_t (*finish)(void* user);
  h2_pal_result_t (*abort)(void* user);
} h2_pal_image_vtable_t;

/** One caller-serialized update transaction per provider instance. begin must
 * reject a running destination and a concurrent transaction. write appends
 * STORED bytes; finish checks the complete native image and leaves a prepared
 * candidate. It never selects a boot partition or publishes a boot header.
 * The caller verifies destination bytes before asking Power to prepare/select.
 * abort is idempotent and is legal after finish, including failed destination
 * verification. It may clear this candidate's native BootInfo even after
 * activation while retaining its activated artifact shadow. It does not cancel
 * unrelated boot policy. Internal cleanup before a new begin must preserve an
 * already published recovery header and must not call this public abort.
 * Activation and platform-specific recovery barriers belong to Power. Reads
 * may inspect the prepared destination without consuming its update session.
 * All operations are synchronous worker calls; buffers remain caller-owned.
 */
typedef struct h2_pal_image_api {
  void* user;
  const h2_pal_image_vtable_t* vtable;
} h2_pal_image_api_t;

static inline h2_pal_result_t h2_pal_image_get_info(
    const h2_pal_image_api_t* api, uint32_t partition_id,
    h2_pal_image_view_t view, h2_pal_image_info_t* out_info) {
  h2_pal_result_t rc;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_info, 0, sizeof(*out_info));
  if (view != H2_PAL_IMAGE_VIEW_STORED && view != H2_PAL_IMAGE_VIEW_NATIVE)
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL || api->vtable->get_info == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = api->vtable->get_info(api->user, partition_id, view, out_info);
  if (rc == H2_PAL_OK &&
      (out_info->flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) != 0u &&
      out_info->image_size > out_info->capacity)
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK) memset(out_info, 0, sizeof(*out_info));
  return rc;
}

static inline h2_pal_result_t h2_pal_image_read(const h2_pal_image_api_t* api,
                                                uint32_t partition_id,
                                                h2_pal_image_view_t view,
                                                uint64_t offset, void* data,
                                                size_t len) {
  if ((data == NULL && len != 0u) || (uint64_t)len > UINT64_MAX - offset ||
      (view != H2_PAL_IMAGE_VIEW_STORED && view != H2_PAL_IMAGE_VIEW_NATIVE))
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL || api->vtable->read == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->read(api->user, partition_id, view, offset, data, len);
}

static inline h2_pal_result_t h2_pal_image_begin(
    const h2_pal_image_api_t* api,
    const h2_pal_image_write_request_t* request) {
  if (request == NULL || request->image_size == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL || api->vtable->begin == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->begin(api->user, request);
}

static inline h2_pal_result_t h2_pal_image_write(const h2_pal_image_api_t* api,
                                                 const void* data, size_t len) {
  if (data == NULL && len != 0u) return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL || api->vtable->write == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->write(api->user, data, len);
}

static inline h2_pal_result_t h2_pal_image_finish(
    const h2_pal_image_api_t* api) {
  if (api == NULL || api->vtable == NULL || api->vtable->finish == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->finish(api->user);
}

static inline h2_pal_result_t h2_pal_image_abort(
    const h2_pal_image_api_t* api) {
  if (api == NULL || api->vtable == NULL || api->vtable->abort == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->abort(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
