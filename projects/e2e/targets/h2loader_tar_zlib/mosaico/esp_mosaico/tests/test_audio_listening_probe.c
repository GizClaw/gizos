#include <assert.h>
#include "h2_runtime.h"
#define H2_MOSAICO_LAUNCHER_HOST_TEST 1
#define H2_MOSAICO_SUITE_AUDIO 1
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ms) { assert(ms == 5000); }
#include "projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico/tests/launcher_under_test.h"

static struct {
  unsigned channels, rate, volume, writes, closes, stops;
  int active, borrowed, start_error, write_error, drain_error, close_errors;
} fake;
static const h2_pal_audio_api_t api;
static h2_pal_audio_track_t fake_track;
static int info(void* user, h2_audio_info_t* out) {
  (void)user;
  *out = (h2_audio_info_t){.playback_supported = 1,
                           .playback_format = {fake.rate, 512, fake.channels,
                                               H2_AUDIO_SAMPLE_S16LE}};
  return 0;
}
static int get_volume(void* user, uint32_t* out) {
  (void)user;
  *out = fake.volume;
  return 0;
}
static int set_volume(void* user, uint32_t value) {
  (void)user;
  fake.volume = value;
  return 0;
}
static int start(void* user) {
  (void)user;
  if (fake.start_error) return fake.start_error;
  fake.active = 1;
  return 0;
}
static int stop(void* user) {
  (void)user;
  assert(!fake.borrowed);
  ++fake.stops;
  fake.active = 0;
  return 0;
}
static int write_frame(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout) {
  assert(track == &fake_track && fake.active && fake.borrowed);
  assert(timeout == 1000 && fake.volume == 75);
  assert(frame->sample_rate_hz == 16000 && frame->samples_per_channel == 512);
  assert(frame->channels == fake.channels &&
         frame->bytes == 1024 * fake.channels);
  const int16_t* pcm = frame->data;
  int peak = 0;
  for (unsigned i = 0; i < 512; ++i) {
    int sample = pcm[i * fake.channels];
    if (sample < 0) sample = -sample;
    if (sample > peak) peak = sample;
    if (i >= 32)
      assert(pcm[i * fake.channels] == pcm[(i - 32) * fake.channels]);
    if (fake.channels == 2) assert(pcm[2 * i] == pcm[2 * i + 1]);
  }
  assert(peak == 3840);
  ++fake.writes;
  return fake.write_error && fake.writes == 3 ? fake.write_error : 0;
}
static int drain(h2_pal_audio_track_t* track, uint32_t timeout) {
  assert(track == &fake_track && timeout == 3000);
  return fake.drain_error;
}
static int close_track(h2_pal_audio_track_t* track) {
  assert(track == &fake_track && fake.borrowed);
  ++fake.closes;
  if (fake.close_errors) {
    --fake.close_errors;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  fake.borrowed = 0;
  return 0;
}
static int create(void* user, const h2_audio_track_config_t* config,
                  h2_pal_audio_track_t** out) {
  (void)user;
  assert(fake.active && !fake.borrowed && config->volume_factor_milli == 1000);
  assert(config->buffer_frames == 4);
  fake_track = (h2_pal_audio_track_t){.audio = &api,
                                      .write = write_frame,
                                      .drain = drain,
                                      .close = close_track};
  fake.borrowed = 1;
  *out = &fake_track;
  return 0;
}
static const h2_pal_audio_vtable_t ops = {
    .get_info = info,
    .get_speaker_volume_percent = get_volume,
    .set_speaker_volume_percent = set_volume,
    .start_speaker = start,
    .stop_speaker = stop,
    .create_track = create};
static const h2_pal_audio_api_t api = {.vtable = &ops};
static h2_runtime_t fixture;
static void prepare(void) {
  assert(!fake.borrowed && !fake.active);
  memset(&fake, 0, sizeof(fake));
  fake.channels = 1;
  fake.rate = 16000;
  fake.volume = 42;
  fixture.audio = &api;
  runtime = &fixture;
}
int main(void) {
  (void)audio_report;
  prepare();
  assert(audio_listening_probe() == 0);
  assert(fake.writes == 64 && fake.closes == 1 && fake.stops == 1 &&
         fake.volume == 42);
  prepare();
  fake.channels = 2;
  fake.close_errors = 2;
  assert(audio_listening_probe() == 0);
  assert(fake.writes == 64 && fake.closes == 3 && fake.stops == 1 &&
         fake.volume == 42);
  prepare();
  fake.write_error = H2_PAL_ERR_TIMEOUT;
  assert(audio_listening_probe() == H2_PAL_ERR_TIMEOUT);
  assert(fake.writes == 3 && fake.closes == 1 && fake.stops == 1 &&
         fake.volume == 42);
  prepare();
  fake.drain_error = H2_PAL_ERR_TIMEOUT;
  assert(audio_listening_probe() == H2_PAL_ERR_TIMEOUT);
  assert(fake.closes == 1 && fake.stops == 1 && fake.volume == 42);
  prepare();
  fake.rate = 48000;
  assert(audio_listening_probe() == H2_PAL_ERR_UNSUPPORTED && !fake.writes &&
         !fake.active);
  prepare();
  fake.start_error = H2_PAL_ERR_IO;
  assert(audio_listening_probe() == H2_PAL_ERR_IO && !fake.closes &&
         !fake.stops && fake.volume == 42);
  prepare();
  fake.close_errors = 3;
  assert(audio_listening_probe() == H2_PAL_ERR_WOULD_BLOCK);
  assert(fake.borrowed && fake.closes == 3 && !fake.stops && fake.volume == 42);
  assert(close_track(&fake_track) == 0 && stop(NULL) == 0);
  return 0;
}
