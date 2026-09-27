#include "h2_ffmpeg_recording.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
std::atomic<uint64_t> clock_us{1000000u};
h2_pal_result_t now(void *, uint64_t *out) {
  *out = clock_us.load();
  return H2_PAL_OK;
}
const h2_pal_time_vtable_t kTimeVtable = {nullptr, now,     nullptr,
                                          nullptr, nullptr, nullptr};
const h2_pal_time_api_t kTime = {nullptr, &kTimeVtable};
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
} // namespace

int main() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  const std::string path = std::string(temporary) + "/recording.mp4";
  h2_ffmpeg_recording_config_t config = {path.c_str(), &kTime, 64u, 48u};
  h2_ffmpeg_recording_t *state = nullptr;
  config.width = 63u;
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_ERR_INVALID_ARG);
  assert(state == nullptr);
  config.width = 64u;
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_OK);
  const h2_media_capture_api_t *capture = h2_ffmpeg_recording_capture(state);
  std::vector<uint16_t> pixels(64u * 48u, 0xf800u);
  h2_media_capture_video_t video = {pixels.data(), 64u,  48u,
                                    128u,          255u, 1000000u};
  capture->vtable->video(capture->user, &video);
  pixels.assign(pixels.size(), 0x07e0u);
  video.timestamp_us = 2200000u;
  capture->vtable->video(capture->user, &video);
  std::vector<int16_t> samples(1600u, 12000);
  const h2_media_capture_audio_t audio = {samples.data(), samples.size(),
                                          16000u, 1u, 2200000u};
  capture->vtable->audio(capture->user, &audio);
  // Stop before the scheduled DAC tail: it must still retain all accepted PCM.
  clock_us = 2250000u;
  h2_ffmpeg_recording_stats_t stats = {};
  assert(h2_ffmpeg_recording_finish(state, &stats) == H2_PAL_OK);
  assert(stats.duration_us == 1300000u && stats.video_frames == 39u);
  assert(stats.captured_audio_frames == 1600u);
  assert(stats.late_audio_frames == 0u && stats.captured_video_frames == 2u);
  assert(h2_ffmpeg_recording_finish(state, nullptr) == H2_PAL_OK);
  h2_ffmpeg_recording_destroy(state);
  inspect(path.c_str());
  state = nullptr;
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_ERR_IO);
  assert(state == nullptr); // never overwrite an existing artifact
  config.path = "/nonexistent-directory/h2-recording.mp4";
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_ERR_IO);
  const std::string overflow_path = std::string(temporary) + "/overflow.mp4";
  config.path = overflow_path.c_str();
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_OK);
  capture = h2_ffmpeg_recording_capture(state);
  h2_media_capture_audio_t future = audio;
  future.timestamp_us = clock_us.load() + 3000000u;
  capture->vtable->audio(capture->user, &future);
  assert(h2_ffmpeg_recording_finish(state, &stats) == H2_PAL_ERR_FULL);
  assert(stats.result == H2_PAL_ERR_FULL);
  h2_ffmpeg_recording_destroy(state);
  const std::string tail_path = std::string(temporary) + "/tail-frame.mp4";
  config.path = tail_path.c_str();
  clock_us = 4000000u;
  assert(h2_ffmpeg_recording_create(&config, &state) == H2_PAL_OK);
  capture = h2_ffmpeg_recording_capture(state);
  video.timestamp_us = 4001000u;
  capture->vtable->video(capture->user, &video);
  clock_us = 4001001u;
  assert(h2_ffmpeg_recording_finish(state, &stats) == H2_PAL_OK);
  assert(stats.video_frames == 2u && stats.duration_us >= 33334u);
  h2_ffmpeg_recording_destroy(state);
  h2_ffmpeg_recording_destroy(nullptr);
  return 0;
}
