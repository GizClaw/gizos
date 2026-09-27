#include "h2_desktop_recording_internal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t kRate = 16000u;
constexpr uint32_t kFps = 30u;
constexpr size_t kPcmCapacity = kRate * 2u;
constexpr size_t kVideoCapacity = 16u;
// Holdback allows the source callbacks to deliver timestamps before sampling.
constexpr uint64_t kHoldbackUs = 100000u;
struct VideoSnapshot {
  std::vector<uint16_t> pixels;
  uint64_t timestamp_us = 0u;
  uint8_t brightness = 255u;
};
} // namespace

struct h2_desktop_recording_encoder {
  uint64_t start_us = 0u;
  uint64_t end_us = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;
  bool finished = false;
  std::thread worker;
  h2_pal_result_t result = H2_PAL_OK;
  h2_desktop_recording_stats_t stats = {};
  std::array<VideoSnapshot, kVideoCapacity> video_queue;
  size_t video_head = 0u;
  size_t video_count = 0u;
  std::vector<uint16_t> current_pixels;
  uint8_t brightness = 255u;
  std::array<int16_t, kPcmCapacity> pcm = {};
  uint64_t audio_cursor = 0u;
  uint64_t audio_end = 0u;
  uint64_t video_cursor = 0u;
  AVFormatContext *format = nullptr;
  AVCodecContext *video = nullptr;
  AVCodecContext *audio = nullptr;
  AVStream *video_stream = nullptr;
  AVStream *audio_stream = nullptr;
  AVFrame *video_frame = nullptr;
  AVFrame *audio_frame = nullptr;
  AVPacket *packet = nullptr;
  SwsContext *scaler = nullptr;
  bool header_written = false;
  int fd = -1;
};

namespace {
void latch(h2_desktop_recording_encoder_t *state, h2_pal_result_t result) {
  if (state->result == H2_PAL_OK) {
    state->result = result;
  }
}

} // namespace

void h2_desktop_recording_encoder_video(void *user,
                                        const h2_sdl3_capture_frame_t *frame) {
  auto *state = static_cast<h2_desktop_recording_encoder_t *>(user);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || state->result != H2_PAL_OK) {
    return;
  }
  if (frame == nullptr || frame->pixels == nullptr ||
      frame->width != state->width || frame->height != state->height ||
      frame->stride_bytes < state->width * sizeof(uint16_t) ||
      frame->timestamp_us < state->start_us) {
    latch(state, H2_PAL_ERR_INVALID_ARG);
    return;
  }
  if (state->video_count == kVideoCapacity) {
    latch(state, H2_PAL_ERR_FULL);
    return;
  }
  const size_t slot = (state->video_head + state->video_count) % kVideoCapacity;
  VideoSnapshot &snapshot = state->video_queue[slot];
  const auto *bytes = reinterpret_cast<const uint8_t *>(frame->pixels);
  for (uint32_t row = 0u; row < state->height; ++row) {
    std::memcpy(snapshot.pixels.data() + row * state->width,
                bytes + row * frame->stride_bytes,
                state->width * sizeof(uint16_t));
  }
  snapshot.timestamp_us = frame->timestamp_us - state->start_us;
  snapshot.brightness = frame->brightness;
  ++state->video_count;
  ++state->stats.captured_video_frames;
  state->wake.notify_one();
}

void h2_desktop_recording_encoder_audio(
    void *user, const h2_portaudio_capture_frame_t *frame) {
  auto *state = static_cast<h2_desktop_recording_encoder_t *>(user);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || state->result != H2_PAL_OK) {
    return;
  }
  if (frame == nullptr || frame->samples == nullptr || frame->frames == 0u ||
      frame->sample_rate != kRate || frame->channels != 1u ||
      frame->timestamp_us < state->start_us || frame->frames > kPcmCapacity) {
    latch(state, H2_PAL_ERR_INVALID_ARG);
    return;
  }
  const uint64_t relative = frame->timestamp_us - state->start_us;
  // Quotient first avoids overflow on long recordings.
  uint64_t at = (relative / 1000000u) * kRate +
                ((relative % 1000000u) * kRate + 500000u) / 1000000u;
  if (at < state->audio_cursor) {
    state->stats.late_audio_frames += frame->frames;
    latch(state, H2_PAL_ERR_TIMEOUT);
    return;
  }
  if (at - state->audio_cursor + frame->frames > kPcmCapacity) {
    latch(state, H2_PAL_ERR_FULL);
    return;
  }
  // Rounding of a split device write can overlap by one sample.
  const size_t skip = static_cast<size_t>(std::min<uint64_t>(
      frame->frames, state->audio_end > at ? state->audio_end - at : 0u));
  for (size_t index = skip; index < frame->frames; ++index) {
    state->pcm[(at + index) % kPcmCapacity] = frame->samples[index];
  }
  state->audio_end = std::max(state->audio_end, at + frame->frames);
  state->stats.captured_audio_frames += frame->frames - skip;
  state->wake.notify_one();
}
namespace {
int write_bytes(void *user, const uint8_t *data, int size) {
  auto *state = static_cast<h2_desktop_recording_encoder_t *>(user);
  int offset = 0;
  while (offset < size) {
    const ssize_t result =
        write(state->fd, data + offset, static_cast<size_t>(size - offset));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return AVERROR(errno == 0 ? EIO : errno);
    }
    offset += static_cast<int>(result);
  }
  return size;
}
int64_t seek_bytes(void *user, int64_t offset, int whence) {
  auto *state = static_cast<h2_desktop_recording_encoder_t *>(user);
  if (whence == AVSEEK_SIZE) {
    return AVERROR(ENOSYS);
  }
  const off_t result = lseek(state->fd, static_cast<off_t>(offset), whence);
  return result < 0 ? AVERROR(errno) : static_cast<int64_t>(result);
}

bool setup_codec(h2_desktop_recording_encoder_t *state, bool is_video) {
  const AVCodec *codec =
      avcodec_find_encoder(is_video ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_AAC);
  if (codec == nullptr) {
    return false;
  }
  AVCodecContext *context = avcodec_alloc_context3(codec);
  AVStream *stream = avformat_new_stream(state->format, nullptr);
  if (is_video) {
    state->video = context;
    state->video_stream = stream;
  } else {
    state->audio = context;
    state->audio_stream = stream;
  }
  if (context == nullptr || stream == nullptr) {
    return false;
  }
  context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (is_video) {
    context->width = static_cast<int>(state->width);
    context->height = static_cast<int>(state->height);
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = {1, static_cast<int>(kFps)};
    context->framerate = {static_cast<int>(kFps), 1};
    context->gop_size = 30;
    context->max_b_frames = 0;
    context->bit_rate = 2000000;
  } else {
    context->sample_rate = static_cast<int>(kRate);
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&context->ch_layout, 1);
    context->time_base = {1, static_cast<int>(kRate)};
    context->bit_rate = 64000;
  }
  if (avcodec_open2(context, codec, nullptr) < 0 ||
      avcodec_parameters_from_context(stream->codecpar, context) < 0) {
    return false;
  }
  stream->time_base = context->time_base;
  return true;
}

bool setup(h2_desktop_recording_encoder_t *state, const char *path) {
  state->fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (state->fd < 0) {
    return false;
  }
  if (avformat_alloc_output_context2(&state->format, nullptr, "mp4", nullptr) <
          0 ||
      state->format == nullptr) {
    return false;
  }
  auto *buffer = static_cast<unsigned char *>(av_malloc(32768u));
  if (buffer == nullptr) {
    return false;
  }
  state->format->pb = avio_alloc_context(buffer, 32768, 1, state, nullptr,
                                         write_bytes, seek_bytes);
  if (state->format->pb == nullptr) {
    av_free(buffer);
    return false;
  }
  state->format->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (!setup_codec(state, true) || !setup_codec(state, false)) {
    return false;
  }
  state->packet = av_packet_alloc();
  state->video_frame = av_frame_alloc();
  state->audio_frame = av_frame_alloc();
  if (state->packet == nullptr || state->video_frame == nullptr ||
      state->audio_frame == nullptr) {
    return false;
  }
  state->video_frame->format = state->video->pix_fmt;
  state->video_frame->width = state->video->width;
  state->video_frame->height = state->video->height;
  state->audio_frame->format = state->audio->sample_fmt;
  state->audio_frame->sample_rate = state->audio->sample_rate;
  state->audio_frame->nb_samples = state->audio->frame_size;
  if (av_channel_layout_copy(&state->audio_frame->ch_layout,
                             &state->audio->ch_layout) < 0 ||
      av_frame_get_buffer(state->video_frame, 0) < 0 ||
      av_frame_get_buffer(state->audio_frame, 0) < 0) {
    return false;
  }
  state->scaler = sws_getContext(state->video->width, state->video->height,
                                 AV_PIX_FMT_RGB565, state->video->width,
                                 state->video->height, state->video->pix_fmt,
                                 SWS_POINT, nullptr, nullptr, nullptr);
  if (state->scaler == nullptr) {
    return false;
  }
  // Fragment once per GOP and omit the optional mfra/tfra index. Without
  // skip_trailer FFmpeg retains a growing fragment index until finalization.
  AVDictionary *options = nullptr;
  const int option_result = av_dict_set(
      &options, "movflags", "frag_keyframe+empty_moov+default_base_moof+skip_trailer", 0);
  state->format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
  const int header_result =
      option_result < 0 ? option_result
                        : avformat_write_header(state->format, &options);
  av_dict_free(&options);
  if (header_result < 0) {
    return false;
  }
  state->header_written = true;
  return true;
}

bool encode(h2_desktop_recording_encoder_t *state, bool video, AVFrame *frame) {
  AVCodecContext *context = video ? state->video : state->audio;
  AVStream *stream = video ? state->video_stream : state->audio_stream;
  if (avcodec_send_frame(context, frame) < 0) {
    return false;
  }
  while (true) {
    const int result = avcodec_receive_packet(context, state->packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
      return true;
    }
    if (result < 0) {
      return false;
    }
    av_packet_rescale_ts(state->packet, context->time_base, stream->time_base);
    state->packet->stream_index = stream->index;
    if (video) {
      state->packet->duration =
          av_rescale_q(1, context->time_base, stream->time_base);
    }
    const int written =
        av_interleaved_write_frame(state->format, state->packet);
    av_packet_unref(state->packet);
    if (written < 0) {
      return false;
    }
  }
}

bool video_step(h2_desktop_recording_encoder_t *state) {
  if (av_frame_make_writable(state->video_frame) < 0) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const uint64_t timestamp = state->video_cursor * 1000000u / kFps;
    while (state->video_count != 0u) {
      VideoSnapshot &snapshot = state->video_queue[state->video_head];
      if (snapshot.timestamp_us > timestamp) {
        break;
      }
      state->current_pixels.swap(snapshot.pixels);
      state->brightness = snapshot.brightness;
      state->video_head = (state->video_head + 1u) % kVideoCapacity;
      --state->video_count;
    }
  }
  // Match SDL's color modulation on the already composited framebuffer.
  const uint8_t *source[4] = {
      reinterpret_cast<const uint8_t *>(state->current_pixels.data()), nullptr,
      nullptr, nullptr};
  const int stride[4] = {static_cast<int>(state->width * 2u), 0, 0, 0};
  if (sws_scale(state->scaler, source, stride, 0, state->video->height,
                state->video_frame->data, state->video_frame->linesize) <= 0) {
    return false;
  }
  // In YUV limited range, black is Y=16, chroma=128.
  if (state->brightness != 255u) {
    for (int plane = 0; plane < 3; ++plane) {
      const int width =
          plane == 0 ? state->video->width : state->video->width / 2;
      const int height =
          plane == 0 ? state->video->height : state->video->height / 2;
      const int base = plane == 0 ? 16 : 128;
      for (int row = 0; row < height; ++row) {
        uint8_t *pixels = state->video_frame->data[plane] +
                          row * state->video_frame->linesize[plane];
        for (int col = 0; col < width; ++col) {
          pixels[col] = static_cast<uint8_t>(
              base +
              (static_cast<int>(pixels[col]) - base) * state->brightness / 255);
        }
      }
    }
  }
  state->video_frame->pts = static_cast<int64_t>(state->video_cursor++);
  return encode(state, true, state->video_frame);
}

bool audio_step(h2_desktop_recording_encoder_t *state) {
  if (av_frame_make_writable(state->audio_frame) < 0) {
    return false;
  }
  auto *samples = reinterpret_cast<float *>(state->audio_frame->data[0]);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->audio_frame->pts = static_cast<int64_t>(state->audio_cursor);
    for (int index = 0; index < state->audio_frame->nb_samples; ++index) {
      const size_t slot = state->audio_cursor++ % kPcmCapacity;
      samples[index] = state->pcm[slot] / 32768.0f;
      state->pcm[slot] = 0;
    }
  }
  return encode(state, false, state->audio_frame);
}

void run(h2_desktop_recording_encoder_t *state) {
  bool failed = false;
  while (!failed) {
    uint64_t target_us = 0u;
    bool stopping = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      const uint64_t now = h2_desktop_recording_now_us();
      if (now < state->start_us) {
        latch(state, H2_PAL_ERR_IO);
        break;
      }
      stopping = state->stopping;
      target_us = stopping ? state->end_us
                           : (now - state->start_us > kHoldbackUs
                                  ? now - state->start_us - kHoldbackUs
                                  : 0u);
    }
    const uint64_t video_us = state->video_cursor * 1000000u / kFps;
    const uint64_t audio_us = state->audio_cursor * 1000000u / kRate;
    const bool video_due = video_us < target_us;
    const bool audio_due =
        stopping
            ? audio_us < target_us
            : audio_us + static_cast<uint64_t>(state->audio_frame->nb_samples) *
                             1000000u / kRate <=
                  target_us;
    if (video_due || audio_due) {
      failed = video_due && (!audio_due || video_us <= audio_us)
                   ? !video_step(state)
                   : !audio_step(state);
      continue;
    }
    if (stopping) {
      break;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->wake.wait_for(lock, std::chrono::milliseconds(5));
  }
  bool final_ok = encode(state, true, nullptr);
  final_ok = encode(state, false, nullptr) && final_ok;
  final_ok = av_write_trailer(state->format) >= 0 && final_ok;
  avio_flush(state->format->pb);
  final_ok = state->format->pb->error >= 0 && final_ok;
  if (state->fd >= 0) {
    final_ok = close(state->fd) == 0 && final_ok;
    state->fd = -1;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (failed || !final_ok) {
    latch(state, H2_PAL_ERR_IO);
  }
  state->stats.video_frames = state->video_cursor;
}

void release(h2_desktop_recording_encoder_t *state) {
  sws_freeContext(state->scaler);
  av_packet_free(&state->packet);
  av_frame_free(&state->video_frame);
  av_frame_free(&state->audio_frame);
  avcodec_free_context(&state->video);
  avcodec_free_context(&state->audio);
  if (state->format != nullptr) {
    if (state->format->pb != nullptr) {
      av_freep(&state->format->pb->buffer);
      avio_context_free(&state->format->pb);
    }
    avformat_free_context(state->format);
  }
  if (state->fd >= 0) {
    (void)close(state->fd);
  }
  delete state;
}
} // namespace

uint64_t h2_desktop_recording_now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

h2_pal_result_t h2_desktop_recording_encoder_create(
    const h2_desktop_recording_encoder_config_t *config,
    h2_desktop_recording_encoder_t **out_recording) {
  if (out_recording == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_recording = nullptr;
  if (config == nullptr || config->path == nullptr || config->path[0] == '\0' ||
      config->width == 0u || config->height == 0u || config->width > 4096u ||
      config->height > 4096u || config->width % 2u != 0u ||
      config->height % 2u != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  auto *state = new (std::nothrow) h2_desktop_recording_encoder_t();
  if (state == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  state->width = config->width;
  state->height = config->height;
  state->start_us = config->start_us;
  try {
    state->current_pixels.resize(state->width * state->height);
    for (VideoSnapshot &snapshot : state->video_queue) {
      snapshot.pixels.resize(state->width * state->height);
    }
    state->stats.buffer_bytes = (kVideoCapacity + 1u) * state->width *
                                    state->height * sizeof(uint16_t) +
                                sizeof(state->pcm);
    if (!setup(state, config->path)) {
      release(state);
      return H2_PAL_ERR_IO;
    }
    state->worker = std::thread(run, state);
  } catch (...) {
    release(state);
    return H2_PAL_ERR_NO_MEMORY;
  }
  *out_recording = state;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_desktop_recording_encoder_finish(h2_desktop_recording_encoder_t *state,
                                    uint64_t stop_us,
                                    h2_desktop_recording_stats_t *out_stats) {
  if (out_stats != nullptr) {
    *out_stats = {};
  }
  if (state == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!state->finished) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      uint64_t now = stop_us;
      if (now < state->start_us) {
        latch(state, H2_PAL_ERR_IO);
        now = state->start_us;
      }
      state->end_us =
          std::max(now - state->start_us,
                   (state->audio_end * 1000000u + kRate - 1u) / kRate);
      if (state->video_count != 0u) {
        const size_t last =
            (state->video_head + state->video_count - 1u) % kVideoCapacity;
        const uint64_t timestamp = state->video_queue[last].timestamp_us;
        // A flush just after the previous sample boundary must still appear
        // in the final encoded frame, even if the app stops immediately.
        const uint64_t frame_index =
            timestamp / 1000000u * kFps +
            (timestamp % 1000000u * kFps + 999999u) / 1000000u;
        state->end_us =
            std::max(state->end_us, frame_index * 1000000u / kFps + 1u);
      }
      if (state->stats.captured_video_frames == 0u) {
        latch(state, H2_PAL_ERR_UNAVAILABLE);
      }
      state->end_us = std::max<uint64_t>(state->end_us, 1u);
      state->stopping = true;
      state->wake.notify_one();
    }
    state->worker.join();
    state->finished = true;
    state->stats.duration_us = state->end_us;
    state->stats.result = state->result;
  }
  if (out_stats != nullptr) {
    *out_stats = state->stats;
  }
  return state->result;
}

void h2_desktop_recording_encoder_destroy(
    h2_desktop_recording_encoder_t *state) {
  if (state != nullptr) {
    (void)h2_desktop_recording_encoder_finish(
        state, h2_desktop_recording_now_us(), nullptr);
    release(state);
  }
}
