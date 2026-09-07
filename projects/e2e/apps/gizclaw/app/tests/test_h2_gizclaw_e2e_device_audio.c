#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "h2_gizclaw_e2e_catalog.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_time_t clock;
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_time_init(&clock, 100u);
  h2_runtime_t runtime = {0};
  h2_gizclaw_e2e_config_t config = {0};
  h2_gizclaw_e2e_fixture_t fixture = {.runtime = &runtime,
                                      .config = &config,
                                      .allocator = &mem.api,
                                      .time = &clock.api};
  assert(h2_gizclaw_e2e_prepare_device(&fixture) == H2_PAL_OK);
  assert(fixture.device_audio && fixture.device_audio_wrapper &&
         fixture.device_cleanup);
  assert(h2_pal_audio_start_speaker(fixture.device_audio) == H2_PAL_OK);
  uint8_t pcm[640] = {0xff, 0x7f, 0x00, 0x80};
  h2_audio_track_config_t track_config = {
      .format = {16000, 320, 1, H2_AUDIO_SAMPLE_S16LE}};
  h2_pal_audio_track_t *track = NULL;
  assert(h2_pal_audio_create_track(fixture.device_audio, &track_config,
                                   &track) == H2_PAL_OK);
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(pcm, sizeof(pcm), track_config.format);
  frame.bytes = sizeof(pcm);
  assert(h2_pal_audio_track_write(track, &frame, 50) == H2_PAL_OK);
  assert(clock.monotonic_ms == 120);
  h2_app_test_audio_evidence_t evidence;
  assert(h2_app_test_audio_copy_evidence(fixture.device_audio_wrapper,
                                         &evidence) == H2_PAL_OK);
  assert(evidence.playback_bytes == 640 && evidence.playback_peak == 32768);
  assert(evidence.active_track_count == 1);
  assert(h2_pal_audio_set_speaker_volume_percent(fixture.device_audio, 37) ==
         H2_PAL_OK);
  uint32_t volume = 0;
  assert(h2_pal_audio_get_speaker_volume_percent(fixture.device_audio,
                                                 &volume) == H2_PAL_OK);
  assert(volume == 37);
  const size_t retained_blocks = mem.live_blocks;
  fixture.device_audio_fake.close_track =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_BUSY, .remaining = 1};
  assert(h2_pal_audio_track_close(track) == H2_PAL_ERR_BUSY);
  assert(fixture.device_cleanup(&fixture) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.device_audio && fixture.device_audio_wrapper &&
         mem.live_blocks == retained_blocks);
  assert(h2_pal_audio_track_close(track) == H2_PAL_OK);
  assert(fixture.device_cleanup(&fixture) == H2_PAL_OK);
  assert(!fixture.device_audio && !fixture.device_cleanup &&
         mem.live_blocks == 0);
  for (unsigned fail = 1; fail <= 2; ++fail) {
    mem.calls = 0;
    mem.fail_at = fail;
    assert(h2_gizclaw_e2e_prepare_device(&fixture) == H2_PAL_ERR_NO_MEMORY);
    assert(fixture.device_cleanup(&fixture) == H2_PAL_OK);
    assert(!mem.live_blocks);
  }
  return 0;
}
