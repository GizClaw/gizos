#ifndef H2_GIZCLAW_MOBILE_RUNNER_H
#define H2_GIZCLAW_MOBILE_RUNNER_H
#include "h2_gizclaw_e2e.h"
/* Copies fixture strings/PCM. Retained results keep that copy and Runtime
 * alive; the launcher must also retain its PAL owners until process exit. */
int h2_gizclaw_mobile_run(h2_runtime_config_t runtime_config,
    const char *platform, const char *endpoint, const char *token,
    const char *api_url, const char *audio_url, const uint8_t *pcm, size_t pcm_len,
    h2_gizclaw_e2e_result_t *result);
int h2_gizclaw_mobile_report(const char *path, const char *platform,
    const h2_gizclaw_e2e_result_t *result, int rc, int teardown);
#endif
