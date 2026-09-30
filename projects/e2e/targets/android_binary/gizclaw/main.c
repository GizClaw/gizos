#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <stdio.h>

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_gizclaw_MainActivity_nativeRun(JNIEnv *env, jclass type,
    jstring directory, jstring version, jstring endpoint, jstring token,
    jstring api, jstring audio, jbyteArray pcm) {
  (void)type;
  jstring strings[] = {directory, version, endpoint, token, api, audio};
  const char *values[6] = {0};
  int rc = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  for (unsigned i = 0u; i < 6u; ++i) {
    if (!strings[i]) goto done;
    values[i] = (*env)->GetStringUTFChars(env, strings[i], NULL);
    if (!values[i]) goto done;
  }
  if (!pcm || !h2_gizclaw_e2e_fixture_key()[0]) goto done;
  char log_path[1024], report_path[1024];
  if (snprintf(log_path,sizeof(log_path),"%s/gizclaw.log",values[0]) >= (int)sizeof(log_path) ||
      snprintf(report_path,sizeof(report_path),"%s/gizclaw-result.json",values[0]) >= (int)sizeof(report_path)) goto done;
  if (!freopen(log_path,"w",stdout)) goto done;
  setvbuf(stdout,NULL,_IOLBF,0);
  jbyte *bytes = (*env)->GetByteArrayElements(env,pcm,NULL);
  if (!bytes) goto done;
  const jsize length = (*env)->GetArrayLength(env,pcm);
  h2_android_http_t *http = NULL;
  h2_android_webrtc_t *webrtc = NULL;
  h2_gizclaw_e2e_result_t result = {0};
  rc = h2_android_platform_set_image_version(values[1]);
  if (!rc) rc = h2_android_http_create(NULL,0u,&http);
  if (!rc) rc = h2_android_webrtc_create(&webrtc);
  if (!rc) {
    h2_runtime_config_t config = h2_android_app_host_config();
    config.http = h2_android_http_api(http);
    config.webrtc = h2_android_webrtc_api(webrtc);
    rc = h2_gizclaw_mobile_run(config,"android-emulator",values[2],values[3],
        values[4],values[5],(const uint8_t *)bytes,(size_t)length,&result);
  }
  (*env)->ReleaseByteArrayElements(env,pcm,bytes,JNI_ABORT);
  int teardown = H2_PAL_ERR_INVALID_STATE;
  if (!result.retained_resources) {
    teardown = h2_android_webrtc_destroy(&webrtc);
    h2_android_http_destroy(http);
    if (!teardown) teardown = h2_android_platform_core_shutdown();
  }
  rc = h2_gizclaw_mobile_report(report_path,"android-emulator",&result,rc,teardown);
  fflush(stdout);
done:
  for (unsigned i = 0u; i < 6u; ++i)
    if (values[i]) (*env)->ReleaseStringUTFChars(env,strings[i],values[i]);
  return rc;
}
