#ifndef H2_ADEC_MOBILE_RUNNER_H
#define H2_ADEC_MOBILE_RUNNER_H
#include "h2_runtime.h"
int h2_adec_mobile_run(h2_runtime_config_t config,
    const h2_pal_audio_decoder_api_t *decoder, const char *platform,
    const char *version, const char *path, int (*shutdown)(void));
#endif
