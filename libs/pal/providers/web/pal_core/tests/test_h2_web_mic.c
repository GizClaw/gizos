#include "h2_web_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>

static h2_web_platform_t *platform;
static const h2_pal_audio_api_t *audio;
static int read_result;
static int ticks;

EMSCRIPTEN_KEEPALIVE void stop_pending_start(void) {
  h2_web_platform_destroy(platform); // suspended start pins the platform
  assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
}

static void read_until_stopped(void *user) {
  (void)user;
  int16_t samples[320];
  h2_audio_frame_t frame = {.data = samples, .capacity = sizeof(samples)};
  read_result = h2_pal_audio_mic_read(audio, &frame, 1000u);
  assert(frame.bytes == 0u);
}

static void stop_reader(void *user) {
  (void)user;
  assert(h2_pal_time_sleep_ms(h2_web_platform_time_api(platform), 10u) == H2_PAL_OK);
  int16_t samples[320];
  h2_audio_frame_t frame = {.data = samples, .capacity = sizeof(samples)};
  assert(h2_pal_audio_mic_read(audio, &frame, 0u) == H2_PAL_ERR_BUSY);
  h2_web_platform_destroy(platform); // pending read must pin its platform
  ++ticks;
  assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
}

static void read_pcm(void) {
  int16_t samples[320];
  h2_audio_frame_t frame = {.data = samples, .capacity = sizeof(samples)};
  const double deadline = emscripten_get_now() + 5000.0;
  int nonzero = 0;
  do {
    assert(h2_pal_audio_mic_read(audio, &frame, 1000u) == H2_PAL_OK);
    assert(frame.bytes == 640u && frame.samples_per_channel == 320u &&
           frame.sample_rate_hz == 16000u && frame.channels == 1u &&
           frame.sample_format == H2_AUDIO_SAMPLE_S16LE);
    for (size_t i = 0; i < 320u; ++i) nonzero |= samples[i] != 0;
  } while (!nonzero && emscripten_get_now() < deadline);
  // Devices may produce startup silence; require real signal within a bound.
  assert(nonzero);
}

static int restarted;
static void restart_from_task(void *user) {
  (void)user;
  assert(h2_pal_audio_start_mic(audio) == H2_PAL_OK);
  read_pcm();
  assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
  restarted = 1;
}

// clang-format off
int main(void) {
  platform = h2_web_platform_create(&(h2_web_platform_config_t){
      .display_width = 1, .display_height = 1});
  assert(platform);
  audio = h2_web_platform_audio_api(platform);
  h2_audio_info_t info;
  assert(h2_pal_audio_get_info(audio, &info) == H2_PAL_OK && info.mic_supported);
  assert(info.mic_format.sample_rate_hz == 16000u && info.mic_queue_frames == 8u);
  assert(h2_pal_audio_start_mic(audio) == H2_PAL_OK);
  assert(h2_pal_audio_start_mic(audio) == H2_PAL_ERR_INVALID_STATE);
  read_pcm();
  puts("WEB_MIC first-pcm=PASS");
  assert(EM_ASM_INT({
    const echo = Module.h2WebMicrophoneStreams.get($0).getAudioTracks()[0]
      .getSettings().echoCancellation;
    return echo === true || echo === 'all' || echo === 'remote-only';
  }, (uintptr_t)platform));
  puts("WEB_MIC echoCancellation=enabled PASS");
  if (!EM_ASM_INT({ return !!Module.h2MicFake; })) {
    // Borrow the exact capture stream through the current WebRTC track contract.
    EM_ASM({ Module.h2WebRtcTracks = new Map([[7, {
      stream: Module.h2WebMicrophoneStreams.get($0)
    }]]); }, (uintptr_t)platform);
    const h2_pal_webrtc_api_t *rtc = h2_web_platform_webrtc_api(platform);
    h2_pal_webrtc_peer_t *peer = NULL;
    h2_pal_webrtc_track_t track = {.native_handle = (void *)(uintptr_t)7};
    assert(h2_pal_webrtc_peer_create(rtc, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_set_track(rtc, peer, &track) == H2_PAL_OK);
    assert(EM_ASM_INT({
      return Module.h2WebRtcPeers.get($0).pc.getSenders()[0].track ===
        Module.h2WebMicrophoneStreams.get($1).getAudioTracks()[0];
    }, (uintptr_t)peer, (uintptr_t)platform));
    puts("WEB_MIC WebRTC=set-track");
    assert(h2_pal_webrtc_peer_unset_track(rtc, peer, &track) == H2_PAL_OK);
    assert(EM_ASM_INT({
      return Module.h2WebMicrophoneStreams.get($0).getAudioTracks()[0].readyState === 'live';
    }, (uintptr_t)platform));
    h2_pal_webrtc_peer_close(rtc, peer);
    EM_ASM({ Module.h2WebRtcTracks.clear(); });
    puts("WEB_MIC WebRTC=shared-stream unset=still-live PASS");
  }
  // The bounded pool covers queued messages as well as pending worklet output.
  emscripten_sleep(250u);
  assert(EM_ASM_INT({ return Module.h2WebMicrophones.get($0).queue.length <= 8; },
                    (uintptr_t)platform));
  int16_t sample;
  h2_audio_frame_t small = {.data = &sample, .capacity = sizeof(sample), .bytes = 9};
  assert(h2_pal_audio_mic_read(audio, &small, 0u) == H2_PAL_ERR_INVALID_ARG && small.bytes == 0);

  puts("WEB_MIC bounded=PASS");
  if (EM_ASM_INT({ return !!Module.h2MicFake; })) {
    // Saturate the real worklet pool, then exercise defensive overflow delivery.
    assert(EM_ASM_INT({
      const entry = Module.h2WebMicrophones.get($0);
      for (let i = 0; i < 32; ++i)
        entry.node.processor.process([[new Float32Array(320).fill(0.25)]]);
      if (entry.queue.length !== 8 || entry.status !== 0) return 0;
      entry.node.port.onmessage({data: new ArrayBuffer(640)});
      return entry.queue.length === 8 && entry.status === 0 &&
        entry.stream.getAudioTracks()[0].readyState === 'live';
    }, (uintptr_t)platform));
    read_pcm();
  }
  // Stop source delivery to test real timeout and cooperative stop wakeup.
  EM_ASM({ Module.h2WebMicrophones.get($0).source.disconnect(); }, (uintptr_t)platform);
  emscripten_sleep(30u);
  int16_t samples[320];
  h2_audio_frame_t frame = {.data = samples, .capacity = sizeof(samples)};
  int result;
  do { result = h2_pal_audio_mic_read(audio, &frame, 0u); } while (result == H2_PAL_OK);
  puts("WEB_MIC queue=empty");
  assert(result == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_audio_mic_read(audio, &frame, 5u) == H2_PAL_ERR_TIMEOUT);
  h2_pal_task_t *reader = NULL, *stopper = NULL;
  const h2_pal_task_api_t *tasks = h2_web_platform_task_api(platform);
  assert(h2_pal_task_start(tasks, NULL, read_until_stopped, NULL, &reader) == H2_PAL_OK);
  assert(h2_pal_task_start(tasks, NULL, stop_reader, NULL, &stopper) == H2_PAL_OK);
  for (int i = 0; i < 1000 && !ticks; ++i) {
    assert(h2_web_platform_pump(platform, 16u, NULL) == H2_PAL_OK);
    emscripten_sleep(1u);
  }
  for (int i = 0; i < 10; ++i) {
    assert(h2_web_platform_pump(platform, 16u, NULL) == H2_PAL_OK);
    emscripten_sleep(1u);
  }
  puts("WEB_MIC tasks=returned");
  assert(ticks == 1 && read_result == H2_PAL_ERR_CLOSED);
  assert(h2_pal_task_join(tasks, reader) == H2_PAL_OK);
  assert(h2_pal_task_join(tasks, stopper) == H2_PAL_OK);
  assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(audio, &frame, 0u) == H2_PAL_ERR_CLOSED);
  assert(EM_ASM_INT({ return !Module.h2WebMicrophoneStreams.has($0); }, (uintptr_t)platform));
  h2_pal_task_t *restart = NULL;
  assert(h2_pal_task_start(tasks, NULL, restart_from_task, NULL, &restart) == H2_PAL_OK);
  for (int i = 0; i < 6000 && !restarted; ++i) {
    assert(h2_web_platform_pump(platform, 16u, NULL) == H2_PAL_OK);
    emscripten_sleep(1u);
  }
  assert(restarted && h2_pal_task_join(tasks, restart) == H2_PAL_OK);

  if (EM_ASM_INT({ return !!Module.h2MicFake; })) {
    assert(EM_ASM_INT({ return Module.h2MicConstraints.audio.echoCancellation === true; }));
    // A preference is not mandatory: unsupported AEC still permits capture.
    EM_ASM({ Module.h2MicEchoDisabled = true; });
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_OK);
    read_pcm();
    assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
    EM_ASM({ Module.h2MicEchoDisabled = false; Module.h2MicDenied = true; });
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_ERR_UNAVAILABLE);
    EM_ASM({ Module.h2MicDenied = false; Module.h2MicPending = true;
      setTimeout(() => { Module._stop_pending_start(); Module.h2MicResolve(); }, 5);
    });
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_ERR_CLOSED);
    emscripten_sleep(5u);
    assert(EM_ASM_INT({ return Module.h2MicLastStream.getTracks()[0].readyState === 'ended'; }));
    EM_ASM({ Module.h2MicPending = false; });
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_OK);
    EM_ASM({ Module.h2MicLastStream.getTracks()[0].end(); });
    assert(h2_pal_audio_mic_read(audio, &frame, 0u) == H2_PAL_ERR_CLOSED);
    assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_OK);
    EM_ASM({ Module.h2WebMicrophones.get($0).node.onprocessorerror(); }, (uintptr_t)platform);
    assert(h2_pal_audio_mic_read(audio, &frame, 0u) == H2_PAL_ERR_IO);
    assert(h2_pal_audio_stop_mic(audio) == H2_PAL_OK);
    EM_ASM({ globalThis.AudioWorkletNode = undefined; });
    assert(h2_pal_audio_get_info(audio, &info) == H2_PAL_OK && !info.mic_supported);
    assert(h2_pal_audio_start_mic(audio) == H2_PAL_ERR_UNSUPPORTED);
  }
  puts("WEB_MIC destroying");
  h2_web_platform_destroy(platform);
  assert(EM_ASM_INT({ return !Module.h2WebMicrophones.has($0) && !Module.h2WebAudioPlatforms.has($0); }, (uintptr_t)platform));
  puts("WEB_MIC PCM=16000/mono/S16LE frames=320 bounded=8 timeout=PASS stop-wakeup=PASS restart=PASS");
  // Cooperative Fiber tests explicitly terminate after all owners are released.
  emscripten_force_exit(0);
  return 0;
}
// clang-format on
