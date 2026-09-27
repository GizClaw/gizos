#ifndef H2_DESKTOP_RECORDING_H
#define H2_DESKTOP_RECORDING_H

#include "h2_portaudio.h"
#include "h2_sdl3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_recording h2_desktop_recording_t;

/** Desktop-only recording statistics. buffer_bytes counts capture storage,
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

/** Desktop-only, streaming MPEG-4 Visual/AAC MP4 (30 fps, 16 kHz mono).
 * Start capture on the actual display/speaker providers. Borrows both
 * until stop/destroy. One recorder per source; already registered
 * sources return BUSY. The MP4 path must not exist. No environment/CLI policy.
 * Uses the same native steady clock for display, speaker and encoder.
 * Buffers are preallocated (16 RGB565 snapshots and 2 seconds PCM); callback
 * overflow, late PCM and encoder/I/O failures are latched and returned at stop.
 * LVGL alpha is already composited into the opaque display output. Gaps in
 * speaker output are silence; video holds the last presentation. MP4 fragments
 * once per GOP to bound muxer metadata as well as capture memory.
 * Call from the launcher's control/main thread after opening the display.
 */
h2_pal_result_t
h2_desktop_recording_start(h2_sdl3_t *display, h2_portaudio_t *audio,
                           const char *path,
                           h2_desktop_recording_t **out_recording);

/** Synchronously detach both sources, finish accepted video/audio including
 * queued DAC tail, flush encoders and close MP4. Idempotent. Use for normal
 * completion and cooperative cancel. Stop only after app/audio producers
 * have finished; it captures accepted speaker writes, not unplayed tracks.
 * Does not stop/destroy the borrowed providers. Errors remain visible.
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
