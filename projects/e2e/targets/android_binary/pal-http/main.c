#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
#include <string.h>

JNIEXPORT jint JNICALL
Java_com_haivivi_gizos_e2e_palhttp_MainActivity_nativeRun(JNIEnv *env, jclass type,
    jstring report, jstring version, jstring http, jstring https, jstring untrusted, jstring ca) {
    (void)type;
    jstring strings[] = {report, version, http, https, untrusted, ca};
    const char *values[6] = {0};
    int rc = H2_PAL_OK;
    for (unsigned index = 0u; index < 6u; ++index) {
        values[index] = (*env)->GetStringUTFChars(env, strings[index], NULL);
        if (values[index] == NULL) { rc = H2_PAL_ERR_NO_MEMORY; break; }
    }
    if (rc == H2_PAL_OK) {
        h2_android_http_t *owner = NULL;
        h2_pal_http_e2e_result_t result = {0};
        rc = h2_android_platform_set_image_version(values[1]);
        if (rc == H2_PAL_OK)
            rc = h2_android_http_create((const uint8_t *)values[5], strlen(values[5]), &owner);
        if (rc == H2_PAL_OK) {
            h2_runtime_config_t config = h2_android_app_host_config();
            config.http = h2_android_http_api(owner);
            rc = h2_http_mobile_run(config, values[2], values[3], values[4], &result);
        }
        h2_android_http_destroy(owner);
        int teardown = h2_android_platform_core_shutdown();
        rc = h2_http_mobile_report(values[0], "android-emulator", values[1], &result, rc, teardown);
    }
    for (unsigned index = 0u; index < 6u; ++index)
        if (values[index] != NULL) (*env)->ReleaseStringUTFChars(env, strings[index], values[index]);
    return rc;
}
