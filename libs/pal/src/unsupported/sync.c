#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_sync_create_mutex(void *p0, const h2_pal_mutex_config_t *p1, h2_pal_mutex_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_destroy_mutex(void *p0, h2_pal_mutex_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_lock_mutex(void *p0, h2_pal_mutex_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_try_lock_mutex(void *p0, h2_pal_mutex_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_unlock_mutex(void *p0, h2_pal_mutex_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_create_semaphore(void *p0, const h2_pal_semaphore_config_t *p1, h2_pal_semaphore_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_destroy_semaphore(void *p0, h2_pal_semaphore_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_take_semaphore(void *p0, h2_pal_semaphore_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_give_semaphore(void *p0, h2_pal_semaphore_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_create_cond(void *p0, const h2_pal_cond_config_t *p1, h2_pal_cond_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_destroy_cond(void *p0, h2_pal_cond_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_wait_cond(void *p0, h2_pal_cond_t *p1, h2_pal_mutex_t *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_signal_cond(void *p0, h2_pal_cond_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sync_broadcast_cond(void *p0, h2_pal_cond_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_sync_vtable_t unsupported_sync_vtable = {
    .create_mutex = unsupported_sync_create_mutex,
    .destroy_mutex = unsupported_sync_destroy_mutex,
    .lock_mutex = unsupported_sync_lock_mutex,
    .try_lock_mutex = unsupported_sync_try_lock_mutex,
    .unlock_mutex = unsupported_sync_unlock_mutex,
    .create_semaphore = unsupported_sync_create_semaphore,
    .destroy_semaphore = unsupported_sync_destroy_semaphore,
    .take_semaphore = unsupported_sync_take_semaphore,
    .give_semaphore = unsupported_sync_give_semaphore,
    .create_cond = unsupported_sync_create_cond,
    .destroy_cond = unsupported_sync_destroy_cond,
    .wait_cond = unsupported_sync_wait_cond,
    .signal_cond = unsupported_sync_signal_cond,
    .broadcast_cond = unsupported_sync_broadcast_cond,
};
static const h2_pal_sync_api_t unsupported_sync_api = { .user = NULL, .vtable = &unsupported_sync_vtable };
const h2_pal_sync_api_t *h2_pal_unsupported_sync_api(void) { return &unsupported_sync_api; }
