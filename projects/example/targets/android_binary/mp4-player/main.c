#include "h2_android_platform.h"
#include "h2_mobile_mp4_player.h"
#include "h2_smoke_host_runtime.h"

#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct h2_mobile_android_mp4_context {
  h2_android_platform_t *host;
  h2_runtime_t *runtime;
  uint8_t *media;
  size_t media_size;
  atomic_bool stop;
  pthread_t thread;
  int thread_started;
} h2_mobile_android_mp4_context_t;

static h2_mobile_android_mp4_context_t *context_from_handle(jlong handle) {
  return (h2_mobile_android_mp4_context_t *)(uintptr_t)handle;
}

static int android_mp4_should_stop(void *user) {
  h2_mobile_android_mp4_context_t *context = user;
  return context == NULL ||
         atomic_load_explicit(&context->stop, memory_order_acquire);
}

static void *android_mp4_app_thread(void *user) {
  h2_mobile_android_mp4_context_t *context = user;
  const h2_mobile_mp4_player_config_t config = {
      .media = context->media,
      .media_size = context->media_size,
      .should_stop = android_mp4_should_stop,
      .stop_user = context,
  };
  (void)h2_mobile_mp4_player_run(context->runtime, &config);
  return NULL;
}

JNIEXPORT jlong JNICALL
Java_com_haivivi_firmwares_smokeapps_mp4player_MainActivity_nativeCreate(
    JNIEnv *env, jclass clazz, jobject view, jbyteArray media,
    jint display_width, jint display_height) {
  (void)clazz;
  if (media == NULL || display_width <= 0 || display_height <= 0) {
    return 0;
  }
  const jsize media_size = (*env)->GetArrayLength(env, media);
  if (media_size <= 0) {
    return 0;
  }
  h2_mobile_android_mp4_context_t *context = calloc(1u, sizeof(*context));
  if (context == NULL) {
    return 0;
  }
  context->media = malloc((size_t)media_size);
  if (context->media == NULL) {
    free(context);
    return 0;
  }
  (*env)->GetByteArrayRegion(env, media, 0, media_size,
                             (jbyte *)context->media);
  if ((*env)->ExceptionCheck(env)) {
    free(context->media);
    free(context);
    return 0;
  }
  context->media_size = (size_t)media_size;
  const h2_android_platform_config_t platform_config = {
      .display_width = display_width,
      .display_height = display_height,
  };
  context->host = h2_android_platform_create(env, view, &platform_config);
  if (context->host == NULL) {
    free(context->media);
    free(context);
    return 0;
  }
  atomic_init(&context->stop, false);
  return (jlong)(uintptr_t)context;
}

JNIEXPORT jboolean JNICALL
Java_com_haivivi_firmwares_smokeapps_mp4player_MainActivity_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  h2_mobile_android_mp4_context_t *context = context_from_handle(handle);
  if (context == NULL || context->thread_started) {
    return JNI_FALSE;
  }
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "android-emulator", "android", "arm64-v8a", h2_android_platform_mem_api(),
      h2_android_platform_time_api(), h2_android_platform_queue_api(),
      h2_android_platform_display_api(context->host));
  runtime_config.task = h2_android_platform_task_api();
  runtime_config.log = h2_android_platform_log_api();
  runtime_config.audio = h2_android_platform_audio_api(context->host);
  runtime_config.audio_decoder = h2_android_platform_audio_decoder_api();
  runtime_config.video_decoder = h2_android_platform_video_decoder_api();
  if (h2_runtime_init(&runtime_config, &context->runtime) != H2_PAL_OK) {
    return JNI_FALSE;
  }
  if (pthread_create(&context->thread, NULL, android_mp4_app_thread, context) !=
      0) {
    h2_runtime_deinit(context->runtime);
    context->runtime = NULL;
    return JNI_FALSE;
  }
  context->thread_started = 1;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_haivivi_firmwares_smokeapps_mp4player_MainActivity_nativeRender(
    JNIEnv *env, jclass clazz, jlong handle, jobject bitmap) {
  (void)clazz;
  h2_mobile_android_mp4_context_t *context = context_from_handle(handle);
  if (context != NULL) {
    (void)h2_android_platform_copy_frame(context->host, env, bitmap);
  }
}

JNIEXPORT void JNICALL
Java_com_haivivi_firmwares_smokeapps_mp4player_MainActivity_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  h2_mobile_android_mp4_context_t *context = context_from_handle(handle);
  if (context == NULL) {
    return;
  }
  if (context->thread_started) {
    atomic_store_explicit(&context->stop, true, memory_order_release);
    (void)pthread_join(context->thread, NULL);
  }
  if (context->runtime != NULL) {
    h2_runtime_deinit(context->runtime);
  }
  h2_android_platform_destroy(context->host);
  free(context->media);
  free(context);
}
