#ifndef H2_SMOKE_MP4_PLAYER_H
#define H2_SMOKE_MP4_PLAYER_H

#include "h2_mp4_decoder.h"
#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How decoded video is mapped onto the physical display. */
typedef enum h2_smoke_mp4_player_display_mode {
    /** Require the decoded video and display dimensions to match exactly. */
    H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT = 0,
    /** Draw at native size, centered on a black physical display. */
    H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER = 1,
} h2_smoke_mp4_player_display_mode_t;

/** @brief Immutable configuration borrowed for the blocking player call. */
typedef struct h2_smoke_mp4_player_config {
    /**
     * Caller-owned random-access MP4 source, valid for the blocking call.
     * Leave read_at NULL to open media_path through Runtime FS instead.
     */
    h2_mp4_decoder_source_api_t source;
    /** Optional Runtime FS path used when source.read_at is NULL. */
    const char *media_path;
    /** Maximum decoder acquire wait in milliseconds. */
    uint32_t acquire_timeout_ms;
    /** Frames to present before returning, or zero to run until EOS or stop. */
    size_t max_frames;
    /** Nonzero restarts the video track at end-of-stream. */
    int looping;
    /** Video-to-display mapping. Zero selects exact-size presentation. */
    h2_smoke_mp4_player_display_mode_t display_mode;
    /**
     * Nonzero requires decoded audio and Audio PAL playback.
     * Leave zero for video-only consumers that can ignore an MP4 audio track.
     */
    int require_audio;
    /**
     * Optional cooperative stop callback polled on the blocking caller thread
     * while it waits for and paces video presentation.
     */
    int (*should_stop)(void *user);
    /** Borrowed callback context valid until the blocking call returns. */
    void *stop_user;
    /**
     * Optional callback invoked on the blocking caller thread once after the
     * first frame is presented.
     * Returning an error stops playback and returns that error.
     */
    h2_pal_result_t (*on_ready)(void *user);
    /** Borrowed ready callback context valid until the blocking call returns. */
    void *ready_user;
} h2_smoke_mp4_player_config_t;

/**
 * @brief Decode, pace, draw, present, and release frames using Runtime PAL APIs only.
 * @param runtime Borrowed initialized Runtime with mem, time, task, queue,
 * Video Decoder, and Display PAL capabilities. Audio playback additionally
 * requires Audio Decoder and Audio PAL. The display remains launcher-owned.
 * @param config Borrowed immutable configuration valid until this blocking call returns.
 * @return H2_PAL_OK on bounded completion, EOS, or cooperative stop; otherwise the PAL failure.
 */
h2_pal_result_t h2_smoke_mp4_player_run(
    h2_runtime_t *runtime,
    const h2_smoke_mp4_player_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
