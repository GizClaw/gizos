#ifndef H2_PAL_WEBRTC_DEVICE_H
#define H2_PAL_WEBRTC_DEVICE_H
#include "h2_pal_webrtc_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Registered device runner identity; launchers supply a 64 KiB stack. */
extern const char h2_pal_webrtc_device_runner_task_name[];
/**
 * Borrow an initialized Board Runtime and reuse an active IPv4 or non-link-local
 * IPv6 STA connection, falling back to its saved STA settings when needed.
 * Use the Board WebRTC provider and isolated Pion signaling to run every case.
 * A network prerequisite error leaves the result empty so the launcher can wait
 * for provisioning; a nonempty result must never be rerun to obtain a pass.
 * Only public test fixture inputs come from build definitions. Saved credentials
 * are never changed. No reboot, erase or install occurs here.
 */
int h2_pal_webrtc_device_run(h2_runtime_t *runtime,
                             uint32_t connection_timeout_ms, uint32_t soak_duration_ms,
                             h2_pal_webrtc_e2e_result_t *result);
/** Replay an immutable result; never reruns network requests. */
void h2_pal_webrtc_device_report(const h2_runtime_t *runtime,
                                 const h2_pal_webrtc_e2e_result_t *result);
#ifdef __cplusplus
}
#endif
#endif
