#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Observe liblog's actual output and forward it to logd unchanged. The test
 * requires API 30 for this observer; the provider itself still supports 28. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int last_priority;
static char last_tag[128], last_message[1024];
static void (*logd_logger)(const struct __android_log_message *);
static void observe_native_log(const struct __android_log_message *entry) {
  pthread_mutex_lock(&log_lock);
  last_priority = entry->priority;
  snprintf(last_tag, sizeof(last_tag), "%s", entry->tag ? entry->tag : "");
  snprintf(last_message, sizeof(last_message), "%s",
           entry->message ? entry->message : "");
  pthread_mutex_unlock(&log_lock);
  logd_logger(entry);
}
static h2_pal_result_t log_seen(void *user, h2_pal_log_level_t level,
                                const char *scope, const char *message) {
  (void)user;
  static const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                   ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  char expected[1024];
  snprintf(expected, sizeof(expected), "%s: %s", scope, message);
  pthread_mutex_lock(&log_lock);
  int ok = last_priority == priorities[level] &&
           !strcmp(last_tag, "H2Firmwares") && !strcmp(last_message, expected);
  pthread_mutex_unlock(&log_lock);
  return ok ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static h2_pal_result_t resources(void *user, h2_pal_core_resources_t *out) {
  (void)user;
  h2_android_platform_resource_stats_t v;
  int rc = h2_android_platform_get_resource_stats(&v);
  if (rc)
    return rc;
  *out = (h2_pal_core_resources_t){.tasks = v.tasks,
                                   .task_stack_bytes = v.task_stack_bytes,
                                   .queues = v.queues,
                                   .mutexes = v.mutexes,
                                   .semaphores = v.semaphores,
                                   .conditions = v.conditions,
                                   .timers = v.timers,
                                   .firmware_infos = 1,
                                   .allocations = v.allocations,
                                   .allocation_bytes = v.allocation_bytes};
  return H2_PAL_OK;
}
static h2_pal_result_t fault(void *user, int after) {
  (void)user;
  return h2_android_platform_task_allocation_fault(after);
}
JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palcore_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jstring report_path, jstring image_version) {
  (void)type;
  void (*set_logger)(void (*)(const struct __android_log_message *)) =
      dlsym(RTLD_DEFAULT, "__android_log_set_logger");
  int32_t (*set_priority)(int32_t) =
      dlsym(RTLD_DEFAULT, "__android_log_set_minimum_priority");
  logd_logger = dlsym(RTLD_DEFAULT, "__android_log_logd_logger");
  if (!set_logger || !set_priority || !logd_logger)
    return H2_PAL_ERR_UNAVAILABLE;
  const char *path = (*env)->GetStringUTFChars(env, report_path, NULL);
  if (!path)
    return H2_PAL_ERR_NO_MEMORY;
  const char *version = (*env)->GetStringUTFChars(env, image_version, NULL);
  if (!version) {
    (*env)->ReleaseStringUTFChars(env, report_path, path);
    return H2_PAL_ERR_NO_MEMORY;
  }
  int rc = h2_android_platform_set_image_version(version);
  if (rc == H2_PAL_OK) {
    int32_t old_priority = set_priority(ANDROID_LOG_VERBOSE);
    set_logger(observe_native_log);
    const h2_mobile_core_fixture_t fixture = {
        .platform = "android-emulator",
        .image_version = version,
        .runtime_config = h2_android_app_host_config(),
        .tests = {.event_fixture = h2_android_system_event_api(),
                  .observe_log = log_seen,
                  .observe_resources = resources,
                  .task_allocation_fault = fault},
        .shutdown = h2_android_platform_core_shutdown,
    };
    char log_path[4096];
    snprintf(log_path, sizeof(log_path), "%s.log", path);
    int saved_stdout = dup(STDOUT_FILENO);
    int output = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (saved_stdout < 0 || output < 0) {
      rc = H2_PAL_ERR_IO;
    } else {
      fflush(stdout);
      dup2(output, STDOUT_FILENO);
      rc = h2_mobile_core_run(&fixture, path);
      fflush(stdout);
      dup2(saved_stdout, STDOUT_FILENO);
    }
    if (saved_stdout >= 0)
      close(saved_stdout);
    if (output >= 0)
      close(output);
    set_logger(logd_logger);
    set_priority(old_priority);
  }
  (*env)->ReleaseStringUTFChars(env, image_version, version);
  (*env)->ReleaseStringUTFChars(env, report_path, path);
  return rc;
}
