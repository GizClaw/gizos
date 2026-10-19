#ifndef H2_DESKTOP_RECORDING_H
#define H2_DESKTOP_RECORDING_H

#include "h2_desktop_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_recording h2_desktop_recording_t;

/** Desktop-only recording statistics. buffer_bytes counts capture and output storage,
 * excluding bounded FFmpeg codec/scaler/fragment working space. */
typedef struct h2_desktop_recording_stats {
  uint64_t duration_us;
  uint64_t video_frames;
  uint64_t captured_video_frames;
  uint64_t captured_audio_frames;
  uint64_t late_audio_frames;
  size_t buffer_bytes;
  h2_pal_result_t result;
} h2_desktop_recording_stats_t;

/** Create a Desktop-only streaming MPEG-4 Visual/AAC MP4 consumer.
 * Dimensions must be even, nonzero and <=4096. The path is borrowed for this
 * call only; its parent must exist and the file must not exist. Creation starts
 * the encoder and file writer but does not open devices or register source hooks.
 * Uses native steady-clock time. Buffers are preallocated (64 RGB565 snapshots
 * and 2 seconds of timestamped PCM payload); gaps consume no PCM queue storage.
 * A separate writer drains a fixed 4 MiB encoded-byte queue, so transient file
 * write stalls do not stop encoding or grow memory with recording duration.
 * Presentations within one 30 fps output boundary share a queue slot.
 * Overflow, late PCM and encoder/I/O errors are latched and end encoding at the
 * first error. Accepted media is finalized while the sink remains writable;
 * failures cannot extend the recording into a frozen tail.
 * LVGL alpha is already composited into display pixels. Speaker gaps are
 * silent; video holds the last frame. Per-GOP MP4 fragments bound muxer
 * metadata.
 */
h2_pal_result_t
h2_desktop_recording_create(const char *path, uint32_t width, uint32_t height,
                            h2_desktop_recording_t **out_recording);

/** Borrow the consumer's callback configuration until destroy. Display and
 * speaker callbacks copy to bounded queues; on_mic is NULL because the MP4
 * audio track is speaker playback, not microphone input. Callers register these
 * hooks through Desktop capture configuration. NULL input returns NULL.
 */
const h2_desktop_capture_hooks_t *
h2_desktop_recording_hooks(h2_desktop_recording_t *recording);

/** Read the current latched result without stopping; safe during capture.
 * OK means no error has been observed yet, not that finalization has succeeded.
 * Poll from the control task to stop producers promptly on failure. The handle
 * must remain alive; do not race this call with destroy. NULL is INVALID_ARG.
 */
h2_pal_result_t
h2_desktop_recording_result(h2_desktop_recording_t *recording);

/** Finish accepted media, including the final frame and speaker tail, flush
 * codecs, drain pending file writes, and close MP4. Idempotent and blocking. Unregister capture first;
 * no callback may run concurrently with finish/destroy. Cooperative cancel
 * uses the same operation. No captured display returns UNAVAILABLE. Errors
 * remain visible independently of application/test results.
 */
h2_pal_result_t
h2_desktop_recording_stop(h2_desktop_recording_t *recording,
                          h2_desktop_recording_stats_t *out_stats);

/** Stop if needed and release. NULL allowed. Call stop first to observe errors.
 */
void h2_desktop_recording_destroy(h2_desktop_recording_t *recording);

#ifdef __cplusplus
}
#endif
#endif
