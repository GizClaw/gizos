#ifndef H2_APP_TEST_AUDIO_FAKE_H
#define H2_APP_TEST_AUDIO_FAKE_H
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2_app_test_fault.h"
#include "h2/pal/os/h2_pal_time.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_AUDIO_FAKE_TRACKS_MAX 4u
/** In-memory Audio PAL, independent of real hardware. mic_read produces one
 * silent frame without waiting. Use testing_audio decorator + a Time PAL for
 * paced PCM fixtures. Playback validates frames and records bytes, not sound.
 * Controls/evidence are serialized as in fault.h. Stop/close failures preserve
 * active ownership for retry. Audio info may be configured before first use. */
typedef struct h2_app_test_audio_fake {
  h2_pal_audio_api_t api;
  const h2_pal_mem_api_t *mem;
  /** Optional borrowed clock configured before use. Successful track writes
   * sleep for their PCM duration (rounded up); NULL consumes without waiting.
   * Use a yielding/real Time PAL with production playback workers. */
  const h2_pal_time_api_t *playback_time;
  void *implementation;
  h2_audio_info_t info;
  bool mic_active, speaker_active;
  uint32_t volume_percent, last_volume_percent, active_tracks;
  uint64_t playback_bytes;
  uint32_t last_timeout_ms;
  h2_app_test_fault_t get_info, start_mic, stop_mic, read_mic;
  h2_app_test_fault_t start_speaker, stop_speaker, set_volume;
  h2_app_test_fault_t create_track, write_track, drain_track, close_track;
} h2_app_test_audio_fake_t;
/** Initialize fresh storage borrowing allocator. Default format is 16 kHz mono
 * S16LE, 320 samples/frame; all four tracks are available.
 * INVALID_ARG/NO_MEMORY on failure. Do not reinitialize a live object. */
h2_pal_result_t h2_app_test_audio_fake_init(h2_app_test_audio_fake_t *audio,
                                            const h2_pal_mem_api_t *mem);
/** Refuse active mic/speaker/tracks with INVALID_STATE; NULL/repeated deinit
 * OK. */
h2_pal_result_t h2_app_test_audio_fake_deinit(h2_app_test_audio_fake_t *audio);

#ifdef __cplusplus
}
#endif
#endif
