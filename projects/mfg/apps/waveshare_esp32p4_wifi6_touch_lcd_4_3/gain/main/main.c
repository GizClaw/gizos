#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The existing single-board probe, at its original level and five times
 * that level. This measures far-end echo and digital headroom only.
 * Human near-end/double-talk acceptance remains in the MFG AEC check. */
static const int16_t tone[32] = {
    0,     2631,  4280,  4478,  3496,  2147,  1292,  1344,  2048,  2673, 2492,
    1259,  -600,  -2202, -2712, -1832, 0,     1832,  2712,  2202,  600,  -1259,
    -2492, -2673, -2048, -1344, -1292, -2147, -3496, -4478, -4280, -2631};
enum { SAMPLES = 512, PEAK_LIMIT = 30000 };

typedef struct capture_meter {
  uint64_t sequence, end_us, energy[4], reference_energy, output_energy;
  uint32_t skip, frames, peak[4], clipped[4];
  uint32_t reference_peak, reference_clipped, output_peak, output_clipped;
  int result;
} capture_meter_t;

typedef struct playback_meter {
  uint64_t end_us;
  uint32_t skip, frames, active, peak, clipped;
  int result;
} playback_meter_t;

static h2_runtime_t* runtime;
static const h2_pal_audio_t* audio;
static capture_meter_t capture_meter;
static playback_meter_t playback_meter;
static uint64_t run_id;
static uint8_t device_mac[6];
static unsigned measured;
static int retained;

static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static int valid(const h2_audio_frame_t* frame, unsigned channels) {
  return frame != NULL && frame->data != NULL &&
         frame->sample_rate_hz == 16000 &&
         frame->sample_format == H2_AUDIO_SAMPLE_S16LE &&
         frame->samples_per_channel == SAMPLES && frame->channels == channels &&
         frame->bytes == SAMPLES * channels * sizeof(int16_t) &&
         frame->capacity >= frame->bytes;
}

static void sample(int16_t value, uint32_t* peak, uint64_t* energy,
                   uint32_t* clipped) {
  int32_t signed_value = value;
  uint32_t magnitude =
      (uint32_t)(signed_value < 0 ? -signed_value : signed_value);
  if (magnitude > *peak) *peak = magnitude;
  if (energy != NULL) *energy += (uint64_t)magnitude * magnitude;
  *clipped += magnitude >= PEAK_LIMIT;
}

static void capture(void* user, const h2_audio_aec_frame_t* frame) {
  (void)user;
  capture_meter_t* meter = &capture_meter;
  if (frame == NULL || !valid(frame->raw, 4) ||
      !valid(frame->reference_input, 1) || !valid(frame->processed, 1) ||
      frame->microphone_lane != 1 || frame->microphone_mask != 10 ||
      frame->reference_lane != 0 || frame->sequence == 0 ||
      (meter->sequence != 0 && frame->sequence != meter->sequence + 1)) {
    meter->result = H2_PAL_ERR_FORMAT;
    return;
  }
  meter->sequence = frame->sequence;
  if (meter->skip != 0) {
    --meter->skip;
    return;
  }
  if (meter->frames == measured) return;
  const int16_t* raw = frame->raw->data;
  const int16_t* reference = frame->reference_input->data;
  const int16_t* output = frame->processed->data;
  for (unsigned i = 0; i < SAMPLES; ++i) {
    for (unsigned lane = 0; lane < 4; ++lane)
      sample(raw[i * 4 + lane], &meter->peak[lane], &meter->energy[lane],
             &meter->clipped[lane]);
    sample(reference[i], &meter->reference_peak, &meter->reference_energy,
           &meter->reference_clipped);
    sample(output[i], &meter->output_peak, &meter->output_energy,
           &meter->output_clipped);
  }
  if (++meter->frames == measured)
    meter->end_us = (uint64_t)esp_timer_get_time();
}

static void playback(void* user, const h2_audio_frame_t* frame) {
  (void)user;
  playback_meter_t* meter = &playback_meter;
  if (!valid(frame, 1)) {
    meter->result = H2_PAL_ERR_FORMAT;
    return;
  }
  if (meter->skip != 0) {
    --meter->skip;
    return;
  }
  if (meter->frames == measured) return;
  const int16_t* pcm = frame->data;
  int active = 0;
  for (unsigned i = 0; i < SAMPLES; ++i) {
    sample(pcm[i], &meter->peak, NULL, &meter->clipped);
    active |= pcm[i] != 0;
  }
  meter->active += active;
  if (++meter->frames == measured)
    meter->end_us = (uint64_t)esp_timer_get_time();
}

static void observation_error(void* user, int is_capture, int result) {
  (void)user;
  int* out = is_capture ? &capture_meter.result : &playback_meter.result;
  *out = result == 0 ? H2_PAL_ERR_IO : result;
}

static void remember(int* first, int result) {
  if (*first == 0 && result != 0) *first = result;
}

static int window(unsigned candidate, unsigned index, unsigned speaker,
                  unsigned mic, unsigned scale) {
  unsigned warmup = speaker == 0 ? 32 : 96;
  measured = 96;
  memset(&capture_meter, 0, sizeof(capture_meter));
  memset(&playback_meter, 0, sizeof(playback_meter));
  capture_meter.skip = playback_meter.skip = warmup;
  uint32_t actual_speaker = 0, actual_mic = 0;
  int result = h2_pal_audio_set_speaker_volume_percent(audio, speaker);
  if (result == 0) result = h2_pal_audio_set_mic_gain_percent(audio, mic);
  if (result == 0)
    result = h2_pal_audio_get_speaker_volume_percent(audio, &actual_speaker);
  if (result == 0)
    result = h2_pal_audio_get_mic_gain_percent(audio, &actual_mic);
  if (result == 0 && (actual_speaker != speaker || actual_mic != mic))
    result = H2_PAL_ERR_IO;
  const h2_audio_aec_observer_t observer = {.on_capture = capture,
                                            .on_playback = playback,
                                            .on_error = observation_error};
  int registered = 0, mic_started = 0, speaker_started = 0;
  if (result == 0) {
    result = h2_pal_audio_set_aec_observer(audio, &observer);
    registered = result == 0;
  }
  uint64_t start_us = (uint64_t)esp_timer_get_time();
  if (result == 0) {
    result = h2_pal_audio_start_mic(audio);
    mic_started = result == 0;
  }
  if (result == 0) {
    result = h2_pal_audio_start_speaker(audio);
    speaker_started = result == 0;
  }
  const h2_audio_pcm_format_t format = {.sample_rate_hz = 16000,
                                        .frame_samples_per_channel = SAMPLES,
                                        .channels = 1,
                                        .sample_format = H2_AUDIO_SAMPLE_S16LE};
  const h2_audio_track_config_t config = {.name = "mfg/gain",
                                          .format = format,
                                          .volume_factor_milli = 1000,
                                          .buffer_frames = 4,
                                          .allocator = runtime->mem};
  h2_pal_audio_track_t* track = NULL;
  if (result == 0) result = h2_pal_audio_create_track(audio, &config, &track);
  int16_t tx_data[SAMPLES], rx_data[SAMPLES];
  for (unsigned i = 0; i < SAMPLES; ++i)
    tx_data[i] = speaker == 0 ? 0 : (int16_t)(tone[i % 32] * scale);
  uint64_t nominal_us = (uint64_t)(warmup + measured) * 32000;
  for (unsigned i = 0; result == 0 && i < warmup + measured; ++i) {
    h2_audio_frame_t tx =
        h2_audio_frame_for_buffer(tx_data, sizeof(tx_data), format);
    h2_audio_frame_t rx =
        h2_audio_frame_for_buffer(rx_data, sizeof(rx_data), format);
    tx.bytes = sizeof(tx_data);
    result = h2_pal_audio_track_write(track, &tx, 1000);
    if (result == 0) result = h2_pal_audio_mic_read(audio, &rx, 1000);
    if (result == 0 && !valid(&rx, 1)) result = H2_PAL_ERR_FORMAT;
    if ((uint64_t)esp_timer_get_time() - start_us > nominal_us * 5 / 4 + 100000)
      remember(&result, H2_PAL_ERR_TIMEOUT);
  }
  if (result == 0) result = h2_pal_audio_track_drain(track, 1000);
  int cleanup = 0;
  if (mic_started) remember(&cleanup, h2_pal_audio_stop_mic(audio));
  if (track != NULL) {
    int closed = h2_pal_audio_track_close(track);
    remember(&cleanup, closed);
    if (closed == 0) track = NULL;
  }
  if (speaker_started && track == NULL)
    remember(&cleanup, h2_pal_audio_stop_speaker(audio));
  if (cleanup == 0 && registered)
    remember(&cleanup, h2_pal_audio_set_aec_observer(audio, NULL));
  /* Keep callback state and its dependencies alive on cleanup failure. */
  if (cleanup != 0) retained = 1;
  remember(&result, cleanup);
  if (cleanup == 0) {
    remember(&result, capture_meter.result);
    remember(&result, playback_meter.result);
    uint64_t end_us = capture_meter.end_us > playback_meter.end_us
                          ? capture_meter.end_us
                          : playback_meter.end_us;
    uint64_t elapsed = end_us >= start_us ? end_us - start_us : 0;
    if (capture_meter.frames != measured || playback_meter.frames != measured ||
        playback_meter.active != (speaker == 0 ? 0 : measured) ||
        elapsed + 1000 < nominal_us || elapsed > nominal_us * 5 / 4 + 100000)
      remember(&result, H2_PAL_ERR_INVALID_STATE);
    /* UART bridges can lose early boot output while their baud rate settles.
     * Emit the authoritative run identity beside the first measured window,
     * after warmup, so the host can validate the complete raw receipt. */
    if (candidate == 0 && index == 0) {
      printf(
          "H2_MFG_GAIN_BOOT board=waveshare_esp32p4_wifi6_touch_lcd_4_3"
          " uid=%02x%02x%02x%02x%02x%02x version=%s run=%016" PRIx64
          " codec_max=bf ref_gain=16000 mic_mask=10 ref_lane=0"
          " probe_peak=4478,22390 scope=single-board-far-echo\n",
          device_mac[0], device_mac[1], device_mac[2], device_mac[3],
          device_mac[4], device_mac[5], esp_app_get_description()->version,
          run_id);
    }
    printf(
        "H2_MFG_GAIN_POINT {\"run\":\"%016" PRIx64
        "\",\"candidate\":%u,\"index\":%u,\"scale\":%u,"
        "\"speaker\":%u,\"mic\":%u,\"actual_speaker\":%" PRIu32
        ",\"actual_mic\":%" PRIu32
        ",\"frames\":%u,\"dac_frames\":%u,\"active\":%u,"
        "\"warmup\":%u,\"measured\":%u,\"elapsed_us\":%" PRIu64
        ",\"peak\":[%u,%u,%u,%u],\"energy\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64
        "],\"clipped\":[%u,%u,%u,%u],"
        "\"reference_peak\":%u,\"reference_energy\":%" PRIu64
        ",\"reference_clipped\":%u,\"output_peak\":%u,"
        "\"output_energy\":%" PRIu64
        ",\"output_clipped\":%u,"
        "\"dac_peak\":%u,\"dac_clipped\":%u,\"rc\":%d,\"cleanup\":%d}\n",
        run_id, candidate, index, scale, speaker, mic, actual_speaker,
        actual_mic, (unsigned)capture_meter.frames,
        (unsigned)playback_meter.frames, (unsigned)playback_meter.active,
        warmup, measured, elapsed, (unsigned)capture_meter.peak[0],
        (unsigned)capture_meter.peak[1], (unsigned)capture_meter.peak[2],
        (unsigned)capture_meter.peak[3], capture_meter.energy[0],
        capture_meter.energy[1], capture_meter.energy[2],
        capture_meter.energy[3], (unsigned)capture_meter.clipped[0],
        (unsigned)capture_meter.clipped[1], (unsigned)capture_meter.clipped[2],
        (unsigned)capture_meter.clipped[3],
        (unsigned)capture_meter.reference_peak, capture_meter.reference_energy,
        (unsigned)capture_meter.reference_clipped,
        (unsigned)capture_meter.output_peak, capture_meter.output_energy,
        (unsigned)capture_meter.output_clipped, (unsigned)playback_meter.peak,
        (unsigned)playback_meter.clipped, result, cleanup);
  } else {
    printf("H2_MFG_GAIN_CLEANUP_FAIL run=%016" PRIx64 " rc=%d retained=1\n",
           run_id, cleanup);
  }
  fflush(stdout);
  return result;
}

static int run(void) {
  uint32_t saved_speaker = 0, saved_mic = 0;
  int result = h2_pal_audio_get_speaker_volume_percent(audio, &saved_speaker);
  if (result == 0)
    result = h2_pal_audio_get_mic_gain_percent(audio, &saved_mic);
  unsigned completed = 0;
  const unsigned speakers[] = {93, 97, 100};
  const unsigned microphones[] = {63, 79, 95, 100};
  for (unsigned s = 0; result == 0 && s < 3; ++s) {
    for (unsigned m = 0; result == 0 && m < 4; ++m) {
      unsigned candidate = s * 4 + m;
      for (unsigned index = 0; result == 0 && index < 4; ++index) {
        unsigned speaker = index == 0 || index == 3 ? 0 : speakers[s];
        unsigned scale = index == 2 ? 5 : 1;
        result = window(candidate, index, speaker, microphones[m], scale);
        if (result == 0) ++completed;
      }
    }
  }
  int cleanup = 0;
  if (!retained) {
    remember(&cleanup,
             h2_pal_audio_set_speaker_volume_percent(audio, saved_speaker));
    remember(&cleanup, h2_pal_audio_set_mic_gain_percent(audio, saved_mic));
  } else {
    cleanup = H2_PAL_ERR_INVALID_STATE;
  }
  remember(&result, cleanup);
  printf("H2_MFG_GAIN_DONE run=%016" PRIx64
         " windows=%u rc=%d cleanup=%d"
         " acoustic_qualified=0 controls_persisted=0\n",
         run_id, completed, result, cleanup);
  fflush(stdout);
  return result;
}

static int before_startup(void* user, h2_loader_t* loader,
                          const h2_pal_sync_api_t* sync,
                          h2_pal_mutex_t* operation_mutex,
                          const h2_pal_sync_api_t* wifi_sync,
                          h2_pal_mutex_t* wifi_mutex) {
  (void)user;
  (void)wifi_sync;
  (void)wifi_mutex;
  int result = h2_pal_mutex_lock(sync, operation_mutex);
  if (result != 0) return result;
  h2_loader_status_t status;
  result = h2_loader_read_status(loader, &status);
  int recovery =
      result == 0 &&
      (status.running_partition_id != loader->config.h2loader_partition_id ||
       (status.stage.valid &&
        status.stage.role == H2_LOADER_IMAGE_ROLE_H2LOADER));
  /* Finish managed Loader replacement before taking ownership of audio.
   * An explicit second Loader boot starts measurement with empty Stage. */
  if (result != 0 || recovery) {
    int unlock = h2_pal_mutex_unlock(sync, operation_mutex);
    return result == 0 ? unlock : result;
  }
  if (result == 0 && esp_efuse_mac_get_default(device_mac) != ESP_OK)
    result = H2_PAL_ERR_IO;
  run_id = ((uint64_t)esp_random() << 32) | esp_random();
  printf("H2_MFG_GAIN_START run=%016" PRIx64 "\n", run_id);
  if (result == 0) result = run();
  int unlock = h2_pal_mutex_unlock(sync, operation_mutex);
  return result == 0 ? unlock : result;
}

static void entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config = {0};
  int result = h2_esp_board_runtime_config(&config);
  audio = config.audio;
  if (result == 0) result = h2_runtime_init(&config, &runtime);
  if (result == 0) {
    const h2_esp_h2loader_config_t loader = {
        .board = "waveshare_esp32p4_wifi6_touch_lcd_4_3",
        .mfg_required_total = 10,
        .run_mfg = before_startup,
    };
    h2_esp_h2loader_run_with_config(runtime, &loader);
  } else {
    printf("H2_MFG_GAIN_SETUP_FAIL rc=%d\n", result);
  }
  hold();
}

void app_main(void) {
  int result = h2_esp_target_task_policy_install();
  if (result == 0)
    result = h2_esp_board_start_entry_task("mfg/gain/entry", entry, NULL);
  if (result != 0) printf("H2_MFG_GAIN_SETUP_FAIL rc=%d\n", result);
}
