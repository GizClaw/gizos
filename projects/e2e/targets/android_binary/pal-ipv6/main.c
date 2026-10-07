#include "h2_android_net.h"
#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct mobile_owner {
  h2_android_net_t *net;
  h2_android_webrtc_t *webrtc;
  h2_pal_ipv6_result_t result;
  char *report_path;
  int rc;
} mobile_owner_t;
static pthread_mutex_t admission = PTHREAD_MUTEX_INITIALIZER;
static mobile_owner_t *retained_owner;

static int cleanup_owner(mobile_owner_t *owner) {
  int rc = h2_ipv6_mobile_cleanup(&owner->result);
  if (rc == H2_PAL_OK)
    rc = h2_android_webrtc_destroy(&owner->webrtc);
  if (rc == H2_PAL_OK)
    rc = h2_android_net_destroy(&owner->net);
  if (rc == H2_PAL_OK)
    rc = h2_android_platform_core_shutdown();
  return rc;
}
static void release_owner(mobile_owner_t *owner) {
  free(owner->report_path);
  free(owner);
}

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palipv6_MainActivity_nativeRun(
    JNIEnv *env, jclass type, jstring report, jstring version, jstring host,
    jint port, jstring session, jstring ca, jstring wrong, jstring dns_host,
    jstring dns_ip, jstring http_url, jstring fallback_url, jint mqtt_port,
    jstring offer, jstring stun, jint dns_port) {
  (void)type;
  if (pthread_mutex_trylock(&admission) != 0)
    return H2_PAL_ERR_BUSY;
  jstring strings[] = {report,   version, host,     session,      ca,    wrong,
                       dns_host, dns_ip,  http_url, fallback_url, offer, stun};
  const char *values[12] = {0};
  int rc = H2_PAL_OK;
  for (unsigned i = 0u; i < 12u; ++i) {
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
  if (rc == H2_PAL_OK && port > 0 && port <= 65535 && mqtt_port > 0 &&
      mqtt_port <= 65535) {
    if (retained_owner) {
      int cleanup = cleanup_owner(retained_owner);
      (void)h2_ipv6_write_report(retained_owner->report_path, "android-emulator",
          &retained_owner->result, retained_owner->rc, cleanup);
      if (cleanup != H2_PAL_OK) {
        rc = cleanup;
        goto done;
      }
      release_owner(retained_owner);
      retained_owner = NULL;
    }
    mobile_owner_t *owner = calloc(1u, sizeof(*owner));
    if (!owner) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto done;
    }
    owner->report_path = malloc(strlen(values[0]) + 1u);
    if (!owner->report_path) {
      free(owner);
      rc = H2_PAL_ERR_NO_MEMORY;
      goto done;
    }
    strcpy(owner->report_path, values[0]);
    rc = h2_android_platform_set_image_version(values[1]);
    if (rc == H2_PAL_OK)
      rc = h2_android_net_create(&owner->net);
    if (rc == H2_PAL_OK)
      rc = h2_android_webrtc_create(&owner->webrtc);
    if (rc == H2_PAL_OK) {
      h2_runtime_config_t config = h2_android_app_host_config();
      config.net = h2_android_net_api(owner->net);
      config.webrtc = h2_android_webrtc_api(owner->webrtc);
      rc = h2_ipv6_mobile_run(config, values[2], (uint16_t)port, values[3],
                              (const uint8_t *)values[4], strlen(values[4]),
                              (const uint8_t *)values[5], strlen(values[5]),
                              values[6], values[7], values[8], values[9],
                              (uint16_t)mqtt_port, values[10], values[11],
                              (uint16_t)dns_port, &owner->result);
    }
    owner->rc = rc;
    int teardown = cleanup_owner(owner);
    int written = h2_ipv6_write_report(owner->report_path, "android-emulator",
                                       &owner->result, rc, teardown);
    if (rc == H2_PAL_OK && teardown != H2_PAL_OK)
      rc = teardown;
    if (written != H2_PAL_OK)
      rc = written;
    if (teardown != H2_PAL_OK)
      retained_owner = owner;
    else
      release_owner(owner);
  } else if (rc == H2_PAL_OK)
    rc = H2_PAL_ERR_INVALID_ARG;
done:
  for (unsigned i = 0u; i < 12u; ++i)
    if (values[i])
      (*env)->ReleaseStringUTFChars(env, strings[i], values[i]);
  pthread_mutex_unlock(&admission);
  return rc;
}
