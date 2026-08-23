#ifndef H2_PAL_TIME_H
#define H2_PAL_TIME_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_time_wall_source {
    H2_PAL_TIME_WALL_SOURCE_UNKNOWN = 0,
    H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT = 1,
    H2_PAL_TIME_WALL_SOURCE_RTC = 2,
    H2_PAL_TIME_WALL_SOURCE_NTP = 3,
    H2_PAL_TIME_WALL_SOURCE_USER = 4,
    H2_PAL_TIME_WALL_SOURCE_SERVER_ALIGNED = 5,
} h2_pal_time_wall_source_t;

typedef struct h2_pal_time_wall_status {
    uint8_t valid;
    h2_pal_time_wall_source_t source;
} h2_pal_time_wall_status_t;

typedef struct h2_pal_time_vtable {
    h2_pal_result_t (*get_monotonic_ms)(void *user, uint64_t *out_ms);
    h2_pal_result_t (*get_monotonic_us)(void *user, uint64_t *out_us);
    h2_pal_result_t (*get_wall_ms)(void *user, uint64_t *out_ms);
    h2_pal_result_t (*set_wall_ms)(void *user, uint64_t wall_ms);
    h2_pal_result_t (*get_wall_status)(void *user, h2_pal_time_wall_status_t *out_status);
    h2_pal_result_t (*sleep_ms)(void *user, uint32_t ms);
} h2_pal_time_vtable_t;

typedef struct h2_pal_time_api {
    void *user;
    const h2_pal_time_vtable_t *vtable;
} h2_pal_time_api_t;

static inline h2_pal_result_t h2_pal_time_get_monotonic_ms(
    const h2_pal_time_api_t *api,
    uint64_t *out_ms) {
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_monotonic_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_monotonic_ms(api->user, out_ms);
}

static inline h2_pal_result_t h2_pal_time_get_monotonic_us(
    const h2_pal_time_api_t *api,
    uint64_t *out_us) {
    if (out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_monotonic_us == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_monotonic_us(api->user, out_us);
}

static inline h2_pal_result_t h2_pal_time_get_wall_ms(
    const h2_pal_time_api_t *api,
    uint64_t *out_ms) {
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_wall_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_wall_ms(api->user, out_ms);
}

static inline h2_pal_result_t h2_pal_time_sleep_ms(
    const h2_pal_time_api_t *api,
    uint32_t ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->sleep_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->sleep_ms(api->user, ms);
}

static inline h2_pal_result_t h2_pal_time_set_wall_ms(
    const h2_pal_time_api_t *api,
    uint64_t wall_ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->set_wall_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_wall_ms(api->user, wall_ms);
}

static inline h2_pal_result_t h2_pal_time_get_wall_status(
    const h2_pal_time_api_t *api,
    h2_pal_time_wall_status_t *out_status) {
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_wall_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_wall_status(api->user, out_status);
}

static inline uint64_t h2_pal_time_elapsed_ms(uint64_t start_ms, uint64_t end_ms) {
    return end_ms - start_ms;
}

static inline uint64_t h2_pal_time_deadline_ms(uint64_t start_ms, uint32_t timeout_ms) {
    return start_ms + (uint64_t)timeout_ms;
}

static inline int h2_pal_time_deadline_expired(uint64_t now_ms, uint64_t deadline_ms) {
    return (now_ms - deadline_ms) < (UINT64_C(1) << 63);
}

#ifdef __cplusplus
}
#endif

#endif
