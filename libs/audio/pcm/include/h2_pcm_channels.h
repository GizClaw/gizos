#ifndef H2_PCM_CHANNELS_H
#define H2_PCM_CHANNELS_H

#include "hal/h2_pal_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Extracts or averages selected interleaved S16LE lanes into mono PCM.
 *
 * Borrows frames, buffers and channel_indices for this synchronous, nonblocking
 * call; no allocation or platform operations occur. The caller serializes
 * access to buffers and output metadata. Buffers must not overlap, except that
 * output may start at the same address as input for in-place compaction.
 *
 * channel_count is nonzero; each index must be below raw_frame->channels.
 * Repeated indices are allowed and weight that lane repeatedly. Sums use
 * widened arithmetic and division rounds toward zero. A single index extracts
 * that lane. Input and output must use S16LE and equal sample rates; output
 * must be mono. Unaligned buffers are supported and byte order is explicitly
 * little endian.
 *
 * Processes the smaller of complete input frames and complete output capacity;
 * trailing partial frames are ignored. On success, updates only output bytes
 * and samples_per_channel. Returns INVALID_ARG for NULL frame/indices, input
 * bytes exceeding capacity, zero complete frames, or a result count above
 * UINT16_MAX. Returns UNSUPPORTED for invalid formats, data pointers, channel
 * selections or sample-rate mismatch. Failure leaves output metadata and PCM
 * unchanged.
 */
int h2_pcm_s16_select_mono(const h2_audio_frame_t* raw_frame,
                           h2_audio_frame_t* out_frame,
                           const uint8_t* channel_indices,
                           uint8_t channel_count);

#ifdef __cplusplus
}
#endif

#endif
