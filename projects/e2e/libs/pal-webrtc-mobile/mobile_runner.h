#ifndef H2_WEBRTC_MOBILE_RUNNER_H
#define H2_WEBRTC_MOBILE_RUNNER_H
#include "h2_pal_webrtc_e2e.h"
/* Borrow config/fixture inputs, destroy Runtime before returning to the owner.
 */
int h2_webrtc_mobile_run(h2_runtime_config_t config, const char *offer_url,
                         const char *stun_url, h2_pal_webrtc_e2e_result_t *out);
/* Save a bounded report after the launcher retires WEBRTC and Core owners. */
int h2_webrtc_mobile_report(const char *path, const char *platform,
                            const char *version,
                            const h2_pal_webrtc_e2e_result_t *result, int rc,
                            int teardown);
#endif
