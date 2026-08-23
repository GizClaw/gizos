#ifndef H2_LINUX_ALSA_AUDIO_H
#define H2_LINUX_ALSA_AUDIO_H

#include "h2/pal/hal/h2_pal_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_linux_alsa_audio_config {
    const char *device;
    h2_audio_pcm_format_t playback_format;
} h2_linux_alsa_audio_config_t;

/** Configure the process-wide ALSA playback provider before Runtime init. */
h2_pal_result_t h2_linux_alsa_audio_configure(
    const h2_linux_alsa_audio_config_t *config);

/** Return the process-wide Linux ALSA Audio PAL provider. */
h2_pal_audio_t *h2_linux_alsa_audio_api(void);

#ifdef __cplusplus
}
#endif

#endif
