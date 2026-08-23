#ifndef H2_LINUX_FDK_AAC_DECODER_H
#define H2_LINUX_FDK_AAC_DECODER_H

#include "h2/pal/hal/h2_pal_audio_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the process-wide FDK AAC-LC Audio Decoder PAL provider. */
const h2_pal_audio_decoder_api_t *h2_linux_fdk_aac_decoder_api(void);

#ifdef __cplusplus
}
#endif

#endif
