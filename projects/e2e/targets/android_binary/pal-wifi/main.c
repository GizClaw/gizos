#include "h2_android_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#include <jni.h>
JNIEXPORT jint JNICALL Java_com_haivivi_gizos_e2e_palwifi_MainActivity_nativeRun(JNIEnv *env,
                                                                                 jclass type,
                                                                                 jstring report,
                                                                                 jstring version) {
    (void)type;
    const char *path = (*env)->GetStringUTFChars(env, report, NULL);
    if (!path)
        return H2_PAL_ERR_NO_MEMORY;
    const char *image = (*env)->GetStringUTFChars(env, version, NULL);
    if (!image) {
        (*env)->ReleaseStringUTFChars(env, report, path);
        return H2_PAL_ERR_NO_MEMORY;
    }
    int rc = h2_android_platform_set_image_version(image);
    if (!rc)
        rc = h2_wifi_mobile_run(h2_android_app_host_config(), "android-emulator", image, path,
                                h2_android_platform_core_shutdown);
    (*env)->ReleaseStringUTFChars(env, version, image);
    (*env)->ReleaseStringUTFChars(env, report, path);
    return rc;
}
