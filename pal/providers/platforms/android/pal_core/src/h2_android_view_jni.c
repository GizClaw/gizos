#include "h2_android_platform.h"
#include <stdint.h>

JNIEXPORT jlong JNICALL
Java_com_gizclaw_gizos_pal_H2AndroidPlatform_nativeCreate(
    JNIEnv* env, jclass type, jobject view, jint width, jint height) {
  (void)type;
  const h2_android_platform_config_t config = {.display_width = width,
                                               .display_height = height};
  return (jlong)(uintptr_t)h2_android_platform_create(env, view, &config);
}
JNIEXPORT jint JNICALL Java_com_gizclaw_gizos_pal_H2AndroidPlatform_nativeCopy(
    JNIEnv* env, jclass type, jlong handle, jobject bitmap) {
  (void)type;
  return h2_android_platform_copy_frame(
      (h2_android_platform_t*)(uintptr_t)handle, env, bitmap);
}
JNIEXPORT void JNICALL
Java_com_gizclaw_gizos_pal_H2AndroidPlatform_nativePointer(
    JNIEnv* env, jclass type, jlong handle, jint x, jint y, jboolean down) {
  (void)env;
  (void)type;
  h2_android_platform_update_pointer((h2_android_platform_t*)(uintptr_t)handle,
                                     x, y, down != 0);
}
JNIEXPORT void JNICALL
Java_com_gizclaw_gizos_pal_H2AndroidPlatform_nativeDestroy(JNIEnv* env,
                                                           jclass type,
                                                           jlong handle) {
  (void)env;
  (void)type;
  h2_android_platform_destroy((h2_android_platform_t*)(uintptr_t)handle);
}
