#include "h2_android_net.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <string.h>

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palnettls_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jstring report, jstring version, jstring host,
    jint port, jstring session, jstring ca, jstring wrong, jstring dns_host,
    jstring dns_ip) {
  (void)type;
  jstring strings[] = {report, version, host,     session,
                       ca,     wrong,   dns_host, dns_ip};
  const char *values[8] = {0};
  int rc = H2_PAL_OK;
  for (unsigned i = 0u; i < 8u; ++i) {
    if (!strings[i]) {
      rc = H2_PAL_ERR_INVALID_ARG;
      break;
    }
    values[i] = (*env)->GetStringUTFChars(env, strings[i], NULL);
    if (!values[i]) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
  }
  if (rc == H2_PAL_OK && port > 0 && port <= 65535) {
    h2_android_net_t *owner = NULL;
    h2_net_tls_result_t result = {0};
    rc = h2_android_platform_set_image_version(values[1]);
    if (rc == H2_PAL_OK)
      rc = h2_android_net_create(&owner);
    if (rc == H2_PAL_OK) {
      h2_runtime_config_t config = h2_android_app_host_config();
      config.net = h2_android_net_api(owner);
      rc = h2_net_tls_mobile_run(config, values[2], (uint16_t)port, values[3],
                                 (const uint8_t *)values[4], strlen(values[4]),
                                 (const uint8_t *)values[5], strlen(values[5]),
                                 values[6], values[7], &result);
    }
    int teardown = h2_android_net_destroy(&owner);
    if (teardown == H2_PAL_OK)
      teardown = h2_android_platform_core_shutdown();
    int written = h2_net_tls_write_report(values[0], "android-emulator",
                                          &result, rc, teardown);
    if (written != H2_PAL_OK)
      rc = written;
  } else if (rc == H2_PAL_OK)
    rc = H2_PAL_ERR_INVALID_ARG;
  for (unsigned i = 0u; i < 8u; ++i)
    if (values[i])
      (*env)->ReleaseStringUTFChars(env, strings[i], values[i]);
  return rc;
}
