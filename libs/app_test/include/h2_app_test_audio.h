#ifndef H2_APP_TEST_AUDIO_H
#define H2_APP_TEST_AUDIO_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_APP_TEST_AUDIO_SCRATCH_MAX 8192u
#define H2_APP_TEST_AUDIO_TRACKS_MAX 4u

/** Observation counters saturate at UINT32_MAX except playback_digest, which
 * wraps modulo 2^32 (sum of per-write FNV-1a hashes). Counts describe
 * successful operations. Capture health and fixture progress reset on
 * each mic start attempt after the clock is read. Other counters accumulate
 * until destroy. No captured PCM is retained. */
typedef struct h2_app_test_audio_evidence {
  uint64_t mic_start_count;
  uint64_t mic_read_count;
  uint64_t fixture_bytes_emitted;
  bool fixture_complete;
  uint64_t real_capture_frames;
  uint64_t real_capture_no_frame;
  int real_capture_first_error;
  int real_capture_last_error;
  uint64_t mic_stop_count;
  uint64_t speaker_start_count;
  uint64_t speaker_stop_count;
  uint64_t track_create_count;
  uint64_t track_write_count;
  uint64_t track_drain_count;
  uint64_t track_close_count;
  uint64_t playback_bytes;
  uint64_t playback_digest;
  bool mic_active;
  bool speaker_active;
  uint32_t active_track_count;
} h2_app_test_audio_evidence_t;

typedef struct h2_app_test_audio h2_app_test_audio_t;

/** Immutable, borrowed interleaved S16LE PCM. Size must contain whole samples
 * across all channels. Format must match the delegate microphone format. */
typedef struct h2_app_test_audio_fixture {
  const uint8_t *pcm;
  size_t size;
  h2_audio_pcm_format_t format;
} h2_app_test_audio_fixture_t;

/** Create an Audio PAL decorator. The delegate may be a real or fake PAL.
 * All dependencies and PCM remain borrowed until destroy or fixture
 * replacement. Mic lifecycle delegates; each read drains real capture with zero
 * timeout into bounded scratch, clears it, and supplies fixture PCM paced by
 * monotonic time. Real capture errors are evidence, not fixture-read failures.
 * At EOF, frames continue as silence. Speaker and track operations delegate
 * unchanged. Caller serializes mic start/read/stop and speaker lifecycle; each
 * track is serialized independently. Evidence may be read concurrently, but is
 * not an atomic multi-field snapshot. Destroy requires all callers to be
 * quiescent. Returns INVALID_ARG for invalid config or NO_MEMORY; failure
 * clears out_audio.
 */
h2_pal_result_t
h2_app_test_audio_create(const h2_pal_mem_api_t *mem,
                         const h2_pal_time_api_t *time,
                         const h2_pal_audio_api_t *delegate,
                         const h2_app_test_audio_fixture_t *fixture,
                         h2_app_test_audio_t **out_audio);

/** Borrow the decorated PAL until destroy. NULL input returns NULL. */
const h2_pal_audio_api_t *h2_app_test_audio_api(h2_app_test_audio_t *audio);

/** Replace the borrowed fixture while mic is stopped; otherwise INVALID_STATE.
 * The next start rewinds and establishes a new pacing epoch. */
h2_pal_result_t
h2_app_test_audio_set_fixture(h2_app_test_audio_t *audio,
                              const h2_app_test_audio_fixture_t *fixture);

/** Copy bounded observations, without resetting counters; INVALID_ARG on NULL.
 */
h2_pal_result_t
h2_app_test_audio_copy_evidence(const h2_app_test_audio_t *audio,
                                h2_app_test_audio_evidence_t *out_evidence);

/** Free wrapper only, never the delegate. NULL is OK. Active mic, speaker or
 * tracks return INVALID_STATE and retain ownership. Failed stop/close may be
 * retried; after successful destroy the pointer must no longer be used. */
h2_pal_result_t h2_app_test_audio_destroy(h2_app_test_audio_t *audio);

#ifdef __cplusplus
}
#endif
#endif
