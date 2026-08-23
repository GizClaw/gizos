#ifndef H2_BK_AUDIO_DECODER_H
#define H2_BK_AUDIO_DECODER_H

#include "h2/pal/hal/h2_pal_audio_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the BK Helix AAC-LC Audio Decoder PAL provider. */
const h2_pal_audio_decoder_api_t *h2_bk_audio_decoder_api(void);

#ifdef __cplusplus
}
#endif

#endif
