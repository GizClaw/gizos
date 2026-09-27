#include "h2_ios_platform.h"
#include "h2_posix_core.h"
#import <Foundation/Foundation.h>
#include <string.h>
static int firmware_current(void *user, h2_pal_firmware_info_t *out) {
  (void)user;
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  @autoreleasepool {
    NSString *version =
        NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
    const char *text = version.UTF8String;
    if (!text || !text[0])
      return H2_PAL_ERR_UNAVAILABLE;
    size_t n = strlen(text);
    if (n >= sizeof(out->version))
      return H2_PAL_ERR_NO_SPACE;
    memcpy(out->version, text, n + 1);
    return H2_PAL_OK;
  }
}
static const h2_pal_firmware_info_vtable_t info_methods = {
    .get_current = firmware_current};
static const h2_pal_firmware_info_api_t info_api = {.vtable = &info_methods};
const h2_pal_mem_api_t *h2_ios_platform_mem_api(void) {
  return h2_posix_core_mem_api();
}
const h2_pal_time_api_t *h2_ios_platform_time_api(void) {
  return h2_posix_core_time_api();
}
const h2_pal_task_api_t *h2_ios_platform_task_api(void) {
  return h2_posix_core_task_api();
}
const h2_pal_queue_api_t *h2_ios_platform_queue_api(void) {
  return h2_posix_core_queue_api();
}
const h2_pal_sync_api_t *h2_ios_platform_sync_api(void) {
  return h2_posix_core_sync_api();
}
const h2_pal_timer_api_t *h2_ios_platform_timer_api(void) {
  return h2_posix_core_timer_api();
}
const h2_pal_log_api_t *h2_ios_platform_log_api(void) {
  return h2_posix_core_log_api();
}
const h2_pal_firmware_info_api_t *h2_ios_platform_firmware_info_api(void) {
  return &info_api;
}
const h2_pal_system_event_api_t *h2_ios_system_event_api(void) {
  return h2_posix_system_event_api();
}
h2_pal_result_t h2_ios_platform_task_allocation_fault(int after) {
  return h2_posix_core_task_allocation_fault(after);
}
h2_pal_result_t h2_ios_platform_core_shutdown(void) {
  return h2_posix_core_shutdown();
}
h2_pal_result_t
h2_ios_platform_get_resource_stats(h2_ios_platform_resource_stats_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  h2_posix_core_resources_t v;
  h2_pal_result_t rc = h2_posix_core_resources(&v);
  if (rc != H2_PAL_OK)
    return rc;
  *out = (h2_ios_platform_resource_stats_t){
      v.tasks,   v.task_stack_bytes, v.queues,
      v.mutexes, v.semaphores,       v.conditions,
      v.timers,  v.allocations,      v.allocation_bytes};
  return H2_PAL_OK;
}
