#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_power_get_capabilities(
    void* p0, h2_pal_power_capabilities_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_boot_info(
    void* p0, h2_pal_power_boot_info_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_state(void* p0,
                                                   h2_pal_power_state_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_list_boot_partitions(
    void* p0, h2_pal_power_boot_partition_cb_t p1, void* p2) {
  (void)p0;
  (void)p1;
  (void)p2;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_running_boot_partition(
    void* p0, h2_pal_power_boot_partition_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_next_boot_partition(
    void* p0, h2_pal_power_boot_partition_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_set_next_boot_partition(void* p0,
                                                                 uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_set_hold(void* p0, int p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_hold(
    void* p0, h2_pal_power_hold_state_t* p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_shutdown(void* p0, uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_reboot(void* p0, uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_sleep(void* p0, uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_deep_sleep(void* p0, uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_set_deep_sleep_wake_timer(
    void* p0, uint32_t p1) {
  (void)p0;
  (void)p1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_prepare_next_boot_partition(
    void* user, const h2_pal_power_boot_request_t* request,
    h2_pal_power_native_evidence_t* out_evidence) {
  (void)user;
  (void)request;
  (void)out_evidence;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_get_image_state(
    void* user, uint32_t partition_id, h2_pal_power_image_state_t* out_state) {
  (void)user;
  (void)partition_id;
  (void)out_state;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_confirm_running_image(
    void* user, const h2_pal_power_native_evidence_t* evidence,
    h2_pal_power_image_confirmation_t* out_confirmation) {
  (void)user;
  (void)evidence;
  (void)out_confirmation;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_power_feed_watchdog(void* user) {
  (void)user;
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
    .set_deep_sleep_wake_timer = unsupported_power_set_deep_sleep_wake_timer,
    .prepare_next_boot_partition =
        unsupported_power_prepare_next_boot_partition,
    .get_image_state = unsupported_power_get_image_state,
    .confirm_running_image = unsupported_power_confirm_running_image,
    .feed_watchdog = unsupported_power_feed_watchdog,
};
static const h2_pal_power_api_t unsupported_power_api = {
    .user = NULL, .vtable = &unsupported_power_vtable};
const h2_pal_power_api_t* h2_pal_unsupported_power_api(void) {
  return &unsupported_power_api;
}
