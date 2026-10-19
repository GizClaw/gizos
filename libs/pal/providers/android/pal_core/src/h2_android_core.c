#include "h2_android_platform.h"
#include "h2_posix_core.h"
#include <android/log.h>
#include <pthread.h>
#include <string.h>
static pthread_mutex_t version_lock = PTHREAD_MUTEX_INITIALIZER;
static char image_version[H2_PAL_FIRMWARE_VERSION_MAX];
h2_pal_result_t h2_android_platform_set_image_version(const char *version) {
  if (!version || !version[0] || strlen(version) >= sizeof(image_version))
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&version_lock);
  int rc = H2_PAL_OK;
  if (image_version[0] && strcmp(image_version, version))
    rc = H2_PAL_ERR_INVALID_STATE;
  else
    memcpy(image_version, version, strlen(version) + 1);
  pthread_mutex_unlock(&version_lock);
  return rc;
}
static int firmware_current(void *user, h2_pal_firmware_info_t *out) {
  (void)user;
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&version_lock);
  memcpy(out->version, image_version, sizeof(image_version));
  pthread_mutex_unlock(&version_lock);
  return out->version[0] ? H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
}
static int android_log_write(void *user, h2_pal_log_level_t level,
                             const char *scope, const char *message) {
  (void)user;
  static const int priorities[] = {
      ANDROID_LOG_DEBUG,
      ANDROID_LOG_INFO,
      ANDROID_LOG_WARN,
      ANDROID_LOG_ERROR,
  };
  if (level < H2_PAL_LOG_DEBUG || level > H2_PAL_LOG_ERROR || message == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)__android_log_print(priorities[level], "H2Firmwares", "%s: %s",
                            scope == NULL ? "app" : scope, message);
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t s_android_log_vtable = {
    .write = android_log_write,
};
static const h2_pal_log_api_t s_android_log = {
    .user = NULL,
    .vtable = &s_android_log_vtable,
};

static const h2_pal_firmware_info_vtable_t info_methods = {
    .get_current = firmware_current};
static const h2_pal_firmware_info_api_t info_api = {.vtable = &info_methods};
const h2_pal_mem_api_t *h2_android_platform_mem_api(void) {
  return h2_posix_core_mem_api();
}
const h2_pal_time_api_t *h2_android_platform_time_api(void) {
  return h2_posix_core_time_api();
}
const h2_pal_task_api_t *h2_android_platform_task_api(void) {
  return h2_posix_core_task_api();
}
const h2_pal_queue_api_t *h2_android_platform_queue_api(void) {
  return h2_posix_core_queue_api();
}
const h2_pal_sync_api_t *h2_android_platform_sync_api(void) {
  return h2_posix_core_sync_api();
}
const h2_pal_timer_api_t *h2_android_platform_timer_api(void) {
  return h2_posix_core_timer_api();
}
const h2_pal_log_api_t *h2_android_platform_log_api(void) {
  return &s_android_log;
}
const h2_pal_firmware_info_api_t *h2_android_platform_firmware_info_api(void) {
  return &info_api;
}
const h2_pal_system_event_api_t *h2_android_system_event_api(void) {
  return h2_posix_system_event_api();
}
h2_pal_result_t h2_android_platform_task_allocation_fault(int after) {
  return h2_posix_core_task_allocation_fault(after);
}
h2_pal_result_t h2_android_platform_core_shutdown(void) {
  h2_pal_result_t rc = h2_posix_core_shutdown();
  if (rc == H2_PAL_OK) h2_android_platform_crypto_shutdown();
  return rc;
}
h2_pal_result_t h2_android_platform_get_resource_stats(
    h2_android_platform_resource_stats_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  h2_posix_core_resources_t v;
  h2_pal_result_t rc = h2_posix_core_resources(&v);
  if (rc != H2_PAL_OK)
    return rc;
  *out = (h2_android_platform_resource_stats_t){
      v.tasks,   v.task_stack_bytes, v.queues,
      v.mutexes, v.semaphores,       v.conditions,
      v.timers,  v.allocations,      v.allocation_bytes};
  return H2_PAL_OK;
}
