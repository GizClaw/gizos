#include "aec_prepare.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

typedef struct fixture {
  uint64_t now;
  unsigned frames, registrations, unregistrations, stops;
  int mode, start_error, stop_error, unregister_error, read_error;
  h2_audio_aec_observer_t observer;
  mosaico_aec_preparation_t preparation;
} fixture_t;
static int time_ms(void* user, uint64_t* out) {
  *out = ((fixture_t*)user)->now;
  return 0;
}
static int observer(void* user, const h2_audio_aec_observer_t* value) {
  fixture_t* f = user;
  if (value) {
    ++f->registrations;
    assert(value->on_capture != NULL);
    f->observer = *value;
  } else {
    ++f->unregistrations;
    assert(f->stops > 0);
    return f->unregister_error;
  }
  return 0;
}
static int volume(void* user, uint32_t percent) {
  (void)user;
  assert(percent == 0);
  return 0;
}
static int start(void* user) { return ((fixture_t*)user)->start_error; }
static int stop(void* user) {
  fixture_t* f = user;
  ++f->stops;
  return f->stop_error;
}
static int read_pcm(void* user, h2_audio_frame_t* frame, uint32_t timeout) {
  fixture_t* f = user;
  assert(timeout == 100);
  f->now += 32;
  ++f->frames;
  if (f->read_error) return f->read_error;
  if (f->mode == 4) f->observer.on_error(f->observer.user, 1, H2_PAL_ERR_IO);
  int16_t value = 10;
  if (f->mode == 1 && f->frames <= 32) value = 31000;
  if (f->mode == 2 && ((f->frames - 1) / 16) % 2) value = 1000;
  int16_t* pcm = frame->data;
  for (unsigned i = 0; i < 512; ++i) pcm[i] = i % 2 ? value : -value;
  frame->bytes = f->mode == 3 ? 1022 : 1024;
  return 0;
}
static int run(fixture_t* f, uint32_t* windows, uint32_t* mse) {
  const h2_pal_time_vtable_t tv = {.get_monotonic_ms = time_ms};
  const h2_pal_time_api_t time = {.user = f, .vtable = &tv};
  const h2_pal_audio_vtable_t av = {.set_aec_observer = observer,
                                    .set_speaker_volume_percent = volume,
                                    .start_mic = start,
                                    .stop_mic = stop,
                                    .mic_read = read_pcm};
  const h2_pal_audio_api_t audio = {.user = f, .vtable = &av};
  return mosaico_aec_prepare(&audio, &time, &f->preparation, windows, mse);
}
int main(void) {
  uint32_t windows, mse;
  fixture_t f = {0};
  assert(run(&f, &windows, &mse) == 0);
  assert(f.frames >= 64 && windows >= 4 && mse == 100);
  assert(f.stops == 1 && f.unregistrations == 1);
  f = (fixture_t){.mode = 1};
  assert(run(&f, &windows, &mse) == 0);
  assert(f.frames >= 96 && f.stops == 1);
  f = (fixture_t){.mode = 2};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_TIMEOUT);
  assert(f.now >= 8000 && f.stops == 1 && f.unregistrations == 1);
  f = (fixture_t){.mode = 3};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_FORMAT);
  assert(f.stops == 1 && f.unregistrations == 1);
  f = (fixture_t){.start_error = H2_PAL_ERR_IO};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_IO);
  assert(f.stops == 1 && f.unregistrations == 1);
  f = (fixture_t){.read_error = H2_PAL_ERR_IO, .stop_error = H2_PAL_ERR_BUSY};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_BUSY);
  assert(f.stops == 1 && f.unregistrations == 0);
  f = (fixture_t){.mode = 4};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_IO);
  assert(f.stops == 1 && f.unregistrations == 1);
  f = (fixture_t){.unregister_error = H2_PAL_ERR_IO};
  assert(run(&f, &windows, &mse) == H2_PAL_ERR_IO);
  puts(
      "Mosaico preparation: settle, transient, timeout, format and cleanup passed.");
}
