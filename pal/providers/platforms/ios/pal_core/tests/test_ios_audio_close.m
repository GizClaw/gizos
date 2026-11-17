/* Compile the actual iOS provider with deterministic native Reset/wait faults.
 * No device is opened: a pending output queue is injected below. Runtime's
 * actual Audio proxy still owns and consumes the two allocated track handles. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#import "h2_ios_platform.h"
#include "h2_ios_audio_wait.h"
#include "h2_pal_unsupported.h"
#include "h2_runtime.h"

#import <AudioToolbox/AudioToolbox.h>
#import <XCTest/XCTest.h>
#include <assert.h>

static OSStatus reset_result;
static int wait_result;
static unsigned resets, waits;

static OSStatus injected_reset(AudioQueueRef queue) {
  assert(queue != NULL);
  ++resets;
  return reset_result;
}

static int injected_wait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                         const h2_ios_audio_wait_budget_t *budget) {
  (void)condition;
  (void)mutex;
  (void)budget;
  ++waits;
  return wait_result;
}

#define AudioQueueReset injected_reset
#define h2_ios_audio_wait_changed injected_wait
#define h2_ios_audio_create fixture_audio_create
#define h2_ios_audio_api fixture_audio_api
#define h2_ios_audio_destroy fixture_audio_destroy
#include "../src/h2_ios_audio.m"
#undef h2_ios_audio_destroy
#undef h2_ios_audio_api
#undef h2_ios_audio_create
#undef h2_ios_audio_wait_changed
#undef AudioQueueReset

static unsigned live_allocations;

static void *counted_allocate(void *user, size_t bytes) {
  (void)user;
  void *allocation = malloc(bytes);
  if (allocation) ++live_allocations;
  return allocation;
}

static void counted_release(void *user, void *allocation) {
  (void)user;
  if (allocation) {
    assert(live_allocations > 0u);
    --live_allocations;
    free(allocation);
  }
}

static const h2_pal_mem_vtable_t memory_vtable = {.alloc = counted_allocate,
                                                  .free = counted_release};
static const h2_pal_mem_api_t memory_api = {NULL, &memory_vtable};

static h2_runtime_config_t runtime_config(h2_ios_audio_t *audio) {
  h2_runtime_config_t config = {
      .board = "ios-audio-lifecycle-test",
      .target = "ios",
      .chip = "simulator",
      .mem = &memory_api,
      .log = h2_ios_platform_log_api(),
      .time = h2_ios_platform_time_api(),
      .timer = h2_ios_platform_timer_api(),
      .task = h2_ios_platform_task_api(),
      .queue = h2_ios_platform_queue_api(),
      .sync = h2_ios_platform_sync_api(),
      .audio = fixture_audio_api(audio),
  };
#define UNSUPPORTED(field) config.field = h2_pal_unsupported_##field##_api()
  UNSUPPORTED(firmware_info);
  UNSUPPORTED(fs);
  UNSUPPORTED(disk);
  UNSUPPORTED(pref);
  UNSUPPORTED(crypto);
  UNSUPPORTED(http);
  UNSUPPORTED(net);
  UNSUPPORTED(netif);
  UNSUPPORTED(mqtt);
  UNSUPPORTED(webrtc);
  UNSUPPORTED(wifi_sta);
  UNSUPPORTED(wifi_ap);
  UNSUPPORTED(wifi_csi);
  UNSUPPORTED(wifi_settings);
  UNSUPPORTED(ble_host);
  UNSUPPORTED(modem);
  UNSUPPORTED(power);
  UNSUPPORTED(display);
  UNSUPPORTED(audio_decoder);
  UNSUPPORTED(periph);
  UNSUPPORTED(button);
  UNSUPPORTED(touch);
  UNSUPPORTED(buzzer);
  UNSUPPORTED(nfc);
  UNSUPPORTED(nfc_card_emulation);
  UNSUPPORTED(imu);
  UNSUPPORTED(gpio_irq);
  UNSUPPORTED(led);
  UNSUPPORTED(input);
  UNSUPPORTED(system_event);
  UNSUPPORTED(video_decoder);
#undef UNSUPPORTED
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  return config;
}

@interface H2IOSAudioCloseTests : XCTestCase
@end

@implementation H2IOSAudioCloseTests

- (void)testDrainFailureThenConsumingCloseThroughRuntime {
  const int faults[] = {H2_AUDIO_OK, H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_IO, H2_AUDIO_OK};
  for (unsigned i = 0u; i < sizeof(faults) / sizeof(faults[0]); ++i) {
    resets = waits = live_allocations = 0u;
    reset_result = i >= 2u ? (OSStatus)-50 : noErr;
    wait_result = faults[i];
    h2_ios_audio_t *audio = NULL;
    XCTAssertEqual(fixture_audio_create(&audio), H2_AUDIO_OK);
    /* Stalled output without opening AudioQueue or accessing a speaker. */
    audio->speaker_started = 1;
    audio->output_queue = (AudioQueueRef)audio;
    audio->output_pending = faults[i] == H2_AUDIO_OK ? 0u : 1u;
    h2_runtime_t *runtime = NULL;
    const h2_runtime_config_t config = runtime_config(audio);
    XCTAssertEqual(h2_runtime_init(&config, &runtime), H2_PAL_OK);
    const unsigned before_track = live_allocations;
    const h2_audio_track_config_t track_config = {.name = "ios-close-failure",
                                                  .format = {16000, 512, 1, H2_AUDIO_SAMPLE_S16LE},
                                                  .volume_factor_milli = 1000,
                                                  .buffer_frames = 2,
                                                  .allocator = &memory_api};
    h2_pal_audio_track_t *track = NULL;
    XCTAssertEqual(h2_pal_audio_create_track(runtime->audio, &track_config, &track), H2_AUDIO_OK);
    XCTAssertNotEqual(track, &audio->track->base); /* Real Runtime wrapper. */
    XCTAssertEqual(live_allocations, before_track + 2u);
    XCTAssertEqual(h2_pal_audio_track_drain(track, 0u), faults[i]);
    const unsigned drain_waits = waits;
    const int close_result = faults[i] != H2_AUDIO_OK ? faults[i]
                             : reset_result != noErr  ? H2_PAL_ERR_IO
                                                      : H2_AUDIO_OK;
    XCTAssertEqual(h2_pal_audio_track_close(track), close_result);
    XCTAssertEqual(waits, drain_waits + (faults[i] == H2_AUDIO_OK ? 0u : 1u));
    XCTAssertEqual(resets, 1u);
    XCTAssertEqual(audio->track, NULL);
    XCTAssertEqual(live_allocations, before_track); /* Backend + wrapper freed. */
    /* New tracks can be created without a stale backend blocking lifecycle. */
    XCTAssertEqual(h2_pal_audio_create_track(runtime->audio, &track_config, &track), H2_AUDIO_OK);
    reset_result = noErr;
    audio->output_pending = 0u; /* Device recovers before the next track ends. */
    XCTAssertEqual(h2_pal_audio_track_close(track), H2_AUDIO_OK);
    XCTAssertEqual(live_allocations, before_track);
    /* The injected queue has no native allocation to dispose. The real
     * stop path must now accept the owner because close detached its track. */
    audio->output_queue = NULL;
    XCTAssertEqual(h2_pal_audio_stop_speaker(runtime->audio), H2_AUDIO_OK);
    XCTAssertEqual(h2_runtime_destroy(&runtime), H2_PAL_OK);
    XCTAssertEqual(live_allocations, 0u);
    fixture_audio_destroy(audio);
  }
}

@end
