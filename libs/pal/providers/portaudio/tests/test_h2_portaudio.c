#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_portaudio.h"
#include "h2_portaudio_internal.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <pthread.h>
#include "h2_atomic.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum fake_output_mode {
  FAKE_OUTPUT_ZERO_CAPACITY = 0,
  FAKE_OUTPUT_PARTIAL_CAPACITY,
  FAKE_OUTPUT_UNDERFLOW,
  FAKE_OUTPUT_SLOW_WRITE,
  FAKE_OUTPUT_AVAILABILITY_ERROR,
  FAKE_OUTPUT_WRITE_ERROR,
};

enum {
  FAKE_OUTPUT_ERROR = -10000,
  FAKE_OUTPUT_FRAME_SAMPLES = 320,
  FAKE_OUTPUT_PARTIAL_SAMPLES = 73,
};

typedef struct fake_output {
  h2_atomic_int_t mode;
  h2_atomic_int_t open_result;
  h2_atomic_int_t open_count;
  h2_atomic_int_t start_count;
  h2_atomic_int_t availability_count;
  h2_atomic_int_t write_count;
  h2_atomic_int_t stop_count;
  h2_atomic_int_t abort_count;
  h2_atomic_int_t close_count;
  h2_atomic_bool_t failure_observed;
  h2_atomic_bool_t write_active;
  h2_atomic_bool_t control_during_write;
  h2_atomic_size_t sample_count;
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES];
} fake_output_t;

enum fake_device_mode {
  FAKE_DEVICE_AVAILABLE = 0,
  FAKE_DEVICE_MISSING,
  FAKE_DEVICE_INFO_MISSING,
};

typedef struct fake_device {
  enum fake_device_mode mode;
  int device;
  int no_device;
  double high_output_latency;
  int info_count;
  int open_count;
  int observed_device;
  double observed_latency;
} fake_device_t;

typedef struct mic_reader {
  h2_pal_audio_t *audio;
  h2_audio_frame_t frame;
  int16_t samples[320];
  h2_atomic_bool_t entered;
  h2_atomic_bool_t done;
  int result;
} mic_reader_t;

typedef struct echo_order {
  h2_atomic_int_t reset_count;
  h2_atomic_int_t enqueue_count;
  h2_atomic_int_t rejected_enqueue_count;
  h2_atomic_int_t playback_count;
  h2_atomic_int_t capture_count;
  h2_atomic_bool_t capture_before_playback;
  h2_atomic_bool_t block_enqueue;
  h2_atomic_bool_t enqueue_entered;
  h2_atomic_bool_t enqueue_release;
} echo_order_t;

static h2_audio_frame_t mic_frame(int16_t *samples) {
  return (h2_audio_frame_t){
      .data = samples,
      .capacity = 320u * sizeof(*samples),
      .sample_rate_hz = 16000u,
      .samples_per_channel = 320u,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
}

static void sleep_ms(long milliseconds) {
  const struct timespec delay = {
      .tv_sec = milliseconds / 1000L,
      .tv_nsec = (milliseconds % 1000L) * 1000000L,
  };
  assert(nanosleep(&delay, NULL) == 0);
}

static void fake_output_init(fake_output_t *output) {
  assert(h2_atomic_init(&output->mode, FAKE_OUTPUT_ZERO_CAPACITY) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->open_result, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->open_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->start_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->availability_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->write_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->stop_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->abort_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->close_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->failure_observed, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->write_active, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->control_during_write, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&output->sample_count, 0u) == H2_ATOMIC_OK);
  memset(output->samples, 0, sizeof(output->samples));
}

static void fake_output_destroy(fake_output_t *output) {
  h2_atomic_destroy(&output->mode);
  h2_atomic_destroy(&output->open_result);
  h2_atomic_destroy(&output->open_count);
  h2_atomic_destroy(&output->start_count);
  h2_atomic_destroy(&output->availability_count);
  h2_atomic_destroy(&output->write_count);
  h2_atomic_destroy(&output->stop_count);
  h2_atomic_destroy(&output->abort_count);
  h2_atomic_destroy(&output->close_count);
  h2_atomic_destroy(&output->failure_observed);
  h2_atomic_destroy(&output->write_active);
  h2_atomic_destroy(&output->control_during_write);
  h2_atomic_destroy(&output->sample_count);
}

static void fake_output_reset(fake_output_t *output,
                              enum fake_output_mode mode) {
  h2_atomic_store(&output->mode, mode);
  h2_atomic_store(&output->open_result, 0);
  h2_atomic_store(&output->open_count, 0);
  h2_atomic_store(&output->start_count, 0);
  h2_atomic_store(&output->availability_count, 0);
  h2_atomic_store(&output->write_count, 0);
  h2_atomic_store(&output->stop_count, 0);
  h2_atomic_store(&output->abort_count, 0);
  h2_atomic_store(&output->close_count, 0);
  h2_atomic_store(&output->failure_observed, false);
  h2_atomic_store(&output->write_active, false);
  h2_atomic_store(&output->control_during_write, false);
  h2_atomic_store(&output->sample_count, 0u);
  memset(output->samples, 0, sizeof(output->samples));
}

static int fake_output_open(void *user, void **out_stream) {
  fake_output_t *output = user;
  h2_atomic_fetch_add(&output->open_count, 1);
  const int result = h2_atomic_load(&output->open_result);
  if (result != 0) {
    *out_stream = NULL;
    return result;
  }
  *out_stream = output;
  return 0;
}

static int fake_default_output_device(void *user) {
  fake_device_t *device = user;
  return device->mode == FAKE_DEVICE_MISSING ? device->no_device
                                              : device->device;
}

static int fake_default_high_output_latency(void *user, int device,
                                            double *out_latency) {
  fake_device_t *fake = user;
  assert(device == fake->device);
  ++fake->info_count;
  if (fake->mode == FAKE_DEVICE_INFO_MISSING)
    return h2_portaudio_internal_error_for_test();
  *out_latency = fake->high_output_latency;
  return 0;
}

static int fake_open_output_device(void *user, int device,
                                   double suggested_latency,
                                   void **out_stream) {
  fake_device_t *fake = user;
  ++fake->open_count;
  fake->observed_device = device;
  fake->observed_latency = suggested_latency;
  *out_stream = fake;
  return 0;
}

static int fake_output_start(void *user, void *stream) {
  fake_output_t *output = user;
  assert(stream == output);
  h2_atomic_fetch_add(&output->start_count, 1);
  return 0;
}

static long fake_output_write_available(void *user, void *stream) {
  fake_output_t *output = user;
  assert(stream == output);
  h2_atomic_fetch_add(&output->availability_count, 1);
  switch ((enum fake_output_mode)h2_atomic_load(&output->mode)) {
  case FAKE_OUTPUT_ZERO_CAPACITY:
    return 0;
  case FAKE_OUTPUT_PARTIAL_CAPACITY:
    return h2_atomic_load_explicit(&output->sample_count, H2_ATOMIC_ACQUIRE) <
                   FAKE_OUTPUT_FRAME_SAMPLES
               ? FAKE_OUTPUT_PARTIAL_SAMPLES
               : 0;
  case FAKE_OUTPUT_UNDERFLOW:
    return h2_atomic_load_explicit(&output->sample_count, H2_ATOMIC_ACQUIRE) <
                   FAKE_OUTPUT_FRAME_SAMPLES
               ? FAKE_OUTPUT_FRAME_SAMPLES
               : 0;
  case FAKE_OUTPUT_SLOW_WRITE:
  case FAKE_OUTPUT_WRITE_ERROR:
    return FAKE_OUTPUT_FRAME_SAMPLES;
  case FAKE_OUTPUT_AVAILABILITY_ERROR:
    h2_atomic_store(&output->failure_observed, true);
    return FAKE_OUTPUT_ERROR;
  }
  return FAKE_OUTPUT_ERROR;
}

static int fake_output_write(void *user, void *stream, const void *samples,
                             unsigned long frames) {
  fake_output_t *output = user;
  assert(stream == output);
  h2_atomic_fetch_add(&output->write_count, 1);
  h2_atomic_store_explicit(&output->write_active, true, H2_ATOMIC_RELEASE);
  const enum fake_output_mode mode =
      (enum fake_output_mode)h2_atomic_load(&output->mode);
  if (mode == FAKE_OUTPUT_SLOW_WRITE)
    sleep_ms(50L);
  if (mode == FAKE_OUTPUT_WRITE_ERROR) {
    h2_atomic_store(&output->failure_observed, true);
    h2_atomic_store_explicit(&output->write_active, false, H2_ATOMIC_RELEASE);
    return FAKE_OUTPUT_ERROR;
  }
  const size_t offset =
      h2_atomic_load_explicit(&output->sample_count, H2_ATOMIC_RELAXED);
  assert(frames <= FAKE_OUTPUT_FRAME_SAMPLES - offset);
  memcpy(output->samples + offset, samples, frames * sizeof(int16_t));
  h2_atomic_store_explicit(&output->sample_count, offset + frames,
                        H2_ATOMIC_RELEASE);
  h2_atomic_store_explicit(&output->write_active, false, H2_ATOMIC_RELEASE);
  if (mode == FAKE_OUTPUT_UNDERFLOW)
    return h2_portaudio_output_underflow_error_for_test();
  return 0;
}

static void fake_output_check_control(fake_output_t *output) {
  if (h2_atomic_load_explicit(&output->write_active, H2_ATOMIC_ACQUIRE))
    h2_atomic_store(&output->control_during_write, true);
}

static int fake_output_abort(void *user, void *stream) {
  fake_output_t *output = user;
  assert(stream == output);
  fake_output_check_control(output);
  h2_atomic_fetch_add(&output->abort_count, 1);
  return 0;
}

static int fake_output_stop(void *user, void *stream) {
  fake_output_t *output = user;
  assert(stream == output);
  fake_output_check_control(output);
  h2_atomic_fetch_add(&output->stop_count, 1);
  return 0;
}

static int fake_output_close(void *user, void *stream) {
  fake_output_t *output = user;
  assert(stream == output);
  fake_output_check_control(output);
  h2_atomic_fetch_add(&output->close_count, 1);
  return 0;
}

static const char *fake_output_error_text(void *user, int error) {
  assert(user != NULL);
  assert(error != 0);
  return "fake output error";
}

static void wait_for_int_at_least(h2_atomic_int_t *value, int target) {
  for (int elapsed_ms = 0; elapsed_ms < 1000; ++elapsed_ms) {
    if (h2_atomic_load_explicit(value, H2_ATOMIC_ACQUIRE) >= target)
      return;
    sleep_ms(1L);
  }
  _Exit(EXIT_FAILURE);
}

static void wait_for_size_at_least(h2_atomic_size_t *value, size_t target) {
  for (int elapsed_ms = 0; elapsed_ms < 1000; ++elapsed_ms) {
    if (h2_atomic_load_explicit(value, H2_ATOMIC_ACQUIRE) >= target)
      return;
    sleep_ms(1L);
  }
  _Exit(EXIT_FAILURE);
}

static void wait_for_underflow_count(h2_portaudio_t *provider,
                                     uint64_t target) {
  for (int elapsed_ms = 0; elapsed_ms < 1000; ++elapsed_ms) {
    if (h2_portaudio_output_underflow_count_for_test(provider) >= target)
      return;
    sleep_ms(1L);
  }
  _Exit(EXIT_FAILURE);
}

static void wait_for_bool(h2_atomic_bool_t *value) {
  for (int elapsed_ms = 0; elapsed_ms < 1000; ++elapsed_ms) {
    if (h2_atomic_load_explicit(value, H2_ATOMIC_ACQUIRE))
      return;
    sleep_ms(1L);
  }
  _Exit(EXIT_FAILURE);
}

static void fake_echo_playback(void *user, const int16_t *reference) {
  echo_order_t *order = user;
  assert(reference != NULL);
  h2_atomic_fetch_add_explicit(&order->playback_count, 1, H2_ATOMIC_RELEASE);
}

static void fake_echo_reset(void *user) {
  echo_order_t *order = user;
  h2_atomic_fetch_add_explicit(&order->reset_count, 1, H2_ATOMIC_RELEASE);
}

static void fake_echo_before_enqueue(void *user) {
  echo_order_t *order = user;
  if (!h2_atomic_load_explicit(&order->block_enqueue, H2_ATOMIC_ACQUIRE))
    return;
  h2_atomic_store_explicit(&order->enqueue_entered, true, H2_ATOMIC_RELEASE);
  while (!h2_atomic_load_explicit(&order->enqueue_release, H2_ATOMIC_ACQUIRE))
    sleep_ms(1L);
}

static void fake_echo_enqueue_result(void *user, int queued) {
  echo_order_t *order = user;
  if (queued) {
    h2_atomic_fetch_add_explicit(&order->enqueue_count, 1, H2_ATOMIC_RELEASE);
  } else {
    h2_atomic_fetch_add_explicit(&order->rejected_enqueue_count, 1,
                              H2_ATOMIC_RELEASE);
  }
}

static void fake_echo_capture(void *user, const int16_t *microphone,
                              int16_t *cleaned) {
  echo_order_t *order = user;
  assert(microphone != NULL);
  assert(cleaned != NULL);
  const int capture_count =
      h2_atomic_load_explicit(&order->capture_count, H2_ATOMIC_RELAXED);
  const int playback_count =
      h2_atomic_load_explicit(&order->playback_count, H2_ATOMIC_ACQUIRE);
  if (playback_count <= capture_count)
    h2_atomic_store(&order->capture_before_playback, true);
  memcpy(cleaned, microphone, FAKE_OUTPUT_FRAME_SAMPLES * sizeof(*cleaned));
  h2_atomic_fetch_add_explicit(&order->capture_count, 1, H2_ATOMIC_RELEASE);
}

static long elapsed_ms(const struct timespec *start,
                       const struct timespec *end) {
  return (end->tv_sec - start->tv_sec) * 1000L +
         (end->tv_nsec - start->tv_nsec) / 1000000L;
}

static void *read_mic_forever(void *user) {
  mic_reader_t *reader = user;
  h2_atomic_store(&reader->entered, true);
  reader->result = h2_pal_audio_mic_read(reader->audio, &reader->frame,
                                         H2_PAL_QUEUE_WAIT_FOREVER);
  h2_atomic_store(&reader->done, true);
  return NULL;
}

static void drain_mic_queue(h2_pal_audio_t *audio) {
  int16_t samples[320];
  h2_audio_frame_t frame = mic_frame(samples);
  int result;
  do {
    result = h2_pal_audio_mic_read(audio, &frame, H2_PAL_QUEUE_NO_WAIT);
  } while (result == H2_AUDIO_OK);
  assert(result == H2_AUDIO_ERR_WOULD_BLOCK);
}

static bool stop_wakes_blocked_reader(h2_pal_audio_t *audio) {
  mic_reader_t reader = {
      .audio = audio,
      .result = H2_AUDIO_OK,
  };
  reader.frame = mic_frame(reader.samples);
  assert(h2_atomic_init(&reader.entered, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&reader.done, false) == H2_ATOMIC_OK);

  pthread_t thread;
  assert(pthread_create(&thread, NULL, read_mic_forever, &reader) == 0);
  while (!h2_atomic_load(&reader.entered)) {
    sleep_ms(1L);
  }
  sleep_ms(2L);

  assert(h2_pal_audio_stop_mic(audio) == H2_AUDIO_OK);
  for (int elapsed_ms = 0; elapsed_ms < 1000 && !h2_atomic_load(&reader.done);
       ++elapsed_ms) {
    sleep_ms(1L);
  }
  if (!h2_atomic_load(&reader.done)) {
    _Exit(EXIT_FAILURE);
  }
  assert(pthread_join(thread, NULL) == 0);
  assert(reader.result == H2_AUDIO_OK ||
         reader.result == H2_AUDIO_ERR_INVALID_STATE);
  h2_atomic_destroy(&reader.entered);
  h2_atomic_destroy(&reader.done);
  return reader.result == H2_AUDIO_ERR_INVALID_STATE;
}

static h2_pal_audio_track_t *write_test_frame(h2_pal_audio_t *audio,
                                              int16_t *samples) {
  h2_audio_info_t info = {0};
  assert(h2_pal_audio_get_info(audio, &info) == H2_AUDIO_OK);
  const h2_audio_track_config_t config = {
      .name = "desktop-audio-lifecycle-test",
      .format = info.playback_format,
      .volume_factor_milli = 1000u,
      .buffer_frames = 2u,
  };
  h2_pal_audio_track_t *track = NULL;
  assert(h2_pal_audio_create_track(audio, &config, &track) == H2_AUDIO_OK);
  assert(track != NULL);
  h2_audio_frame_t frame = mic_frame(samples);
  frame.bytes = sizeof(int16_t) * FAKE_OUTPUT_FRAME_SAMPLES;
  assert(h2_pal_audio_track_write(track, &frame, 1000u) == H2_AUDIO_OK);
  return track;
}

static void test_default_output_device_selection(void) {
  fake_device_t device = {
      .mode = FAKE_DEVICE_AVAILABLE,
      .device = 37,
      .no_device = h2_portaudio_no_device_for_test(),
      .high_output_latency = 0.375,
  };
  const h2_portaudio_device_ops_t ops = {
      .user = &device,
      .default_output_device = fake_default_output_device,
      .default_high_output_latency = fake_default_high_output_latency,
      .open_output = fake_open_output_device,
  };
  void *stream = NULL;
  assert(h2_portaudio_open_default_output_for_test(&ops, &stream) == 0);
  assert(stream == &device);
  assert(device.info_count == 1);
  assert(device.open_count == 1);
  assert(device.observed_device == device.device);
  assert(device.observed_latency == device.high_output_latency);

  device.mode = FAKE_DEVICE_MISSING;
  device.info_count = 0;
  device.open_count = 0;
  stream = &device;
  assert(h2_portaudio_open_default_output_for_test(&ops, &stream) ==
         h2_portaudio_device_unavailable_error_for_test());
  assert(stream == NULL);
  assert(device.info_count == 0);
  assert(device.open_count == 0);

  device.mode = FAKE_DEVICE_INFO_MISSING;
  stream = &device;
  assert(h2_portaudio_open_default_output_for_test(&ops, &stream) ==
         h2_portaudio_internal_error_for_test());
  assert(stream == NULL);
  assert(device.info_count == 1);
  assert(device.open_count == 0);
}

static void test_output_open_error_policy(h2_portaudio_t *provider,
                                          h2_pal_audio_t *audio,
                                          fake_output_t *output) {
  const int errors[] = {
      h2_portaudio_device_unavailable_error_for_test(),
      h2_portaudio_internal_error_for_test(),
  };
  for (size_t i = 0u; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    fake_output_reset(output, FAKE_OUTPUT_ZERO_CAPACITY);
    h2_atomic_store(&output->open_result, errors[i]);
    assert(h2_portaudio_set_require_real_devices_for_test(provider, 0) ==
           H2_AUDIO_OK);
    assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
    assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
    assert(h2_atomic_load(&output->open_count) == 1);
    assert(h2_atomic_load(&output->start_count) == 0);

    fake_output_reset(output, FAKE_OUTPUT_ZERO_CAPACITY);
    h2_atomic_store(&output->open_result, errors[i]);
    assert(h2_portaudio_set_require_real_devices_for_test(provider, 1) ==
           H2_AUDIO_OK);
    assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_ERR_UNAVAILABLE);
    assert(h2_atomic_load(&output->open_count) == 1);
    assert(h2_atomic_load(&output->start_count) == 0);
  }
  assert(h2_portaudio_set_require_real_devices_for_test(provider, 0) ==
         H2_AUDIO_OK);
}

static void test_zero_capacity_stop(h2_pal_audio_t *audio,
                                    fake_output_t *output) {
  fake_output_reset(output, FAKE_OUTPUT_ZERO_CAPACITY);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES];
  for (size_t i = 0u; i < FAKE_OUTPUT_FRAME_SAMPLES; ++i)
    samples[i] = (int16_t)i;
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_int_at_least(&output->availability_count, 1);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);

  struct timespec start;
  struct timespec end;
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
  assert(elapsed_ms(&start, &end) < 1000L);
  assert(h2_atomic_load(&output->write_count) == 0);
  assert(h2_atomic_load(&output->stop_count) == 1);
  assert(h2_atomic_load(&output->abort_count) == 0);
  assert(h2_atomic_load(&output->close_count) == 1);
  assert(!h2_atomic_load(&output->control_during_write));
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&output->stop_count) == 1);
  assert(h2_atomic_load(&output->abort_count) == 0);
  assert(h2_atomic_load(&output->close_count) == 1);
}

static void test_partial_frame(h2_pal_audio_t *audio, fake_output_t *output) {
  fake_output_reset(output, FAKE_OUTPUT_PARTIAL_CAPACITY);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES];
  for (size_t i = 0u; i < FAKE_OUTPUT_FRAME_SAMPLES; ++i)
    samples[i] = (int16_t)(1000 + i);
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_size_at_least(&output->sample_count, FAKE_OUTPUT_FRAME_SAMPLES);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&output->write_count) > 1);
  assert(h2_atomic_load(&output->sample_count) == FAKE_OUTPUT_FRAME_SAMPLES);
  assert(memcmp(output->samples, samples, sizeof(samples)) == 0);
  assert(!h2_atomic_load(&output->control_during_write));
}

static void test_stop_waits_for_active_write(h2_pal_audio_t *audio,
                                             fake_output_t *output) {
  fake_output_reset(output, FAKE_OUTPUT_SLOW_WRITE);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES] = {0};
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_bool(&output->write_active);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_atomic_load(&output->stop_count) == 1);
  assert(h2_atomic_load(&output->abort_count) == 0);
  assert(h2_atomic_load(&output->close_count) == 1);
  assert(!h2_atomic_load(&output->control_during_write));
}

static void test_output_underflow_is_recoverable(h2_portaudio_t *provider,
                                                 h2_pal_audio_t *audio,
                                                 fake_output_t *output) {
  fake_output_reset(output, FAKE_OUTPUT_UNDERFLOW);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES] = {0};
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_underflow_count(provider, 1u);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_portaudio_output_underflow_count_for_test(provider) == 1u);
  assert(h2_atomic_load(&output->write_count) == 1);
  assert(h2_atomic_load(&output->stop_count) == 1);
  assert(h2_atomic_load(&output->abort_count) == 0);
  assert(h2_atomic_load(&output->close_count) == 1);

  fake_output_reset(output, FAKE_OUTPUT_ZERO_CAPACITY);
  track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_int_at_least(&output->availability_count, 1);
  assert(h2_portaudio_output_underflow_count_for_test(provider) == 0u);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
}

static void test_worker_failure_restart(h2_pal_audio_t *audio,
                                        fake_output_t *output,
                                        enum fake_output_mode failure_mode) {
  fake_output_reset(output, failure_mode);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES] = {0};
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  wait_for_bool(&output->failure_observed);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);

  h2_atomic_store(&output->mode, FAKE_OUTPUT_ZERO_CAPACITY);
  h2_pal_audio_track_t *restart_track = write_test_frame(audio, samples);
  for (int elapsed_ms = 0;
       elapsed_ms < 1000 && h2_atomic_load(&output->open_count) < 2;
       ++elapsed_ms) {
    assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
    if (h2_atomic_load(&output->open_count) < 2)
      sleep_ms(1L);
  }
  assert(h2_atomic_load(&output->open_count) == 2);
  assert(h2_atomic_load(&output->abort_count) == 1);
  assert(h2_atomic_load(&output->close_count) == 1);
  wait_for_int_at_least(&output->availability_count, 2);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_pal_audio_track_close(restart_track) == H2_AUDIO_OK);
  assert(h2_atomic_load(&output->stop_count) == 1);
  assert(h2_atomic_load(&output->abort_count) == 1);
  assert(h2_atomic_load(&output->close_count) == 2);
  assert(!h2_atomic_load(&output->control_during_write));
}

static void test_echo_reference_precedes_capture(h2_portaudio_t *provider,
                                                 h2_pal_audio_t *audio,
                                                 fake_output_t *output) {
  echo_order_t order = {0};
  assert(h2_atomic_init(&order.reset_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.enqueue_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.rejected_enqueue_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.playback_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.capture_count, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.capture_before_playback, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.block_enqueue, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.enqueue_entered, false) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&order.enqueue_release, false) == H2_ATOMIC_OK);
  const h2_portaudio_echo_test_ops_t echo_ops = {
      .user = &order,
      .reset = fake_echo_reset,
      .before_enqueue = fake_echo_before_enqueue,
      .enqueue_result = fake_echo_enqueue_result,
      .playback = fake_echo_playback,
      .capture = fake_echo_capture,
  };
  assert(h2_portaudio_set_echo_test_ops(provider, &echo_ops) == H2_AUDIO_OK);

  fake_output_reset(output, FAKE_OUTPUT_PARTIAL_CAPACITY);
  int16_t samples[FAKE_OUTPUT_FRAME_SAMPLES] = {0};
  assert(h2_pal_audio_start_mic(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.reset_count) == 1);

  h2_audio_frame_t raw_capture = mic_frame(samples);
  assert(h2_pal_audio_mic_read(audio, &raw_capture, 1000u) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.playback_count) == 0);
  assert(h2_atomic_load(&order.capture_count) == 0);

  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  h2_pal_audio_track_t *track = write_test_frame(audio, samples);
  wait_for_int_at_least(&order.capture_count, 1);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.reset_count) == 2);

  const int captures_before_restart = h2_atomic_load(&order.capture_count);
  fake_output_reset(output, FAKE_OUTPUT_PARTIAL_CAPACITY);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  track = write_test_frame(audio, samples);
  wait_for_int_at_least(&order.capture_count, captures_before_restart + 1);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.reset_count) == 3);

  fake_output_reset(output, FAKE_OUTPUT_PARTIAL_CAPACITY);
  h2_atomic_store(&order.block_enqueue, true);
  assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
  track = write_test_frame(audio, samples);
  wait_for_bool(&order.enqueue_entered);
  assert(h2_pal_audio_stop_mic(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.reset_count) == 4);
  h2_atomic_store_explicit(&order.enqueue_release, true, H2_ATOMIC_RELEASE);
  wait_for_int_at_least(&order.rejected_enqueue_count, 1);
  assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
  assert(h2_atomic_load(&order.reset_count) == 5);
  assert(!h2_atomic_load(&order.capture_before_playback));
  assert(h2_atomic_load(&order.playback_count) >=
         h2_atomic_load(&order.capture_count));
  h2_atomic_destroy(&order.reset_count);
  h2_atomic_destroy(&order.enqueue_count);
  h2_atomic_destroy(&order.rejected_enqueue_count);
  h2_atomic_destroy(&order.playback_count);
  h2_atomic_destroy(&order.capture_count);
  h2_atomic_destroy(&order.capture_before_playback);
  h2_atomic_destroy(&order.block_enqueue);
  h2_atomic_destroy(&order.enqueue_entered);
  h2_atomic_destroy(&order.enqueue_release);

}

int main(void) {
  test_default_output_device_selection();
  const h2_portaudio_config_t config = {
      .allocator = h2_desktop_platform_default_allocator(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .require_real_devices = 0,
  };
  h2_portaudio_t *provider = NULL;
  h2_portaudio_config_t missing_sync = config;
  missing_sync.sync = NULL;
  assert(h2_portaudio_create(&missing_sync, &provider) ==
         H2_AUDIO_ERR_INVALID_ARG);
  assert(provider == NULL);
  assert(h2_portaudio_create(&config, &provider) == H2_AUDIO_OK);
  fake_output_t output = {0};
  fake_output_init(&output);
  const h2_portaudio_output_test_ops_t output_ops = {
      .user = &output,
      .open = fake_output_open,
      .start = fake_output_start,
      .write_available = fake_output_write_available,
      .write = fake_output_write,
      .stop = fake_output_stop,
      .abort = fake_output_abort,
      .close = fake_output_close,
      .error_text = fake_output_error_text,
  };
  assert(h2_portaudio_set_output_test_ops(provider, &output_ops) ==
         H2_AUDIO_OK);
  h2_pal_audio_t *audio = h2_portaudio_audio(provider);

  int16_t samples[320];
  h2_audio_frame_t frame;
  bool observed_stop_wakeup = false;
  for (int attempt = 0; attempt < 10 && !observed_stop_wakeup; ++attempt) {
    assert(h2_pal_audio_start_mic(audio) == H2_AUDIO_OK);
    frame = mic_frame(samples);
    assert(h2_pal_audio_mic_read(audio, &frame, 1000u) == H2_AUDIO_OK);
    drain_mic_queue(audio);
    observed_stop_wakeup = stop_wakes_blocked_reader(audio);
  }
  assert(observed_stop_wakeup);

  assert(h2_pal_audio_start_mic(audio) == H2_AUDIO_OK);
  frame = mic_frame(samples);
  assert(h2_pal_audio_mic_read(audio, &frame, 1000u) == H2_AUDIO_OK);
  assert(h2_pal_audio_stop_mic(audio) == H2_AUDIO_OK);

  test_zero_capacity_stop(audio, &output);
  test_output_open_error_policy(provider, audio, &output);
  test_partial_frame(audio, &output);
  test_stop_waits_for_active_write(audio, &output);
  test_output_underflow_is_recoverable(provider, audio, &output);
  test_worker_failure_restart(audio, &output, FAKE_OUTPUT_AVAILABILITY_ERROR);
  test_worker_failure_restart(audio, &output, FAKE_OUTPUT_WRITE_ERROR);
  test_echo_reference_precedes_capture(provider, audio, &output);
  h2_portaudio_destroy(provider);
  fake_output_destroy(&output);
  return 0;
}
