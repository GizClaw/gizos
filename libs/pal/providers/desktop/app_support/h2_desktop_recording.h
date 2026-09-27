#ifndef H2_DESKTOP_RECORDING_H
#define H2_DESKTOP_RECORDING_H

#include "h2_ffmpeg_recording.h"
#include "h2_portaudio.h"
#include "h2_sdl3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_recording h2_desktop_recording_t;

/** Start capture on the actual display/speaker providers. Borrows both and
 * the clock until stop/destroy. One recorder per source; already registered
 * sources return BUSY. The MP4 path must not exist. No environment/CLI policy.
 * Call from the launcher's control/main thread after opening the display.
 */
h2_pal_result_t
h2_desktop_recording_start(h2_sdl3_t *display, h2_portaudio_t *audio,
                           const h2_pal_time_api_t *time, const char *path,
                           h2_desktop_recording_t **out_recording);

/** Synchronously detach both sources, finish accepted video/audio including
 * queued DAC tail, flush encoders and close MP4. Idempotent. Use for normal
 * completion and cooperative cancel. Stop only after app/audio producers
 * have finished; it captures accepted speaker writes, not unplayed tracks.
 * Does not stop/destroy the borrowed providers. Errors remain visible.
 */
h2_pal_result_t
h2_desktop_recording_stop(h2_desktop_recording_t *recording,
                          h2_ffmpeg_recording_stats_t *out_stats);

/** Stop if needed and release. NULL allowed. Call stop first to observe errors.
 */
void h2_desktop_recording_destroy(h2_desktop_recording_t *recording);

#ifdef __cplusplus
}
#endif
#endif
