#include "h2_desktop_recording_internal.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cassert>
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

void inspect_audio_gap(const char *path) {
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
  float gap_peak = 0.0f, tail_peak = 0.0f;
  size_t gap_samples = 0u;
  auto drain = [&]() {
    int rc;
    while ((rc = avcodec_receive_frame(decoder, frame)) == 0) {
      assert(frame->format == AV_SAMPLE_FMT_FLTP && frame->ch_layout.nb_channels == 1);
      const auto *pcm = reinterpret_cast<const float *>(frame->extended_data[0]);
      const double start = frame->pts * av_q2d(format->streams[stream]->time_base);
      for (int i = 0; i < frame->nb_samples; ++i) {
        const double at = start + static_cast<double>(i) / frame->sample_rate;
        if (at >= 1.0 && at < 2.5) {
          gap_peak = std::max(gap_peak, std::abs(pcm[i]));
          ++gap_samples;
        }
        if (at >= 3.0 && at < 3.2)
          tail_peak = std::max(tail_peak, std::abs(pcm[i]));
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
  assert(gap_samples >= 16000u && gap_peak < 0.001f);
  assert(tail_peak > 0.1f);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  avformat_close_input(&format);
}
} // namespace

int main() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
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
  inspect_audio_gap(gap_path.c_str());
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
  h2_desktop_recording_encoder_destroy(nullptr);
  return 0;
}
