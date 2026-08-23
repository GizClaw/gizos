#ifndef H2_TINYH264_H
#define H2_TINYH264_H

#include "h2/pal/hal/h2_pal_video_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the portable TinyH264 Video Decoder PAL provider.
 *
 * The provider accepts H.264 Baseline/Constrained Baseline Annex-B access
 * units and returns allocator-backed YUV420P or RGB565 frames.
 */
const h2_pal_video_decoder_api_t *h2_tinyh264_video_decoder_api(void);

#ifdef __cplusplus
}
#endif

#endif
