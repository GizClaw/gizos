#ifndef H2_PAL_WEBRTC_DEVICE_H
#define H2_PAL_WEBRTC_DEVICE_H
#include "h2_pal_webrtc_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Registered device runner identity; launchers supply a 64 KiB stack. */
extern const char h2_pal_webrtc_device_runner_task_name[];
/**
 * Borrow an initialized Board Runtime, connect with its saved STA settings,
 * use the Board WebRTC provider and isolated Pion signaling, and run every
 * case. Only public test fixture inputs come from build definitions;
 * credentials stay in the existing board settings. No reboot, erase or install
 * occurs here.
 */
int h2_pal_webrtc_device_run(h2_runtime_t *runtime,
                             h2_pal_webrtc_e2e_result_t *result);
/** Replay an immutable result; never reruns network requests. */
void h2_pal_webrtc_device_report(const h2_runtime_t *runtime,
                                 const h2_pal_webrtc_e2e_result_t *result);
#ifdef __cplusplus
}
#endif
#endif
