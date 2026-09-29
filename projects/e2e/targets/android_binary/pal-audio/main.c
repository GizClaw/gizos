#include "h2_android_platform.h"
#include "mobile_runner.h"

#include <jni.h>

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palaudio_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jobject view, jstring path, jstring version) {
  (void)type;
  if (view == NULL || path == NULL || version == NULL) return H2_PAL_ERR_INVALID_ARG;
  const char *file = (*env)->GetStringUTFChars(env, path, NULL);
  const char *image_version = (*env)->GetStringUTFChars(env, version, NULL);
  if (file == NULL || image_version == NULL) {
    if (file != NULL) (*env)->ReleaseStringUTFChars(env, path, file);
    if (image_version != NULL)
      (*env)->ReleaseStringUTFChars(env, version, image_version);
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_android_platform_config_t config = {
      .display_width = 1, .display_height = 1};
  h2_android_platform_t *platform = h2_android_platform_create(env, view, &config);
  h2_pal_audio_e2e_result_t result = {0};
  int rc = platform == NULL ? H2_PAL_ERR_UNAVAILABLE
                            : h2_audio_mobile_run(h2_android_platform_audio_api(platform),
                                h2_android_platform_time_api(), &result);
  h2_android_platform_destroy(platform);
  const int teardown = h2_android_platform_core_shutdown();
  rc = h2_audio_mobile_report(file, "android-emulator", image_version,
                              &result, rc, teardown);
  (*env)->ReleaseStringUTFChars(env, path, file);
  (*env)->ReleaseStringUTFChars(env, version, image_version);
  return rc;
}
