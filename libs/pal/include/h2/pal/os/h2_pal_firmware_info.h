#ifndef H2_PAL_FIRMWARE_INFO_H
#define H2_PAL_FIRMWARE_INFO_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum storage, including NUL, for an embedded firmware version. */
#define H2_PAL_FIRMWARE_VERSION_MAX 96u

/** Caller-owned metadata for the currently running firmware image. */
typedef struct h2_pal_firmware_info {
    /** NUL-terminated version embedded in the currently running firmware. */
    char version[H2_PAL_FIRMWARE_VERSION_MAX];
} h2_pal_firmware_info_t;

typedef struct h2_pal_firmware_info_vtable {
    /**
     * Reads metadata for the currently running firmware image.
     *
     * out_info is caller-owned storage and is valid after this synchronous
     * call returns. Implementations must return a non-empty NUL-terminated
     * version on success and leave out_info empty on failure.
     */
    h2_pal_result_t (*get_current)(
        void *user,
        h2_pal_firmware_info_t *out_info);
} h2_pal_firmware_info_vtable_t;

/** Firmware metadata capability object. */
typedef struct h2_pal_firmware_info_api {
    void *user;
    const h2_pal_firmware_info_vtable_t *vtable;
} h2_pal_firmware_info_api_t;

/**
 * Reads and validates metadata embedded in the currently running firmware.
 *
 * @param api Borrowed capability object. NULL returns unsupported.
 * @param out_info Caller-owned output, cleared to an empty version on failure.
 * @return H2_PAL_OK for a non-empty NUL-terminated version, otherwise a PAL
 * result describing invalid arguments, unsupported capability, invalid
 * metadata, provider failure, or truncation.
 */
static inline h2_pal_result_t h2_pal_firmware_info_get_current(
    const h2_pal_firmware_info_api_t *api,
    h2_pal_firmware_info_t *out_info) {
    h2_pal_result_t rc;
    size_t index;

    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_info->version[0] = '\0';
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->get_current == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = api->vtable->get_current(api->user, out_info);
    if (rc != H2_PAL_OK) {
        out_info->version[0] = '\0';
        return rc;
    }
    if (out_info->version[0] == '\0') {
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (index = 1u; index < sizeof(out_info->version); ++index) {
        if (out_info->version[index] == '\0') {
            return H2_PAL_OK;
        }
    }
    out_info->version[0] = '\0';
    return H2_PAL_ERR_TRUNCATED;
}

#ifdef __cplusplus
}
#endif

#endif
