#include "h2_libco_internal.h"

static h2_pal_result_t h2_libco_time_get_monotonic_ms(
    void *user, uint64_t *out_ms) {
    h2_libco_t *core = user;
    if (core == NULL || out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_ms = core->config.now_ms(core->config.user);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_time_get_monotonic_us(
    void *user, uint64_t *out_us) {
    h2_libco_t *core = user;
    if (core == NULL || out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (core->config.time_source != NULL) {
        return h2_pal_time_get_monotonic_us(core->config.time_source, out_us);
    }
    *out_us = core->config.now_ms(core->config.user) * UINT64_C(1000);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_time_get_wall_ms(
    void *user, uint64_t *out_ms) {
    h2_libco_t *core = user;
    if (core == NULL || out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_pal_time_get_wall_ms(core->config.time_source, out_ms);
}

static h2_pal_result_t h2_libco_time_set_wall_ms(
    void *user, uint64_t wall_ms) {
    h2_libco_t *core = user;
    if (core == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_pal_time_set_wall_ms(core->config.time_source, wall_ms);
}

static h2_pal_result_t h2_libco_time_get_wall_status(
    void *user, h2_pal_time_wall_status_t *out_status) {
    h2_libco_t *core = user;
    if (core == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_pal_time_get_wall_status(core->config.time_source, out_status);
}

static h2_pal_result_t h2_libco_time_sleep_ms(void *user, uint32_t ms) {
    h2_libco_t *core = user;
    h2_libco_result_t result;
    h2_libco_task_t *task;
    if (core == NULL || !h2_libco_internal_task_context(core)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (ms == 0u) {
        result = h2_libco_yield(core);
    } else {
        task = h2_libco_internal_current_task(core);
        result = h2_libco_wait(core, (uintptr_t)task, ms);
        if (result == H2_LIBCO_ERR_TIMEOUT) {
            return H2_PAL_OK;
        }
    }
    return h2_libco_internal_to_pal(result);
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = h2_libco_time_get_monotonic_ms,
    .get_monotonic_us = h2_libco_time_get_monotonic_us,
    .get_wall_ms = h2_libco_time_get_wall_ms,
    .set_wall_ms = h2_libco_time_set_wall_ms,
    .get_wall_status = h2_libco_time_get_wall_status,
    .sleep_ms = h2_libco_time_sleep_ms,
};

const h2_pal_time_vtable_t *h2_libco_internal_time_vtable(void) {
    return &s_time_vtable;
}
