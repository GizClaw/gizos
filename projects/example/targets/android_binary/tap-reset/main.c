#include "h2_android_platform.h"
#include "h2_mobile_app.h"
#include "h2_smoke_host_runtime.h"

#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct h2_mobile_android_context {
  h2_android_platform_t *host;
  h2_runtime_t *runtime;
  atomic_bool stop;
  pthread_t thread;
  int thread_started;
} h2_mobile_android_context_t;

static h2_mobile_android_context_t *context_from_handle(jlong handle) {
  return (h2_mobile_android_context_t *)(uintptr_t)handle;
}

static int android_should_stop(void *user) {
  h2_mobile_android_context_t *context = user;
  return context == NULL ||
         atomic_load_explicit(&context->stop, memory_order_acquire);
}

static h2_pal_result_t
android_read_pointer(void *user, h2_mobile_pointer_state_t *out_state) {
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_android_platform_read_pointer(user, &out_state->x, &out_state->y,
                                          &out_state->pressed);
}

static void *android_app_thread(void *user) {
  h2_mobile_android_context_t *context = user;
  const h2_mobile_app_config_t config = {
      .platform = H2_MOBILE_PLATFORM_ANDROID,
      .read_pointer = android_read_pointer,
      .pointer_user = context->host,
      .should_stop = android_should_stop,
      .stop_user = context,
  };
  (void)h2_mobile_app_run(context->runtime, &config);
  return NULL;
}

JNIEXPORT jlong JNICALL
Java_com_haivivi_firmwares_smokeapps_tapreset_MainActivity_nativeCreate(
    JNIEnv *env, jclass clazz, jobject view) {
  (void)clazz;
  h2_mobile_android_context_t *context = calloc(1u, sizeof(*context));
  if (context == NULL) {
    return 0;
  }
  const h2_android_platform_config_t platform_config = {
      .display_width = H2_MOBILE_APP_WIDTH,
      .display_height = H2_MOBILE_APP_HEIGHT,
  };
  context->host = h2_android_platform_create(env, view, &platform_config);
  if (context->host == NULL) {
    free(context);
    return 0;
  }
  atomic_init(&context->stop, false);
  return (jlong)(uintptr_t)context;
}

JNIEXPORT jboolean JNICALL
Java_com_haivivi_firmwares_smokeapps_tapreset_MainActivity_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  h2_mobile_android_context_t *context = context_from_handle(handle);
  if (context == NULL || context->thread_started) {
    return JNI_FALSE;
  }
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "android-emulator", "android", "arm64-v8a", h2_android_platform_mem_api(),
      h2_android_platform_time_api(), h2_android_platform_queue_api(),
      h2_android_platform_display_api(context->host));
  if (h2_runtime_init(&runtime_config, &context->runtime) != H2_PAL_OK) {
    return JNI_FALSE;
  }
  if (pthread_create(&context->thread, NULL, android_app_thread, context) !=
      0) {
    h2_runtime_deinit(context->runtime);
    context->runtime = NULL;
    return JNI_FALSE;
  }
  context->thread_started = 1;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_haivivi_firmwares_smokeapps_tapreset_MainActivity_nativeRender(
    JNIEnv *env, jclass clazz, jlong handle, jobject bitmap) {
  (void)clazz;
  h2_mobile_android_context_t *context = context_from_handle(handle);
  if (context != NULL) {
    (void)h2_android_platform_copy_frame(context->host, env, bitmap);
  }
}

JNIEXPORT void JNICALL
Java_com_haivivi_firmwares_smokeapps_tapreset_MainActivity_nativePointer(
    JNIEnv *env, jclass clazz, jlong handle, jint x, jint y, jboolean pressed) {
  (void)env;
  (void)clazz;
  h2_mobile_android_context_t *context = context_from_handle(handle);
  if (context != NULL) {
    h2_android_platform_update_pointer(context->host, x, y,
                                       pressed == JNI_TRUE);
  }
}

JNIEXPORT void JNICALL
Java_com_haivivi_firmwares_smokeapps_tapreset_MainActivity_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  h2_mobile_android_context_t *context = context_from_handle(handle);
  if (context == NULL) {
    return;
  }
  if (context->thread_started) {
    atomic_store_explicit(&context->stop, true, memory_order_release);
    (void)pthread_join(context->thread, NULL);
    context->thread_started = 0;
  }
  if (context->runtime != NULL) {
    h2_runtime_deinit(context->runtime);
  }
  h2_android_platform_destroy(context->host);
  free(context);
}
