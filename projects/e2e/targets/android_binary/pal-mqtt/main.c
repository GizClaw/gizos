#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <string.h>
JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palmqtt_MainActivity_nativeRun(JNIEnv *env, jclass type,
    jstring report, jstring version, jstring host, jint tcp_port, jint tls_port,
    jstring session, jstring ca, jstring wrong_ca) {
    (void)type;
    jstring strings[] = {report, version, host, session, ca, wrong_ca};
    const char *values[6] = {0};
    int rc = H2_PAL_OK;
    h2_mqtt_mobile_result_t result = {0};
    for (unsigned index = 0u; index < 6u; ++index) {
        if (strings[index] == NULL) { rc = H2_PAL_ERR_INVALID_ARG; break; }
        values[index] = (*env)->GetStringUTFChars(env, strings[index], NULL);
        if (values[index] == NULL) { rc = H2_PAL_ERR_NO_MEMORY; break; }
    }
    if (rc == H2_PAL_OK) {
        rc = h2_android_platform_set_image_version(values[1]);
        if (tcp_port <= 0 || tcp_port > UINT16_MAX || tls_port <= 0 || tls_port > UINT16_MAX) rc = H2_PAL_ERR_INVALID_ARG;
        if (rc == H2_PAL_OK) rc = h2_mqtt_mobile_run(h2_android_app_host_config(), values[2],
            (uint16_t)tcp_port, (uint16_t)tls_port, values[3], (const uint8_t *)values[4], strlen(values[4]),
            (const uint8_t *)values[5], strlen(values[5]), &result);
        int teardown = h2_android_platform_core_shutdown();
        rc = h2_mqtt_mobile_report(values[0], "android-emulator", values[1], &result, rc, teardown);
    }
    for (unsigned index = 0u; index < 6u; ++index)
        if (values[index] != NULL) (*env)->ReleaseStringUTFChars(env, strings[index], values[index]);
    return rc;
}
