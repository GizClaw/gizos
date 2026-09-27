#ifndef H2_FFMPEG_RECORDING_H
#define H2_FFMPEG_RECORDING_H

#include "h2_media_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_ffmpeg_recording h2_ffmpeg_recording_t;

/** MP4 recorder configuration. path is borrowed only during create; the clock is
 * borrowed through destroy. Supports even dimensions <=4096, 30 fps video and
 * 16 kHz mono S16 PCM (the current Desktop speaker output contract).
 * Existing files are rejected. The parent directory must already exist.
 */
typedef struct h2_ffmpeg_recording_config {
  const char *path;
  const h2_pal_time_api_t *time;
  uint32_t width;
  uint32_t height;
} h2_ffmpeg_recording_config_t;

typedef struct h2_ffmpeg_recording_stats {
  uint64_t duration_us;
  uint64_t video_frames;
  uint64_t captured_video_frames;
  uint64_t captured_audio_frames;
  uint64_t late_audio_frames;
  size_t buffer_bytes;
  h2_pal_result_t result;
} h2_ffmpeg_recording_stats_t;

/** Create an owned streaming MPEG-4 Visual/AAC MP4 encoder. Starts a worker;
 * callbacks only copy to preallocated storage (16 video snapshots, 2 seconds
 * PCM). Capacity exhaustion, late PCM or encoder/write failure is latched and
 * returned at finish, never silently treated as success. No microphone input.
 * Before the first video presentation output is black; gaps in speaker output
 * are silence. Video is sampled at 30 fps, holding the last presented frame.
 */
h2_pal_result_t
h2_ffmpeg_recording_create(const h2_ffmpeg_recording_config_t *config,
                           h2_ffmpeg_recording_t **out_recording);

/** Borrowed concurrent sink, valid through destroy. Unregister all sources
 * before finish/destroy. Its epoch is create's injected monotonic timestamp.
 */
const h2_media_capture_api_t *
h2_ffmpeg_recording_capture(h2_ffmpeg_recording_t *recording);

/** Blocking, idempotent finalization. Stops worker, encodes through the last
 * accepted speaker sample (including output latency), holds the tail image,
 * pads the final AAC block, flushes both encoders, writes trailer and closes.
 * A final presentation between 30 fps boundaries adds one last video sample.
 * No captured presentation returns UNAVAILABLE instead of a successful black
 * clip. Graceful cancellation uses the same operation and keeps a playable
 * file. Calling concurrently with finish/destroy is unsupported. Failure still
 * attempts to finalize accepted media; the returned error must be reported.
 */
h2_pal_result_t
h2_ffmpeg_recording_finish(h2_ffmpeg_recording_t *recording,
                           h2_ffmpeg_recording_stats_t *out_stats);

/** Finish if needed and release; NULL is allowed. Use finish to observe errors.
 */
void h2_ffmpeg_recording_destroy(h2_ffmpeg_recording_t *recording);

#ifdef __cplusplus
}
#endif
#endif
