#include "h2_desktop_recording_internal.h"
#include "h2_desktop_recording_output.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
void check_fragment_layout(const char *path) {
  FILE *file = std::fopen(path, "rb");
  assert(file != nullptr);
  unsigned fragments = 0u;
  unsigned char header[8];
  while (std::fread(header, 1u, sizeof(header), file) == sizeof(header)) {
    const uint32_t size = (static_cast<uint32_t>(header[0]) << 24u) |
                          (static_cast<uint32_t>(header[1]) << 16u) |
                          (static_cast<uint32_t>(header[2]) << 8u) | header[3];
    assert(size >= sizeof(header));
    assert(std::memcmp(header + 4u, "mfra", 4u) != 0);
    if (std::memcmp(header + 4u, "moof", 4u) == 0) {
      ++fragments;
    }
    assert(std::fseek(file, static_cast<long>(size - sizeof(header)),
                      SEEK_CUR) == 0);
  }
  assert(std::feof(file) != 0 && fragments >= 2u);
  assert(std::fclose(file) == 0);
}

void inspect(const char *path) {
  AVFormatContext *format = nullptr;
  assert(avformat_open_input(&format, path, nullptr, nullptr) == 0);
  assert(avformat_find_stream_info(format, nullptr) >= 0);
  assert(format->nb_streams == 2u);
  int video = -1, audio = -1;
  for (unsigned index = 0; index < format->nb_streams; ++index) {
    const AVCodecParameters *codec = format->streams[index]->codecpar;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      video = static_cast<int>(index);
      assert(codec->width == 64 && codec->height == 48);
      assert(codec->codec_id == AV_CODEC_ID_MPEG4);
    } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio = static_cast<int>(index);
      assert(codec->sample_rate == 16000 && codec->ch_layout.nb_channels == 1);
      assert(codec->codec_id == AV_CODEC_ID_AAC);
    }
  }
  assert(video >= 0 && audio >= 0);
  AVPacket *packet = av_packet_alloc();
  assert(packet != nullptr);
  int counts[2] = {0, 0};
  int64_t last[2] = {INT64_MIN, INT64_MIN};
  double end[2] = {0., 0.};
  while (av_read_frame(format, packet) >= 0) {
    const int index = packet->stream_index;
    assert(index >= 0 && index < 2);
    assert(packet->dts > last[index]);
    last[index] = packet->dts;
    ++counts[index];
    end[index] = (packet->pts + packet->duration) *
                 av_q2d(format->streams[index]->time_base);
    av_packet_unref(packet);
  }
  assert(counts[video] >= 39 && counts[audio] >= 21);
  assert(end[video] >= 1.3 && end[audio] >= 1.3);
  assert(std::abs(end[video] - end[audio]) < 0.07);
  av_packet_free(&packet);
  avformat_close_input(&format);
}

struct AudioWindow {
  double start;
  double end;
  float minimum_peak;
  float maximum_peak;
  float peak = 0.0f;
  size_t samples = 0u;
};

void inspect_audio(const char *path, std::vector<AudioWindow> windows) {
  AVFormatContext *format = nullptr;
  assert(avformat_open_input(&format, path, nullptr, nullptr) == 0);
  assert(avformat_find_stream_info(format, nullptr) >= 0);
  const AVCodec *codec = nullptr;
  const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                        &codec, 0);
  assert(stream >= 0 && codec != nullptr);
  AVCodecContext *decoder = avcodec_alloc_context3(codec);
  assert(decoder != nullptr);
  assert(avcodec_parameters_to_context(decoder,
                                      format->streams[stream]->codecpar) == 0);
  assert(avcodec_open2(decoder, codec, nullptr) == 0);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  assert(packet != nullptr && frame != nullptr);
  auto drain = [&]() {
    int rc;
    while ((rc = avcodec_receive_frame(decoder, frame)) == 0) {
      assert(frame->format == AV_SAMPLE_FMT_FLTP && frame->ch_layout.nb_channels == 1);
      const auto *pcm = reinterpret_cast<const float *>(frame->extended_data[0]);
      const double start = frame->pts * av_q2d(format->streams[stream]->time_base);
      for (int i = 0; i < frame->nb_samples; ++i) {
        const double at = start + static_cast<double>(i) / frame->sample_rate;
        for (auto &window : windows) {
          if (at >= window.start && at < window.end) {
            window.peak = std::max(window.peak, std::abs(pcm[i]));
            ++window.samples;
          }
        }
      }
    }
    assert(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF);
  };
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == stream) {
      assert(avcodec_send_packet(decoder, packet) == 0);
      drain();
    }
    av_packet_unref(packet);
  }
  assert(avcodec_send_packet(decoder, nullptr) == 0);
  drain();
  for (const auto &window : windows) {
    const size_t minimum_samples =
        static_cast<size_t>((window.end - window.start) * 16000.0 * 0.9);
    assert(window.samples >= minimum_samples);
    assert(window.peak >= window.minimum_peak &&
           window.peak < window.maximum_peak);
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  avformat_close_input(&format);
}

void inspect_video_colors(const char *path) {
  AVFormatContext *format = nullptr;
  assert(avformat_open_input(&format, path, nullptr, nullptr) == 0);
  assert(avformat_find_stream_info(format, nullptr) >= 0);
  const AVCodec *codec = nullptr;
  const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                        &codec, 0);
  assert(stream >= 0 && codec != nullptr);
  AVCodecContext *decoder = avcodec_alloc_context3(codec);
  assert(decoder != nullptr);
  assert(avcodec_parameters_to_context(decoder,
                                      format->streams[stream]->codecpar) == 0);
  assert(avcodec_open2(decoder, codec, nullptr) == 0);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  assert(packet != nullptr && frame != nullptr);
  size_t frames = 0u;
  bool saw_red = false;
  int last_y = 0, last_u = 0, last_v = 0;
  auto drain = [&]() {
    int rc;
    while ((rc = avcodec_receive_frame(decoder, frame)) == 0) {
      assert(frame->format == AV_PIX_FMT_YUV420P);
      last_y = frame->data[0][0];
      last_u = frame->data[1][0];
      last_v = frame->data[2][0];
      ++frames;
      saw_red = saw_red || (last_y < 120 && last_v > 180);
    }
    assert(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF);
  };
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == stream) {
      assert(avcodec_send_packet(decoder, packet) == 0);
      drain();
    }
    av_packet_unref(packet);
  }
  assert(avcodec_send_packet(decoder, nullptr) == 0);
  drain();
  assert(saw_red && frames > 1u &&
         last_y > 100 && last_u < 100 && last_v < 100); // green tail
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  avformat_close_input(&format);
}
// Fill a real pipe so the next POSIX write blocks until a reader starts.
size_t fill_pipe(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  const uint8_t padding[4096] = {};
  size_t count = 0u;
  while (true) {
    const ssize_t size = write(fd, padding, sizeof(padding));
    if (size < 0) {
      assert(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
    assert(size > 0);
    count += static_cast<size_t>(size);
  }
  assert(fcntl(fd, F_SETFL, flags) == 0);
  return count;
}

void delayed_output(const char *temporary) {
  int pipe_fd[2];
  assert(pipe(pipe_fd) == 0);
  size_t padding = fill_pipe(pipe_fd[1]);
  const std::string path = std::string(temporary) + "/delayed-output.mp4";
  h2_desktop_recording_encoder_config_t config = {
      path.c_str(), 64u, 48u, h2_desktop_recording_now_us(), pipe_fd[1]};
  h2_desktop_recording_encoder_t *state = nullptr;
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  assert(close(pipe_fd[1]) == 0); // encoder owns its duplicate
  std::vector<uint16_t> pixels(64u * 48u);
  std::vector<int16_t> samples(1600u, 12000);
  for (unsigned i = 0u; i < 32u; ++i) {
    pixels.assign(pixels.size(), i < 16u ? 0xf800u : 0x07e0u);
    const uint64_t now = h2_desktop_recording_now_us();
    h2_sdl3_capture_frame_t video = {pixels.data(), 64u, 48u, 128u, 255u, now};
    h2_portaudio_capture_frame_t audio = {
        samples.data(), samples.size(), 16000u, 1u, now + 200000u};
    h2_desktop_recording_encoder_video(state, &video);
    h2_desktop_recording_encoder_audio(state, &audio);
    assert(h2_desktop_recording_encoder_result(state) == H2_PAL_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // More than the entire two-second PCM capacity arrived while write() was
  // blocked. Recover the actual sequential MP4 bytes and decode both tracks.
  std::thread reader([&] {
    FILE *file = std::fopen(path.c_str(), "wb");
    assert(file != nullptr);
    uint8_t bytes[4096];
    ssize_t count;
    while ((count = read(pipe_fd[0], bytes, sizeof(bytes))) > 0) {
      const size_t skip = std::min(padding, static_cast<size_t>(count));
      padding -= skip;
      const size_t size = static_cast<size_t>(count) - skip;
      assert(std::fwrite(bytes + skip, 1u, size, file) == size);
    }
    assert(count == 0 && padding == 0u);
    assert(std::fclose(file) == 0 && close(pipe_fd[0]) == 0);
  });
  h2_desktop_recording_stats_t stats = {};
  assert(h2_desktop_recording_encoder_finish(
             state, h2_desktop_recording_now_us(), &stats) == H2_PAL_OK);
  reader.join();
  assert(stats.captured_audio_frames == 32u * samples.size());
  assert(stats.captured_video_frames == 32u && stats.late_audio_frames == 0u);
  h2_desktop_recording_encoder_destroy(state);
  inspect(path.c_str());
  check_fragment_layout(path.c_str());
  inspect_video_colors(path.c_str());
  inspect_audio(path.c_str(), {{1.3, 1.8, 0.1f, 1.0f},
                              {2.3, 2.8, 0.1f, 1.0f}});
}

void output_boundaries() {
  int pipe_fd[2];
  assert(pipe(pipe_fd) == 0);
  size_t padding = fill_pipe(pipe_fd[1]);
  h2_desktop_recording_output output;
  assert(output.start(pipe_fd[1]));
  std::vector<uint8_t> bytes(h2_desktop_recording_output::capacity);
  for (size_t i = 0u; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>(i * 37u + i / 65536u);
  assert(output.append(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()));
  assert(output.pending() == bytes.size());
  assert(output.append(bytes.data(), 1u) == -ENOBUFS);
  std::vector<uint8_t> received;
  std::thread reader([&] {
    uint8_t block[4096];
    ssize_t count;
    while ((count = read(pipe_fd[0], block, sizeof(block))) > 0) {
      const size_t skip = std::min(padding, static_cast<size_t>(count));
      padding -= skip;
      received.insert(received.end(), block + skip, block + count);
    }
    assert(count == 0 && padding == 0u);
    assert(close(pipe_fd[0]) == 0);
  });
  // Repeated sub-capacity writes force ring wrap while short pipe writes drain.
  for (unsigned i = 0u; i < 5u; ++i) {
    int rc;
    const size_t size = bytes.size() / 3u;
    while ((rc = output.append(bytes.data(), size)) == -ENOBUFS)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(rc == static_cast<int>(size));
  }
  assert(output.finish() == 0 && output.finish() == 0);
  reader.join();
  std::vector<uint8_t> expected = bytes;
  for (unsigned i = 0u; i < 5u; ++i)
    expected.insert(expected.end(), bytes.begin(), bytes.begin() + bytes.size() / 3u);
  assert(received == expected);

  assert(pipe(pipe_fd) == 0);
  assert(close(pipe_fd[0]) == 0);
  h2_desktop_recording_output broken;
  assert(broken.start(pipe_fd[1]));
  assert(broken.append(bytes.data(), 1u) == 1);
  assert(broken.finish() == -EPIPE);
  assert(broken.append(bytes.data(), 1u) == -EPIPE);
}

void failed_output(const char *temporary) {
  int pipe_fd[2];
  assert(pipe(pipe_fd) == 0);
  const std::string path = std::string(temporary) + "/broken-output.mp4";
  h2_desktop_recording_encoder_config_t config = {
      path.c_str(), 64u, 48u, h2_desktop_recording_now_us(), pipe_fd[1]};
  h2_desktop_recording_encoder_t *state = nullptr;
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  assert(close(pipe_fd[0]) == 0 && close(pipe_fd[1]) == 0);
  std::vector<uint16_t> pixels(64u * 48u, 0xf800u);
  h2_sdl3_capture_frame_t video = {
      pixels.data(), 64u, 48u, 128u, 255u, config.start_us};
  h2_desktop_recording_encoder_video(state, &video);
  const uint64_t deadline = h2_desktop_recording_now_us() + 3000000u;
  while (h2_desktop_recording_encoder_result(state) == H2_PAL_OK &&
         h2_desktop_recording_now_us() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(h2_desktop_recording_encoder_result(state) == H2_PAL_ERR_IO);
  assert(h2_desktop_recording_encoder_finish(
             state, h2_desktop_recording_now_us(), nullptr) == H2_PAL_ERR_IO);
  h2_desktop_recording_encoder_destroy(state);
}
} // namespace

int main() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  assert(std::signal(SIGPIPE, SIG_DFL) != SIG_ERR);
  output_boundaries();
  delayed_output(temporary);
  failed_output(temporary);
  struct sigaction action = {};
  assert(sigaction(SIGPIPE, nullptr, &action) == 0);
  assert(action.sa_handler == SIG_DFL);
  const std::string path = std::string(temporary) + "/recording.mp4";
  const uint64_t start = h2_desktop_recording_now_us();
  h2_desktop_recording_encoder_config_t config = {path.c_str(), 64u, 48u,
                                                  start};
  h2_desktop_recording_encoder_t *state = nullptr;
  config.width = 63u;
  assert(h2_desktop_recording_encoder_create(&config, &state) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(state == nullptr);
  config.width = 64u;
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  std::vector<uint16_t> pixels(64u * 48u, 0xf800u);
  h2_sdl3_capture_frame_t video = {pixels.data(), 64u, 48u, 128u, 255u, start};
  h2_desktop_recording_encoder_video(state, &video);
  pixels.assign(pixels.size(), 0x07e0u);
  video.timestamp_us = start + 1200000u;
  h2_desktop_recording_encoder_video(state, &video);
  std::vector<int16_t> samples(1600u, 12000);
  const h2_portaudio_capture_frame_t audio = {samples.data(), samples.size(),
                                              16000u, 1u, start + 1200000u};
  h2_desktop_recording_encoder_audio(state, &audio);
  // Stop before the scheduled DAC tail: it must still retain all accepted PCM.
  uint64_t stop_us = start + 1250000u;
  h2_desktop_recording_stats_t stats = {};
  assert(h2_desktop_recording_encoder_finish(state, stop_us, &stats) ==
         H2_PAL_OK);
  assert(stats.duration_us == 1300000u && stats.video_frames == 39u);
  assert(stats.captured_audio_frames == 1600u);
  assert(stats.late_audio_frames == 0u && stats.captured_video_frames == 2u);
  assert(h2_desktop_recording_encoder_finish(state, stop_us, nullptr) ==
         H2_PAL_OK);
  h2_desktop_recording_encoder_destroy(state);
  inspect(path.c_str());
  check_fragment_layout(path.c_str());
  state = nullptr;
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_ERR_IO);
  assert(state == nullptr); // never overwrite an existing artifact
  // A scheduling gap is silence, not queued PCM. A future chunk must fit in
  // an empty bounded queue even when its timestamp is more than two seconds
  // beyond the encoder cursor.
  const std::string gap_path = std::string(temporary) + "/audio-gap.mp4";
  config.path = gap_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  video.timestamp_us = config.start_us;
  h2_desktop_recording_encoder_video(state, &video);
  h2_portaudio_capture_frame_t after_gap = audio;
  after_gap.timestamp_us = config.start_us + 3000000u;
  h2_desktop_recording_encoder_audio(state, &after_gap);
  assert(h2_desktop_recording_encoder_finish(state, config.start_us + 3100000u,
                                             &stats) == H2_PAL_OK);
  assert(stats.captured_audio_frames == samples.size());
  assert(stats.duration_us == 3100000u && stats.late_audio_frames == 0u);
  h2_desktop_recording_encoder_destroy(state);
  inspect(gap_path.c_str());
  inspect_audio(gap_path.c_str(), {{1.0, 2.5, 0.0f, 0.001f},
                                  {3.0, 3.2, 0.1f, 1.0f}});
  config.path = "/nonexistent-directory/h2-recording.mp4";
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_ERR_IO);
  const std::string overflow_path = std::string(temporary) + "/overflow.mp4";
  config.path = overflow_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  std::vector<int16_t> full_pcm(32000u, 12000);
  h2_portaudio_capture_frame_t future = audio;
  future.samples = full_pcm.data();
  future.frames = full_pcm.size();
  future.timestamp_us = config.start_us + 3000000u;
  h2_desktop_recording_encoder_audio(state, &future);
  future.samples = samples.data();
  future.frames = samples.size();
  future.timestamp_us += 2000000u;
  h2_desktop_recording_encoder_audio(state, &future);
  assert(h2_desktop_recording_encoder_finish(state, config.start_us, &stats) ==
         H2_PAL_ERR_FULL);
  assert(stats.result == H2_PAL_ERR_FULL);
  h2_desktop_recording_encoder_destroy(state);
  const std::string wrap_path = std::string(temporary) + "/audio-wrap.mp4";
  config.path = wrap_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  video.timestamp_us = config.start_us;
  h2_desktop_recording_encoder_video(state, &video);
  future.samples = full_pcm.data();
  future.frames = full_pcm.size();
  future.timestamp_us = config.start_us + 1000000u;
  h2_desktop_recording_encoder_audio(state, &future);
  // Let the worker release storage, then reuse slots across the queue wrap.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  future.frames = 6400u;
  future.timestamp_us = h2_desktop_recording_now_us() + 3000000u;
  h2_desktop_recording_encoder_audio(state, &future);
  assert(h2_desktop_recording_encoder_finish(state, future.timestamp_us + 400000u,
                                             &stats) == H2_PAL_OK);
  assert(stats.captured_audio_frames == 38400u && stats.late_audio_frames == 0u);
  h2_desktop_recording_encoder_destroy(state);
  inspect(wrap_path.c_str());
  const double wrap_start =
      static_cast<double>(future.timestamp_us - config.start_us) / 1000000.0;
  // Inspect inside each payload window, away from AAC priming/boundary padding.
  // Counted captures alone cannot detect silence replacing reused queue slots.
  inspect_audio(wrap_path.c_str(),
                {{1.2, 2.8, 0.1f, 1.0f},
                 {3.2, wrap_start, 0.0f, 0.001f},
                 {wrap_start + 0.1, wrap_start + 0.3, 0.1f, 1.0f}});
  const std::string tail_path = std::string(temporary) + "/tail-frame.mp4";
  config.path = tail_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  video.timestamp_us = config.start_us + 1000u;
  h2_desktop_recording_encoder_video(state, &video);
  stop_us = config.start_us + 1001u;
  assert(h2_desktop_recording_encoder_finish(state, stop_us, &stats) ==
         H2_PAL_OK);
  assert(stats.video_frames == 2u && stats.duration_us >= 33334u);
  h2_desktop_recording_encoder_destroy(state);
  // Many presentations in one 30 fps sampling interval require one slot, not
  // one retained framebuffer each. Decode the movie to check first/last color.
  const std::string burst_path = std::string(temporary) + "/video-burst.mp4";
  config.path = burst_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  pixels.assign(pixels.size(), 0xf800u);
  video.timestamp_us = config.start_us;
  h2_desktop_recording_encoder_video(state, &video);
  for (uint64_t i = 1u; i <= 100u; ++i) {
    pixels.assign(pixels.size(), i == 100u ? 0x07e0u : 0x001fu);
    video.timestamp_us = config.start_us + 10000000u + i;
    h2_desktop_recording_encoder_video(state, &video);
  }
  assert(h2_desktop_recording_encoder_finish(state, config.start_us + 10200000u,
                                             &stats) == H2_PAL_OK);
  assert(stats.captured_video_frames == 101u);
  h2_desktop_recording_encoder_destroy(state);
  inspect_video_colors(burst_path.c_str());

  const std::string backlog_path = std::string(temporary) + "/video-backlog.mp4";
  config.path = backlog_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  pixels.assign(pixels.size(), 0xf800u);
  video.timestamp_us = config.start_us;
  h2_desktop_recording_encoder_video(state, &video);
  // Retain a full second of distinct 50 Hz presentations while the encoder
  // has not reached them yet; this exceeds the former 16-frame capacity.
  for (uint64_t i = 0u; i < 50u; ++i) {
    pixels.assign(pixels.size(), i == 49u ? 0x07e0u : 0x001fu);
    video.timestamp_us = config.start_us + 10000000u + i * 20000u;
    h2_desktop_recording_encoder_video(state, &video);
  }
  assert(h2_desktop_recording_encoder_finish(state, config.start_us + 11200000u,
                                             &stats) == H2_PAL_OK);
  h2_desktop_recording_encoder_destroy(state);
  inspect_video_colors(backlog_path.c_str());

  const std::string full_path = std::string(temporary) + "/video-full.mp4";
  config.path = full_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  for (uint64_t i = 0u; i < 65u; ++i) {
    video.timestamp_us = config.start_us + 10000000u + i * 33334u;
    h2_desktop_recording_encoder_video(state, &video);
  }
  assert(h2_desktop_recording_encoder_result(state) == H2_PAL_ERR_FULL);
  // A late caller must not extend an already failed recording to its stop time.
  assert(h2_desktop_recording_encoder_finish(state, config.start_us + 60000000u,
                                             &stats) == H2_PAL_ERR_FULL);
  assert(stats.duration_us < 13000000u && stats.video_frames < 390u);
  h2_desktop_recording_encoder_destroy(state);

  const std::string failed_path = std::string(temporary) + "/failed-end.mp4";
  config.path = failed_path.c_str();
  config.start_us = h2_desktop_recording_now_us();
  assert(h2_desktop_recording_encoder_create(&config, &state) == H2_PAL_OK);
  video.timestamp_us = config.start_us;
  h2_desktop_recording_encoder_video(state, &video);
  h2_sdl3_capture_frame_t invalid = video;
  invalid.width = 63u;
  h2_desktop_recording_encoder_video(state, &invalid);
  assert(h2_desktop_recording_encoder_result(state) == H2_PAL_ERR_INVALID_ARG);
  const uint64_t before_wait = h2_desktop_recording_now_us();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  assert(h2_desktop_recording_encoder_finish(state, h2_desktop_recording_now_us(),
                                             &stats) == H2_PAL_ERR_INVALID_ARG);
  const uint64_t limit = before_wait - config.start_us + 100000u;
  assert(stats.duration_us < limit);
  assert(stats.video_frames * 1000000u / 30u < limit + 33334u);
  h2_desktop_recording_encoder_destroy(state);

  assert(h2_desktop_recording_result(nullptr) == H2_PAL_ERR_INVALID_ARG);
  h2_desktop_recording_encoder_destroy(nullptr);
  return 0;
}
