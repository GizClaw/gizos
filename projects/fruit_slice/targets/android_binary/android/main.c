#include "h2_android_platform.h"
#include "h2_fruit_slice.h"
#include "h2_fruit_slice_native_host.h"
#include <android/log.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct game_host {
  h2_android_platform_t* platform; /* Borrowed from the Java PAL owner. */
  h2_android_storage_t* storage;
  h2_runtime_t* runtime;
  atomic_bool stopped, paused;
} game_host_t;

static bool should_exit(void* user) {
  game_host_t* host = user;
  while (atomic_load(&host->paused) && !atomic_load(&host->stopped)) {
    const struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
  }
  return atomic_load(&host->stopped);
}

JNIEXPORT jlong JNICALL Java_com_gizclaw_fruitslice_MainActivity_nativeCreate(
    JNIEnv* env, jclass type, jlong platform, jstring directory,
    jstring version) {
  (void)type;
  if (!platform) return 0;
  game_host_t* host = calloc(1, sizeof(*host));
  if (!host) return 0;
  host->platform = (h2_android_platform_t*)(uintptr_t)platform;
  atomic_init(&host->stopped, false);
  atomic_init(&host->paused, true);
  const char* value = (*env)->GetStringUTFChars(env, version, NULL);
  if (!value) {
    free(host);
    return 0;
  }
  int rc = h2_android_platform_set_image_version(value);
  (*env)->ReleaseStringUTFChars(env, version, value);
  if (rc != H2_PAL_OK) {
    free(host);
    return 0;
  }
  value = (*env)->GetStringUTFChars(env, directory, NULL);
  if (!value) {
    free(host);
    return 0;
  }
  rc = h2_android_storage_create(value, "/data", &host->storage);
  (*env)->ReleaseStringUTFChars(env, directory, value);
  if (rc != H2_PAL_OK) {
    free(host);
    return 0;
  }
  return (jlong)(uintptr_t)host;
}

JNIEXPORT jint JNICALL Java_com_gizclaw_fruitslice_MainActivity_nativeRun(
    JNIEnv* env, jclass type, jlong owner) {
  (void)env;
  (void)type;
  game_host_t* host = (game_host_t*)(uintptr_t)owner;
  if (!host) return H2_PAL_ERR_INVALID_ARG;
  h2_runtime_config_t config = h2_fruit_slice_native_defaults();
  config.board = "android";
  config.target = "android";
  config.chip = "arm64";
  config.firmware_info = h2_android_platform_firmware_info_api();
  config.mem = h2_android_platform_mem_api();
  config.log = h2_android_platform_log_api();
  config.time = h2_android_platform_time_api();
  config.task = h2_android_platform_task_api();
  config.queue = h2_android_platform_queue_api();
  config.sync = h2_android_platform_sync_api();
  config.timer = h2_android_platform_timer_api();
  config.system_event = h2_android_system_event_api();
  config.display = h2_android_platform_display_api(host->platform);
  config.touch = h2_android_platform_touch_api(host->platform);
  config.fs = h2_android_storage_fs_api(host->storage);
  config.pref = h2_android_storage_pref_api(host->storage);
  int rc = h2_runtime_init(&config, &host->runtime);
  if (rc == H2_PAL_OK) {
    const h2_fruit_slice_config_t game = {.should_exit = should_exit,
                                          .user = host};
    rc = h2_fruit_slice_run(host->runtime, &game);
  }
  const int destroyed =
      host->runtime ? h2_runtime_destroy(&host->runtime) : H2_PAL_OK;
  __android_log_print(rc || destroyed ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                      "FruitSlice", "game stopped rc=%d destroy=%d", rc,
                      destroyed);
  return rc != H2_PAL_OK ? rc : destroyed;
}
JNIEXPORT void JNICALL Java_com_gizclaw_fruitslice_MainActivity_nativeState(
    JNIEnv* env, jclass type, jlong owner, jboolean paused, jboolean stopped) {
  (void)env;
  (void)type;
  game_host_t* host = (game_host_t*)(uintptr_t)owner;
  if (!host) return;
  atomic_store(&host->paused, paused != 0);
  if (stopped) atomic_store(&host->stopped, true);
}
/* Called on the UI thread only after nativeRun returns. The Java platform
 * owner closes its View after this succeeds; failed joins retain all borrows.
 */
JNIEXPORT jint JNICALL Java_com_gizclaw_fruitslice_MainActivity_nativeDestroy(
    JNIEnv* env, jclass type, jlong owner) {
  (void)env;
  (void)type;
  game_host_t* host = (game_host_t*)(uintptr_t)owner;
  if (!host) return H2_PAL_OK;
  const int rc = host->runtime ? h2_runtime_destroy(&host->runtime) : H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  h2_android_storage_destroy(host->storage);
  free(host);
  return H2_PAL_OK;
}
