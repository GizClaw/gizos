#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audible.h"
#include <assert.h>
#include <string.h>

typedef struct {
  h2_pal_audio_track_t track;
  int fault, gets, sets, starts, stops, creates, writes, drains, closes, peak;
  uint32_t volume;
} fixture_t;
static int info(void *raw, h2_audio_info_t *out) {
  fixture_t *f = raw;
  if (f->fault == 1)
    return H2_AUDIO_ERR_IO;
  *out = (h2_audio_info_t){
      .playback_format = {.sample_format = H2_AUDIO_SAMPLE_S16LE,
                          .sample_rate_hz = 16000,
                          .channels = 2,
                          .frame_samples_per_channel = 320}};
  return 0;
}
static int get(void *raw, uint32_t *out) {
  fixture_t *f = raw;
  ++f->gets;
  if ((f->fault == 2 && f->gets == 1) || (f->fault == 4 && f->gets == 2))
    return H2_AUDIO_ERR_IO;
  *out = f->volume;
  return 0;
}
static int set(void *raw, uint32_t n) {
  fixture_t *f = raw;
  ++f->sets;
  if ((f->fault == 3 && f->sets == 1) || (f->fault == 11 && f->sets == 2))
    return H2_AUDIO_ERR_IO;
  f->volume = n;
  return 0;
}
static int start(void *raw) {
  fixture_t *f = raw;
  ++f->starts;
  return f->fault == 5 ? H2_AUDIO_ERR_IO : 0;
}
static int stop(void *raw) {
  fixture_t *f = raw;
  ++f->stops;
  return f->fault == 10 ? H2_AUDIO_ERR_IO : 0;
}
static int write_frame(h2_pal_audio_track_t *track,
                       const h2_audio_frame_t *frame, uint32_t timeout) {
  fixture_t *f = track->user;
  ++f->writes;
  assert(timeout == 1000 && frame->bytes == 1280 && frame->capacity == 2048 &&
         frame->channels == 2);
  const int16_t *p = frame->data;
  for (unsigned i = 0; i < 320; ++i) {
    assert(p[2 * i] == p[2 * i + 1] && p[2 * i] >= -4096 && p[2 * i] <= 4096);
    int peak = p[2 * i] < 0 ? -p[2 * i] : p[2 * i];
    if (peak > f->peak)
      f->peak = peak;
    if ((f->writes - 1) % 20 >= 12)
      assert(p[2 * i] == 0);
  }
  return f->fault == 7 ? H2_AUDIO_ERR_IO : 0;
}
static int drain(h2_pal_audio_track_t *track, uint32_t timeout) {
  fixture_t *f = track->user;
  ++f->drains;
  assert(timeout == 3000);
  return f->fault == 8 ? H2_AUDIO_ERR_IO : 0;
}
static int close_track(h2_pal_audio_track_t *track) {
  fixture_t *f = track->user;
  ++f->closes;
  return f->fault == 9 ? H2_AUDIO_ERR_IO : 0;
}
static int create(void *raw, const h2_audio_track_config_t *config,
                  h2_pal_audio_track_t **out) {
  fixture_t *f = raw;
  ++f->creates;
  assert(config->volume_factor_milli == 1000 && f->volume == 100);
  if (f->fault == 6)
    return H2_AUDIO_ERR_IO;
  *out = &f->track;
  return 0;
}
static const h2_pal_audio_vtable_t vt = {.get_info = info,
                                         .get_speaker_volume_percent = get,
                                         .set_speaker_volume_percent = set,
                                         .start_speaker = start,
                                         .stop_speaker = stop,
                                         .create_track = create};
int main(void) {
  h2_mosaico_audible_result_t result;
  assert(h2_mosaico_audible_cue(NULL, NULL) == H2_AUDIO_ERR_INVALID_ARG);
  assert(h2_mosaico_audible_cue(NULL, &result) == H2_AUDIO_ERR_INVALID_ARG);
  for (int fault = 0; fault <= 11; ++fault) {
    fixture_t f = {.fault = fault, .volume = 70};
    f.track = (h2_pal_audio_track_t){
        .user = &f, .write = write_frame, .drain = drain, .close = close_track};
    h2_pal_audio_api_t api = {.user = &f, .vtable = &vt};
    int rc = h2_mosaico_audible_cue(&api, &result);
    if (fault == 0) {
      assert(rc == 0 && f.writes == 60 && result.blocks_written == 60 &&
             f.peak >= 4000 && f.closes == 1 && f.stops == 1);
    } else
      assert(rc == H2_AUDIO_ERR_IO);
    if (fault > 2 && fault != 11)
      assert(f.volume == 70 && result.restore_rc == 0);
    if (fault == 9)
      assert(f.closes == 1 && f.stops == 0 &&
             result.close_rc == H2_AUDIO_ERR_IO);
    if (fault == 10)
      assert(result.stop_rc == H2_AUDIO_ERR_IO);
    if (fault == 11)
      assert(result.restore_rc == H2_AUDIO_ERR_IO);
    if (fault == 3 || fault == 4 || fault == 5)
      assert(f.creates == 0);
    if (fault >= 7 && fault <= 11)
      assert(f.closes == 1);
  }
  return 0;
}
