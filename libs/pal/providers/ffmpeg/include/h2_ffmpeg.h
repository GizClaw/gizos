#ifndef H2_FFMPEG_H
#define H2_FFMPEG_H

#include "h2/pal/hal/h2_pal_audio_decoder.h"
#include "h2/pal/hal/h2_pal_video_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the FFmpeg-backed AAC audio decoder provider. */
const h2_pal_audio_decoder_api_t *h2_ffmpeg_audio_decoder_api(void);

/** Return the FFmpeg-backed H.264 video decoder provider. */
const h2_pal_video_decoder_api_t *h2_ffmpeg_video_decoder_api(void);

#ifdef __cplusplus
}
#endif

#endif
