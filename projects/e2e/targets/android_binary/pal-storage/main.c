#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "runner.h"
#include <jni.h>

static int teardown(void *owner) {
  h2_android_storage_destroy(owner);
  return h2_android_platform_core_shutdown();
}
JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palstorage_MainActivity_nativeRun(JNIEnv *env,jclass type,
    jstring directory,jstring report,jint phase,jlong nonce,jstring version) {
  (void)type;
  const char *dir=(*env)->GetStringUTFChars(env,directory,NULL);
  if (!dir) return H2_PAL_ERR_NO_MEMORY;
  const char *path=(*env)->GetStringUTFChars(env,report,NULL);
  if (!path) {(*env)->ReleaseStringUTFChars(env,directory,dir);return H2_PAL_ERR_NO_MEMORY;}
  const char *image=(*env)->GetStringUTFChars(env,version,NULL);
  if (!image) {(*env)->ReleaseStringUTFChars(env,directory,dir);(*env)->ReleaseStringUTFChars(env,report,path);return H2_PAL_ERR_NO_MEMORY;}
  h2_android_storage_t *storage=NULL;
  int rc=h2_android_platform_set_image_version(image);
  if (!rc) rc=h2_android_storage_create(dir,"/storage",&storage);
  if (!rc) {
    h2_runtime_config_t config=h2_android_app_host_config();
    config.fs=h2_android_storage_fs_api(storage);config.pref=h2_android_storage_pref_api(storage);
    rc=h2_storage_mobile_phase(config,(unsigned)phase,(uint32_t)nonce,path,teardown,storage);
  }
  (*env)->ReleaseStringUTFChars(env,directory,dir);
  (*env)->ReleaseStringUTFChars(env,report,path);
  (*env)->ReleaseStringUTFChars(env,version,image);
  return rc;
}
