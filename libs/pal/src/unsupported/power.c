#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_power_get_capabilities(void *p0, h2_pal_power_capabilities_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_boot_info(void *p0, h2_pal_power_boot_info_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_state(void *p0, h2_pal_power_state_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_list_boot_partitions(void *p0, h2_pal_power_boot_partition_cb_t p1, void *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_running_boot_partition(void *p0, h2_pal_power_boot_partition_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_next_boot_partition(void *p0, h2_pal_power_boot_partition_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_set_next_boot_partition(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_set_hold(void *p0, int p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_hold(void *p0, h2_pal_power_hold_state_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_shutdown(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_reboot(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_sleep(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_deep_sleep(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_power_vtable_t unsupported_power_vtable = {
    .get_capabilities = unsupported_power_get_capabilities,
    .get_boot_info = unsupported_power_get_boot_info,
    .get_state = unsupported_power_get_state,
    .list_boot_partitions = unsupported_power_list_boot_partitions,
    .get_running_boot_partition = unsupported_power_get_running_boot_partition,
    .get_next_boot_partition = unsupported_power_get_next_boot_partition,
    .set_next_boot_partition = unsupported_power_set_next_boot_partition,
    .set_hold = unsupported_power_set_hold,
    .get_hold = unsupported_power_get_hold,
    .shutdown = unsupported_power_shutdown,
    .reboot = unsupported_power_reboot,
    .sleep = unsupported_power_sleep,
    .deep_sleep = unsupported_power_deep_sleep,
};
static const h2_pal_power_api_t unsupported_power_api = { .user = NULL, .vtable = &unsupported_power_vtable };
const h2_pal_power_api_t *h2_pal_unsupported_power_api(void) { return &unsupported_power_api; }
