#include "h2_portaudio.h"
#include "h2_portaudio_internal.h"

#include "h2_audio_mixer.h"

#include <portaudio.h>
#include <speex/speex_echo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace {

constexpr uint32_t kSampleRate = 16000u;
constexpr uint16_t kFrameSamples = 320u;
constexpr uint8_t kChannels = 1u;
constexpr size_t kFrameValues = static_cast<size_t>(kFrameSamples) * kChannels;
constexpr uint8_t kMaxTracks = 4u;
constexpr uint8_t kTrackQueueFrames = 4u;
constexpr uint8_t kMicQueueFrames = 4u;
constexpr size_t kEchoReferenceFrames = 16u;
constexpr auto kPlaybackPollInterval = std::chrono::milliseconds(1);

int portaudio_output_open(void *, void **out_stream) {
  const PaDeviceIndex device = Pa_GetDefaultOutputDevice();
  if (device == paNoDevice) {
    *out_stream = nullptr;
    return paDeviceUnavailable;
  }
  const PaDeviceInfo *device_info = Pa_GetDeviceInfo(device);
  if (device_info == nullptr) {
    *out_stream = nullptr;
    return paInternalError;
  }
  const PaStreamParameters output_parameters = {
      device,
      kChannels,
      paInt16,
      device_info->defaultHighOutputLatency,
      nullptr,
  };
  PaStream *stream = nullptr;
  const PaError error = Pa_OpenStream(
      &stream, nullptr, &output_parameters, kSampleRate, kFrameSamples,
      paNoFlag, nullptr, nullptr);
  *out_stream = stream;
  return error;
}

int portaudio_output_start(void *, void *stream) {
  return Pa_StartStream(static_cast<PaStream *>(stream));
}

long portaudio_output_write_available(void *, void *stream) {
  return Pa_GetStreamWriteAvailable(static_cast<PaStream *>(stream));
}

int portaudio_output_write(void *, void *stream, const void *samples,
                           unsigned long frames) {
  return Pa_WriteStream(static_cast<PaStream *>(stream), samples, frames);
}

int portaudio_output_stop(void *, void *stream) {
  return Pa_StopStream(static_cast<PaStream *>(stream));
}

int portaudio_output_abort(void *, void *stream) {
  return Pa_AbortStream(static_cast<PaStream *>(stream));
}

int portaudio_output_close(void *, void *stream) {
  return Pa_CloseStream(static_cast<PaStream *>(stream));
}

const char *portaudio_output_error_text(void *, int error) {
  return Pa_GetErrorText(static_cast<PaError>(error));
}

const h2_portaudio_output_test_ops_t kPortAudioOutputOps = {
    nullptr,
    portaudio_output_open,
    portaudio_output_start,
    portaudio_output_write_available,
    portaudio_output_write,
    portaudio_output_stop,
    portaudio_output_abort,
    portaudio_output_close,
    portaudio_output_error_text,
};

struct MicQueueFrame {
  size_t bytes = 0u;
  uint16_t samples_per_channel = 0u;
  std::array<int16_t, kFrameValues> samples = {};
};

struct AudioState {
  const h2_pal_mem_api_t *allocator = nullptr;
  const h2_pal_queue_api_t *queue = nullptr;
  const h2_pal_sync_api_t *sync = nullptr;
  std::mutex control_mutex;
  std::mutex echo_mutex;
  std::mutex mic_read_mutex;
  bool mixer_initialized = false;
  bool portaudio_initialized = false;
  bool portaudio_enabled = false;
  bool require_real_devices = false;
  int input_channels = 0;
  bool mic_thread_started = false;
  std::atomic<bool> mic_running = false;
  std::thread mic_thread;
  bool playback_thread_started = false;
  std::atomic<bool> playback_running = false;
  std::atomic<int> playback_result = H2_AUDIO_OK;
  std::atomic<uint64_t> output_underflow_count = 0u;
  std::thread playback_thread;
  std::atomic<uint32_t> speaker_volume_percent = 100u;
  uint32_t mic_counter = 0u;
  h2_pal_queue_t *mic_queue = nullptr;
  PaStream *input_stream = nullptr;
  PaStream *output_stream = nullptr;
  h2_portaudio_output_test_ops_t output_ops = kPortAudioOutputOps;
  bool output_ops_overridden = false;
  h2_portaudio_echo_test_ops_t echo_test_ops = {};
  bool echo_test_ops_overridden = false;
  h2_audio_mixer_t mixer = {};
  SpeexEchoState *echo_state = nullptr;
  h2_pal_audio_t audio = {};
  std::array<int16_t, kFrameValues> mic_scratch = {};
  std::array<int16_t, kFrameValues> playback_scratch = {};
  std::array<int16_t, kFrameValues> reference_scratch = {};
  std::array<std::array<int16_t, kFrameValues>, kEchoReferenceFrames>
      echo_reference_queue = {};
  size_t echo_reference_head = 0u;
  size_t echo_reference_count = 0u;
  std::array<int16_t, kFrameValues> cleaned_scratch = {};
};

} // namespace

struct h2_portaudio {
  AudioState state;
};

namespace {

h2_audio_pcm_format_t desktop_format() {
  return {kSampleRate, kFrameSamples, kChannels, H2_AUDIO_SAMPLE_S16LE};
}

h2_audio_frame_t frame_for_buffer(int16_t *buffer, size_t capacity) {
  const h2_audio_pcm_format_t format = desktop_format();
  return {buffer,
          capacity,
          0u,
          format.sample_rate_hz,
          format.frame_samples_per_channel,
          format.channels,
          format.sample_format};
}

int init_portaudio(AudioState *state) {
  if (state->portaudio_initialized || !state->portaudio_enabled) {
    return H2_AUDIO_OK;
  }
  const PaError error = Pa_Initialize();
  if (error == paNoError) {
    state->portaudio_initialized = true;
    return H2_AUDIO_OK;
  }
  if (state->require_real_devices) {
    std::fprintf(stderr, "desktop audio: PortAudio init failed: %s\n",
                 Pa_GetErrorText(error));
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  std::fprintf(stderr,
               "desktop audio: PortAudio init failed: %s; using "
               "deterministic backend\n",
               Pa_GetErrorText(error));
  state->portaudio_enabled = false;
  return H2_AUDIO_OK;
}

int init_audio(AudioState *state) {
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (!state->portaudio_initialized && !state->portaudio_enabled) {
    state->portaudio_enabled = true;
  }
  const int portaudio_result = init_portaudio(state);
  if (portaudio_result != H2_AUDIO_OK) {
    return portaudio_result;
  }
  if (state->echo_state == nullptr) {
    state->echo_state =
        speex_echo_state_init(kFrameSamples, kFrameSamples * 10u);
    if (state->echo_state != nullptr) {
      int sample_rate = static_cast<int>(kSampleRate);
      (void)speex_echo_ctl(state->echo_state, SPEEX_ECHO_SET_SAMPLING_RATE,
                           &sample_rate);
    }
  }
  if (!state->mixer_initialized) {
    const h2_audio_mixer_config_t config = {
        desktop_format(),
        kMaxTracks,
        kTrackQueueFrames,
        0u,
        state->allocator,
        state->queue,
        state->sync,
    };
    const int result = h2_audio_mixer_init(&state->mixer, &config);
    if (result != H2_AUDIO_OK) {
      return result;
    }
    state->mixer_initialized = true;
  }
  return H2_AUDIO_OK;
}

int ensure_mic_queue(AudioState *state) {
  if (state->mic_queue != nullptr) {
    return H2_AUDIO_OK;
  }
  const h2_pal_queue_config_t config = {
      "desktop_mic",
      sizeof(MicQueueFrame),
      kMicQueueFrames,
      state->allocator,
  };
  const int result = h2_pal_queue_create(state->queue,
                                         &config, &state->mic_queue);
  switch (result) {
  case H2_PAL_QUEUE_OK:
    return H2_AUDIO_OK;
  case H2_PAL_QUEUE_ERR_NO_MEMORY:
    return H2_AUDIO_ERR_NO_MEMORY;
  case H2_PAL_QUEUE_ERR_INVALID_ARG:
    return H2_AUDIO_ERR_INVALID_ARG;
  default:
    return H2_AUDIO_ERR_IO;
  }
}

int open_input(AudioState *state) {
  const int init_result = init_audio(state);
  if (init_result != H2_AUDIO_OK || state->input_stream != nullptr) {
    return init_result;
  }
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (!state->portaudio_enabled || Pa_GetDefaultInputDevice() == paNoDevice) {
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  state->input_channels = kChannels;
  const PaDeviceInfo *device = Pa_GetDeviceInfo(Pa_GetDefaultInputDevice());
  if (device != nullptr && device->maxInputChannels > 0 &&
      device->maxInputChannels < state->input_channels) {
    state->input_channels = device->maxInputChannels;
  }
  PaError error = Pa_OpenDefaultStream(
      &state->input_stream, state->input_channels, 0, paInt16, kSampleRate,
      kFrameSamples, nullptr, nullptr);
  if (error != paNoError) {
    std::fprintf(stderr, "desktop audio: input stream open failed: %s\n",
                 Pa_GetErrorText(error));
    state->input_stream = nullptr;
    state->input_channels = 0;
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  error = Pa_StartStream(state->input_stream);
  if (error != paNoError) {
    std::fprintf(stderr, "desktop audio: input stream start failed: %s\n",
                 Pa_GetErrorText(error));
    (void)Pa_CloseStream(state->input_stream);
    state->input_stream = nullptr;
    state->input_channels = 0;
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  return H2_AUDIO_OK;
}

int open_output(AudioState *state) {
  const int init_result = init_audio(state);
  if (init_result != H2_AUDIO_OK || state->output_stream != nullptr) {
    return init_result;
  }
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (!state->output_ops_overridden &&
      (!state->portaudio_enabled ||
       Pa_GetDefaultOutputDevice() == paNoDevice)) {
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  void *stream = nullptr;
  int error = state->output_ops.open(state->output_ops.user, &stream);
  if (error != paNoError) {
    std::fprintf(stderr, "desktop audio: output stream open failed: %s\n",
                 state->output_ops.error_text(state->output_ops.user, error));
    state->output_stream = nullptr;
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  state->output_stream = static_cast<PaStream *>(stream);
  error = state->output_ops.start(state->output_ops.user, stream);
  if (error != paNoError) {
    std::fprintf(stderr, "desktop audio: output stream start failed: %s\n",
                 state->output_ops.error_text(state->output_ops.user, error));
    (void)state->output_ops.close(state->output_ops.user, stream);
    state->output_stream = nullptr;
    return state->require_real_devices ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK;
  }
  state->output_underflow_count.store(0u);
  return H2_AUDIO_OK;
}

int16_t apply_volume(int16_t sample, uint32_t percent) {
  const int64_t scaled =
      static_cast<int64_t>(sample) * static_cast<int64_t>(percent) / 100;
  return static_cast<int16_t>(std::max<int64_t>(
      std::numeric_limits<int16_t>::min(),
      std::min<int64_t>(std::numeric_limits<int16_t>::max(), scaled)));
}

void abort_stream(PaStream *stream, const char *kind) {
  if (stream == nullptr) {
    return;
  }
  const PaError error = Pa_AbortStream(stream);
  if (error != paNoError && error != paStreamIsStopped) {
    std::fprintf(stderr, "desktop audio: %s stream abort failed: %s\n", kind,
                 Pa_GetErrorText(error));
  }
}

int close_output_stream(AudioState *state, bool abort) {
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (state->output_stream == nullptr) {
    return H2_AUDIO_OK;
  }
  void *stream = state->output_stream;
  int result = H2_AUDIO_OK;
  const int stop_error =
      abort ? state->output_ops.abort(state->output_ops.user, stream)
            : state->output_ops.stop(state->output_ops.user, stream);
  if (stop_error != paNoError && stop_error != paStreamIsStopped) {
    std::fprintf(
        stderr, "desktop audio: output stream %s failed: %s\n",
        abort ? "abort" : "stop",
        state->output_ops.error_text(state->output_ops.user, stop_error));
    result = H2_AUDIO_ERR_IO;
  }
  const int close_error =
      state->output_ops.close(state->output_ops.user, stream);
  if (close_error != paNoError) {
    std::fprintf(
        stderr, "desktop audio: output stream close failed: %s\n",
        state->output_ops.error_text(state->output_ops.user, close_error));
    result = H2_AUDIO_ERR_IO;
  }
  std::fprintf(
      stderr, "desktop audio: output underflow count=%llu\n",
      static_cast<unsigned long long>(state->output_underflow_count.load()));
  state->output_stream = nullptr;
  return result;
}

void reset_echo_reference(AudioState *state) {
  std::lock_guard<std::mutex> lock(state->echo_mutex);
  state->echo_reference_head = 0u;
  state->echo_reference_count = 0u;
  if (state->echo_test_ops_overridden) {
    state->echo_test_ops.reset(state->echo_test_ops.user);
  }
  if (state->echo_state != nullptr) {
    speex_echo_state_reset(state->echo_state);
  }
}

void queue_echo_reference(AudioState *state) {
  if (state->echo_state == nullptr) {
    return;
  }
  if (state->echo_test_ops_overridden) {
    state->echo_test_ops.before_enqueue(state->echo_test_ops.user);
  }
  std::lock_guard<std::mutex> lock(state->echo_mutex);
  if (!state->mic_running.load()) {
    if (state->echo_test_ops_overridden) {
      state->echo_test_ops.enqueue_result(state->echo_test_ops.user, 0);
    }
    return;
  }
  if (state->echo_reference_count == kEchoReferenceFrames) {
    state->echo_reference_head =
        (state->echo_reference_head + 1u) % kEchoReferenceFrames;
    --state->echo_reference_count;
  }
  const size_t tail = (state->echo_reference_head +
                       state->echo_reference_count) %
                      kEchoReferenceFrames;
  state->echo_reference_queue[tail] = state->reference_scratch;
  ++state->echo_reference_count;
  if (state->echo_test_ops_overridden) {
    state->echo_test_ops.enqueue_result(state->echo_test_ops.user, 1);
  }
}

bool clean_echo_capture(AudioState *state, MicQueueFrame *item) {
  if (state->echo_state == nullptr || !state->playback_running.load() ||
      item->bytes != sizeof(state->cleaned_scratch)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->echo_mutex);
  if (state->echo_reference_count == 0u) {
    return false;
  }
  const std::array<int16_t, kFrameValues> &reference =
      state->echo_reference_queue[state->echo_reference_head];
  state->echo_reference_head =
      (state->echo_reference_head + 1u) % kEchoReferenceFrames;
  --state->echo_reference_count;
  if (state->echo_test_ops_overridden) {
    state->echo_test_ops.playback(state->echo_test_ops.user, reference.data());
    state->echo_test_ops.capture(state->echo_test_ops.user,
                                 item->samples.data(),
                                 state->cleaned_scratch.data());
  } else {
    speex_echo_playback(state->echo_state, reference.data());
    speex_echo_capture(state->echo_state, item->samples.data(),
                       state->cleaned_scratch.data());
  }
  item->samples = state->cleaned_scratch;
  return true;
}

void playback_main(AudioState *state) {
  size_t pending_frames = 0u;
  size_t pending_offset = 0u;
  bool pending_echo_reference = false;
  while (state->playback_running.load()) {
    if (pending_frames == 0u) {
      h2_audio_frame_t frame = frame_for_buffer(
          state->playback_scratch.data(), sizeof(state->playback_scratch));
      h2_audio_frame_t reference = frame_for_buffer(
          state->reference_scratch.data(), sizeof(state->reference_scratch));
      const int result =
          h2_audio_mixer_read_with_reference(&state->mixer, &frame, &reference);
      if (result != H2_AUDIO_OK || frame.bytes == 0u) {
        std::this_thread::sleep_for(kPlaybackPollInterval);
        continue;
      }
      const size_t sample_count = frame.bytes / sizeof(int16_t);
      const uint32_t volume = state->speaker_volume_percent.load();
      for (size_t index = 0; index < sample_count; ++index) {
        state->playback_scratch[index] =
            apply_volume(state->playback_scratch[index], volume);
        state->reference_scratch[index] =
            apply_volume(state->reference_scratch[index], volume);
      }
      pending_frames =
          frame.bytes / (sizeof(int16_t) * static_cast<size_t>(kChannels));
      pending_offset = 0u;
      pending_echo_reference = reference.bytes == frame.bytes;
    }

    PaStream *stream = nullptr;
    {
      std::lock_guard<std::mutex> lock(state->control_mutex);
      stream = state->output_stream;
    }
    if (stream == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      pending_frames = 0u;
      pending_offset = 0u;
      pending_echo_reference = false;
      continue;
    }

    const long available =
        state->output_ops.write_available(state->output_ops.user, stream);
    if (available < 0) {
      std::fprintf(stderr, "desktop audio: output availability failed: %s\n",
                   state->output_ops.error_text(state->output_ops.user,
                                                static_cast<int>(available)));
      state->playback_result.store(H2_AUDIO_ERR_IO);
      break;
    }
    if (available == 0) {
      std::this_thread::sleep_for(kPlaybackPollInterval);
      continue;
    }

    const size_t frames_to_write =
        std::min(pending_frames,
                 std::min(static_cast<size_t>(available),
                          static_cast<size_t>(
                              std::numeric_limits<unsigned long>::max())));
    const int16_t *samples =
        state->playback_scratch.data() + pending_offset * kChannels;
    const int error =
        state->output_ops.write(state->output_ops.user, stream, samples,
                                static_cast<unsigned long>(frames_to_write));
    if (error == paOutputUnderflowed) {
      state->output_underflow_count.fetch_add(1u);
    }
    if (error != paNoError && error != paOutputUnderflowed) {
      std::fprintf(stderr, "desktop audio: output write failed: %s\n",
                   state->output_ops.error_text(state->output_ops.user, error));
      state->playback_result.store(H2_AUDIO_ERR_IO);
      break;
    }
    pending_frames -= frames_to_write;
    pending_offset += frames_to_write;
    if (pending_frames == 0u && pending_echo_reference) {
      queue_echo_reference(state);
      pending_echo_reference = false;
    }
  }
  reset_echo_reference(state);
  state->playback_running.store(false);
}

int reap_stopped_playback_thread(AudioState *state) {
  if (!state->playback_thread_started || state->playback_running.load()) {
    return H2_AUDIO_OK;
  }
  state->playback_thread.join();
  state->playback_thread_started = false;
  return close_output_stream(state, true);
}

int capture_mic_frame(AudioState *state, MicQueueFrame *item) {
  bool used_device = false;
  PaStream *stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->control_mutex);
    stream = state->input_stream;
  }
  if (stream != nullptr) {
    const signed long available = Pa_GetStreamReadAvailable(stream);
    if (available >= 0 && available < static_cast<signed long>(kFrameSamples)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return 0;
    }
    if (available < 0) {
      std::fprintf(stderr, "desktop audio: input availability failed: %s\n",
                   Pa_GetErrorText(static_cast<PaError>(available)));
    } else {
      PaError error =
          Pa_ReadStream(stream, state->mic_scratch.data(), kFrameSamples);
      if (error == paInputOverflowed) {
        error = paNoError;
      }
      if (error == paNoError) {
        used_device = true;
      } else {
        std::fprintf(stderr, "desktop audio: input read failed: %s\n",
                     Pa_GetErrorText(error));
      }
    }
  }
  if (!used_device) {
    if (state->require_real_devices) {
      return -1;
    }
    for (int16_t &sample : state->mic_scratch) {
      sample = static_cast<int16_t>(
          static_cast<int32_t>(state->mic_counter & 0x3fu) - 32);
      ++state->mic_counter;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  item->bytes = sizeof(state->mic_scratch);
  item->samples_per_channel = kFrameSamples;
  item->samples = state->mic_scratch;
  return 1;
}

void mic_main(AudioState *state) {
  while (state->mic_running.load()) {
    if (state->mic_queue == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    MicQueueFrame item;
    const int capture_result = capture_mic_frame(state, &item);
    if (capture_result < 0) {
      state->mic_running.store(false);
      (void)h2_pal_queue_close(state->queue,
                               state->mic_queue);
      break;
    }
    if (capture_result == 0) {
      continue;
    }
    (void)clean_echo_capture(state, &item);
    (void)h2_pal_queue_send_latest(state->queue,
                                   state->mic_queue, &item);
  }
}

void reap_stopped_mic_thread(AudioState *state) {
  if (!state->mic_thread_started || state->mic_running.load()) {
    return;
  }
  state->mic_thread.join();
  state->mic_thread_started = false;

  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (state->input_stream != nullptr) {
    abort_stream(state->input_stream, "input");
    (void)Pa_CloseStream(state->input_stream);
    state->input_stream = nullptr;
    state->input_channels = 0;
  }
}

int audio_get_info(void *user, h2_audio_info_t *info) {
  if (user == nullptr || info == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  const int result = init_audio(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  if (state->require_real_devices &&
      (Pa_GetDefaultInputDevice() == paNoDevice ||
       Pa_GetDefaultOutputDevice() == paNoDevice)) {
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  *info = {};
  info->available = 1;
  info->mic_supported = 1;
  info->playback_supported = 1;
  info->mic_format = desktop_format();
  info->playback_format = desktop_format();
  info->mic_queue_frames = kMicQueueFrames;
  info->track_queue_frames = kTrackQueueFrames;
  info->max_tracks = kMaxTracks;
  return H2_AUDIO_OK;
}

int audio_start_mic(void *user) {
  if (user == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  int result = ensure_mic_queue(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  reap_stopped_mic_thread(state);
  result = open_input(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  (void)h2_pal_queue_reset(state->queue, state->mic_queue);
  reset_echo_reference(state);
  state->mic_running.store(true);
  if (!state->mic_thread_started) {
    try {
      state->mic_thread = std::thread(mic_main, state);
      state->mic_thread_started = true;
    } catch (...) {
      state->mic_running.store(false);
      std::lock_guard<std::mutex> lock(state->control_mutex);
      if (state->input_stream != nullptr) {
        abort_stream(state->input_stream, "input");
        (void)Pa_CloseStream(state->input_stream);
        state->input_stream = nullptr;
        state->input_channels = 0;
      }
      return H2_AUDIO_ERR_IO;
    }
  }
  return H2_AUDIO_OK;
}

int audio_stop_mic(void *user) {
  if (user == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  const bool was_running = state->mic_running.exchange(false);
  if (state->mic_queue != nullptr) {
    (void)h2_pal_queue_close(state->queue, state->mic_queue);
  }
  PaStream *stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->control_mutex);
    stream = state->input_stream;
  }
  abort_stream(stream, "input");
  if (state->mic_thread_started) {
    state->mic_thread.join();
    state->mic_thread_started = false;
  }
  {
    std::lock_guard<std::mutex> read_lock(state->mic_read_mutex);
    if (state->mic_queue != nullptr) {
      h2_pal_queue_destroy(state->queue, state->mic_queue);
      state->mic_queue = nullptr;
    }
  }
  if (was_running) {
    reset_echo_reference(state);
  }
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (state->input_stream != nullptr) {
    (void)Pa_StopStream(state->input_stream);
    (void)Pa_CloseStream(state->input_stream);
    state->input_stream = nullptr;
    state->input_channels = 0;
  }
  return H2_AUDIO_OK;
}

int audio_start_speaker(void *user) {
  if (user == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  if (state->playback_thread_started && state->playback_running.load()) {
    return H2_AUDIO_OK;
  }
  int result = reap_stopped_playback_thread(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  result = open_output(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  state->playback_result.store(H2_AUDIO_OK);
  state->playback_running.store(true);
  try {
    state->playback_thread = std::thread(playback_main, state);
    state->playback_thread_started = true;
  } catch (...) {
    state->playback_running.store(false);
    (void)close_output_stream(state, true);
    return H2_AUDIO_ERR_IO;
  }
  return H2_AUDIO_OK;
}

int audio_stop_speaker(void *user) {
  if (user == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  state->playback_running.store(false);
  if (state->playback_thread_started) {
    state->playback_thread.join();
    state->playback_thread_started = false;
  }
  const int playback_result = state->playback_result.exchange(H2_AUDIO_OK);
  const int cleanup_result = close_output_stream(
      state, playback_result != H2_AUDIO_OK);
  if (playback_result != H2_AUDIO_OK) {
    return playback_result;
  }
  return cleanup_result;
}

int map_queue_receive(int result) {
  switch (result) {
  case H2_PAL_QUEUE_OK:
    return H2_AUDIO_OK;
  case H2_PAL_QUEUE_ERR_TIMEOUT:
    return H2_AUDIO_ERR_WOULD_BLOCK;
  case H2_PAL_QUEUE_ERR_CLOSED:
    return H2_AUDIO_ERR_INVALID_STATE;
  case H2_PAL_QUEUE_ERR_INVALID_ARG:
    return H2_AUDIO_ERR_INVALID_ARG;
  default:
    return H2_AUDIO_ERR_IO;
  }
}

int audio_mic_read(void *user, h2_audio_frame_t *out_frame,
                   uint32_t timeout_ms) {
  if (user == nullptr || out_frame == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  std::lock_guard<std::mutex> read_lock(state->mic_read_mutex);
  if (!state->mic_running.load() || state->mic_queue == nullptr) {
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  const h2_audio_pcm_format_t format = desktop_format();
  if (out_frame->sample_rate_hz != format.sample_rate_hz ||
      out_frame->channels != format.channels ||
      out_frame->sample_format != format.sample_format) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  if (out_frame->capacity < sizeof(state->mic_scratch)) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  MicQueueFrame item;
  const int result = h2_pal_queue_recv(state->queue,
                                       state->mic_queue, &item, timeout_ms);
  if (result != H2_PAL_QUEUE_OK) {
    return map_queue_receive(result);
  }
  std::memcpy(out_frame->data, item.samples.data(), item.bytes);
  out_frame->bytes = item.bytes;
  out_frame->samples_per_channel = item.samples_per_channel;
  return H2_AUDIO_OK;
}

int audio_create_track(void *user, const h2_audio_track_config_t *config,
                       h2_pal_audio_track_t **out_track) {
  if (user == nullptr || config == nullptr || out_track == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = static_cast<AudioState *>(user);
  const int result = init_audio(state);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  return h2_audio_mixer_create_track(&state->mixer, &state->audio, config,
                                     out_track);
}

int audio_get_volume(void *user, uint32_t *out_percent) {
  if (user == nullptr || out_percent == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_percent = static_cast<AudioState *>(user)->speaker_volume_percent.load();
  return H2_AUDIO_OK;
}

int audio_set_volume(void *user, uint32_t percent) {
  if (user == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  static_cast<AudioState *>(user)->speaker_volume_percent.store(percent);
  return H2_AUDIO_OK;
}

const h2_pal_audio_vtable_t audio_vtable = {
    audio_get_info,      audio_start_mic,    audio_stop_mic,
    audio_start_speaker, audio_stop_speaker, audio_mic_read,
    audio_create_track,  audio_get_volume,   audio_set_volume,
};
} // namespace

extern "C" {

int h2_portaudio_create(const h2_portaudio_config_t *config,
                        h2_portaudio_t **out_provider) {
  if (config == nullptr || out_provider == nullptr ||
      config->allocator == nullptr || config->queue == nullptr ||
      config->sync == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_provider = nullptr;
  h2_portaudio_t *provider = new (std::nothrow) h2_portaudio_t();
  if (provider == nullptr) {
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  provider->state.allocator = config->allocator;
  provider->state.queue = config->queue;
  provider->state.sync = config->sync;
  provider->state.require_real_devices = config->require_real_devices != 0;
  provider->state.audio.user = &provider->state;
  provider->state.audio.vtable = &audio_vtable;
  *out_provider = provider;
  return H2_AUDIO_OK;
}

int h2_portaudio_set_output_test_ops(
    h2_portaudio_t *provider,
    const h2_portaudio_output_test_ops_t *ops) {
  if (provider == nullptr || ops == nullptr || ops->open == nullptr ||
      ops->start == nullptr ||
      ops->write_available == nullptr || ops->write == nullptr ||
      ops->stop == nullptr || ops->abort == nullptr || ops->close == nullptr ||
      ops->error_text == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = &provider->state;
  if (state->output_stream != nullptr || state->playback_thread_started) {
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  state->output_ops = *ops;
  state->output_ops_overridden = true;
  return H2_AUDIO_OK;
}

int h2_portaudio_output_underflow_error_for_test(void) {
  return paOutputUnderflowed;
}

uint64_t
h2_portaudio_output_underflow_count_for_test(h2_portaudio_t *provider) {
  if (provider == nullptr) {
    return 0u;
  }
  return provider->state.output_underflow_count.load();
}

int h2_portaudio_set_echo_test_ops(
    h2_portaudio_t *provider, const h2_portaudio_echo_test_ops_t *ops) {
  if (provider == nullptr || ops == nullptr || ops->reset == nullptr ||
      ops->before_enqueue == nullptr || ops->enqueue_result == nullptr ||
      ops->playback == nullptr || ops->capture == nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  AudioState *state = &provider->state;
  std::lock_guard<std::mutex> lock(state->control_mutex);
  if (state->mic_running.load() || state->playback_running.load()) {
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  state->echo_test_ops = *ops;
  state->echo_test_ops_overridden = true;
  return H2_AUDIO_OK;
}

void h2_portaudio_destroy(h2_portaudio_t *provider) {
  if (provider == nullptr) {
    return;
  }
  AudioState *state = &provider->state;
  (void)audio_stop_mic(state);
  (void)audio_stop_speaker(state);
  if (state->mixer_initialized) {
    h2_audio_mixer_deinit(&state->mixer);
    state->mixer_initialized = false;
  }
  if (state->echo_state != nullptr) {
    speex_echo_state_destroy(state->echo_state);
    state->echo_state = nullptr;
  }
  if (state->portaudio_initialized) {
    (void)Pa_Terminate();
    state->portaudio_initialized = false;
  }
  delete provider;
}

h2_pal_audio_t *h2_portaudio_audio(h2_portaudio_t *provider) {
  return provider == nullptr ? nullptr : &provider->state.audio;
}

} // extern "C"
