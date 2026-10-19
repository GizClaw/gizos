#ifndef H2_PAL_AUDIO_MOBILE_RUNNER_H
#define H2_PAL_AUDIO_MOBILE_RUNNER_H

#include "h2_pal_audio_e2e.h"

int h2_audio_mobile_run(const h2_pal_audio_api_t *audio,
                        const h2_pal_time_api_t *time,
                        h2_pal_audio_e2e_result_t *result);
int h2_audio_mobile_report(const char *path, const char *platform,
                           const char *version,
                           const h2_pal_audio_e2e_result_t *result,
                           int run_rc, int teardown_rc);
#endif
