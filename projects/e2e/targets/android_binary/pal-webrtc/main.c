#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <string.h>

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palwebrtc_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jstring report, jstring version, jstring offer,
    jstring stun) {
  (void)type;
  jstring strings[] = {report, version, offer, stun};
  const char *values[4] = {0};
  int rc = H2_PAL_OK;
  for (unsigned index = 0u; index < 4u; ++index) {
    values[index] = (*env)->GetStringUTFChars(env, strings[index], NULL);
    if (values[index] == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
  }
  if (rc == H2_PAL_OK) {
    h2_android_http_t *http = NULL;
    h2_android_webrtc_t *owner = NULL;
    h2_pal_webrtc_e2e_result_t result = {0};
    rc = h2_android_platform_set_image_version(values[1]);
    if (rc == H2_PAL_OK)
      rc = h2_android_http_create(NULL, 0u, &http);
    if (rc == H2_PAL_OK)
      rc = h2_android_webrtc_create(&owner);
    if (rc == H2_PAL_OK) {
      h2_runtime_config_t config = h2_android_app_host_config();
      config.http = h2_android_http_api(http);
      config.webrtc = h2_android_webrtc_api(owner);
      rc = h2_webrtc_mobile_run(config, values[2], values[3], &result);
    }
    int teardown = h2_android_webrtc_destroy(&owner);
    h2_android_http_destroy(http);
    if (teardown == H2_PAL_OK)
      teardown = h2_android_platform_core_shutdown();
    rc = h2_webrtc_mobile_report(values[0], "android-emulator", values[1],
                                 &result, rc, teardown);
  }
  for (unsigned index = 0u; index < 4u; ++index)
    if (values[index] != NULL)
      (*env)->ReleaseStringUTFChars(env, strings[index], values[index]);
  return rc;
}
