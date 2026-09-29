#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "h2_pal_display_e2e.h"
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct observation {
  JNIEnv *env;
  jobject view;
  jmethodID capture;
} observation_t;
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  observation_t *o = user;
  JNIEnv *env = o->env;
  jintArray expected = (*env)->NewIntArray(env, width * height);
  if (!expected)
    return H2_DISPLAY_ERR_NO_MEMORY;
  jint *reference = malloc((size_t)width * height * sizeof(jint));
  if (!reference) {
    (*env)->DeleteLocalRef(env, expected);
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  for (int i = 0; i < width * height; ++i)
    reference[i] = pixels[i];
  (*env)->SetIntArrayRegion(env, expected, 0, width * height, reference);
  free(reference);
  jstring case_id = (*env)->NewStringUTF(env, id);
  jint rc = (*env)->CallIntMethod(env, o->view, o->capture, expected,
                                  brightness, case_id);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    rc = H2_DISPLAY_ERR_IO;
  }
  (*env)->DeleteLocalRef(env, expected);
  (*env)->DeleteLocalRef(env, case_id);
  printf("H2_DISPLAY_OBSERVATION case=%s source=Android-view brightness=%u "
         "rc=%d\n",
         id, brightness, rc);
  return rc;
}
JNIEXPORT jlong JNICALL
Java_com_haivivi_gizos_e2e_paldisplay_MainActivity_nativeCreate(JNIEnv *env,
                                                                jclass type,
                                                                jobject view) {
  (void)type;
  const h2_android_platform_config_t config = {.display_width = 96,
                                               .display_height = 80};
  return (jlong)(uintptr_t)h2_android_platform_create(env, view, &config);
}
JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_paldisplay_MainActivity_nativeCopy(JNIEnv *env,
                                                              jclass type,
                                                              jlong owner,
                                                              jobject bitmap) {
  (void)type;
  return h2_android_platform_copy_frame(
      (h2_android_platform_t *)(uintptr_t)owner, env, bitmap);
}
JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_paldisplay_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jlong owner, jobject view, jstring path) {
  (void)type;
  h2_android_platform_t *platform = (h2_android_platform_t *)(uintptr_t)owner;
  const char *report = (*env)->GetStringUTFChars(env, path, NULL);
  if (!report)
    return H2_DISPLAY_ERR_NO_MEMORY;
  FILE *sink = freopen(report, "w", stdout);
  (*env)->ReleaseStringUTFChars(env, path, report);
  if (!sink)
    return H2_DISPLAY_ERR_IO;
  jclass cls = (*env)->GetObjectClass(env, view);
  observation_t observer = {
      .env = env,
      .view = view,
      .capture =
          (*env)->GetMethodID(env, cls, "capture", "([IILjava/lang/String;)I")};
  (*env)->DeleteLocalRef(env, cls);
  h2_runtime_config_t cfg = h2_android_app_host_config();
  cfg.display = h2_android_platform_display_api(platform);
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&cfg, &runtime);
  h2_pal_display_e2e_result_t result = {0};
  const h2_pal_display_e2e_config_t test = {
      .supported_formats = 1, .observe = observe, .user = &observer};
  if (!rc)
    rc = h2_pal_display_e2e_run(runtime, &test, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  cls = (*env)->GetObjectClass(env, view);
  jmethodID retire = (*env)->GetMethodID(env, cls, "retire", "()V");
  (*env)->CallVoidMethod(env, view, retire);
  (*env)->DeleteLocalRef(env, cls);
  if ((*env)->ExceptionCheck(env))
    return H2_DISPLAY_ERR_IO;
  h2_android_platform_destroy(platform);
  int teardown = h2_android_platform_core_shutdown();
  h2_pal_display_e2e_print(&result, "android-emulator", rc, teardown);
  fflush(stdout);
  return rc || teardown || !result.qualified ? 1 : 0;
}
