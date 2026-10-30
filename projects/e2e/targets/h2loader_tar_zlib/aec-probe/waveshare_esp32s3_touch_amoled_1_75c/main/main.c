#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_ws175c_audio_diagnostics.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The H200 single-board probe used these same 500/1000/2500-Hz tones.
 * This is a far-end echo/headroom diagnostic, independent of the complete
 * near-end/double-talk AEC Calibration App. It never grants that qualification.
 */
static const int16_t tone[32] = {
    0,     2631,  4280,  4478,  3496,  2147,  1292,  1344,  2048,  2673, 2492,
    1259,  -600,  -2202, -2712, -1832, 0,     1832,  2712,  2202,  600,  -1259,
    -2492, -2673, -2048, -1344, -1292, -2147, -3496, -4478, -4280, -2631};

enum { SAMPLES = 512, WINDOWS = 10, PROFILES = 4, PEAK_LIMIT = 30000 };
typedef struct meter {
  uint64_t sequence, capture_end_us, playback_end_us;
  uint32_t capture_skip, playback_skip, measured;
  uint32_t frames, playback_frames, active_frames;
  uint8_t mic_lane, mic_mask, ref_lane;
  uint32_t peak[4], reference_peak, output_peak, playback_peak;
  uint64_t energy[4], reference_energy, output_energy;
  uint64_t clipped[4], reference_clipped, output_clipped, playback_clipped;
  int capture_rc, playback_rc;
} meter_t;

static meter_t meter;
static h2_runtime_t* runtime;
static const h2_pal_audio_t* provider_audio;
static uint64_t run_id;
static char uid[13];
static bool retained;

static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000u));
}

static bool valid(const h2_audio_frame_t* f, uint8_t channels) {
  return f != NULL && f->data != NULL && f->sample_rate_hz == 16000u &&
         f->sample_format == H2_AUDIO_SAMPLE_S16LE &&
         f->samples_per_channel == SAMPLES && f->channels == channels &&
         f->bytes == SAMPLES * channels * sizeof(int16_t);
}

static void sample(int16_t value, uint32_t* peak, uint64_t* energy,
                   uint64_t* clipped) {
  const int32_t signed_value = value;
  const uint32_t magnitude =
      (uint32_t)(signed_value < 0 ? -signed_value : signed_value);
  if (magnitude > *peak) *peak = magnitude;
  if (energy != NULL) *energy += (uint64_t)magnitude * magnitude;
  *clipped += magnitude >= PEAK_LIMIT;
}

static void capture(void* user, const h2_audio_aec_frame_t* f) {
  meter_t* m = user;
  if (f == NULL || !valid(f->raw, 4u) || !valid(f->reference_input, 1u) ||
      !valid(f->processed, 1u) || f->sequence <= m->sequence ||
      f->microphone_lane >= 4u || f->reference_lane >= 4u ||
      (f->microphone_mask & (1u << f->reference_lane)) != 0u) {
    m->capture_rc = H2_PAL_ERR_FORMAT;
    return;
  }
  m->sequence = f->sequence;
  if (m->capture_skip != 0u) {
    --m->capture_skip;
    return;
  }
  if (m->frames == m->measured) return;
  m->mic_lane = f->microphone_lane;
  m->mic_mask = f->microphone_mask;
  m->ref_lane = f->reference_lane;
  const int16_t* raw = f->raw->data;
  const int16_t* reference = f->reference_input->data;
  const int16_t* output = f->processed->data;
  for (size_t i = 0u; i < SAMPLES; ++i) {
    for (size_t lane = 0u; lane < 4u; ++lane)
      sample(raw[4u * i + lane], &m->peak[lane], &m->energy[lane],
             &m->clipped[lane]);
    sample(reference[i], &m->reference_peak, &m->reference_energy,
           &m->reference_clipped);
    sample(output[i], &m->output_peak, &m->output_energy, &m->output_clipped);
  }
  ++m->frames;
  m->capture_end_us = (uint64_t)esp_timer_get_time();
}

static void playback(void* user, const h2_audio_frame_t* frame) {
  meter_t* m = user;
  if (!valid(frame, 1u)) {
    m->playback_rc = H2_PAL_ERR_FORMAT;
    return;
  }
  if (m->playback_skip != 0u) {
    --m->playback_skip;
    return;
  }
  if (m->playback_frames == m->measured) return;
  const int16_t* pcm = frame->data;
  bool active = false;
  for (size_t i = 0u; i < SAMPLES; ++i) {
    sample(pcm[i], &m->playback_peak, NULL, &m->playback_clipped);
    active |= pcm[i] != 0;
  }
  m->active_frames += active;
  ++m->playback_frames;
  m->playback_end_us = (uint64_t)esp_timer_get_time();
}

static void observation_error(void* user, int is_capture, int rc) {
  meter_t* m = user;
  if (is_capture)
    m->capture_rc = rc;
  else
    m->playback_rc = rc;
}

static void remember(int* first, int rc) {
  if (*first == H2_PAL_OK && rc != H2_PAL_OK) *first = rc;
}

static int window(const h2_pal_audio_t* audio, unsigned profile, unsigned index,
                  unsigned speaker, unsigned mic) {
  const bool far = speaker != 0u;
  const unsigned warmup = far ? 96u : 32u;
  const unsigned measured = far ? 96u : 64u;
  memset(&meter, 0, sizeof(meter));
  meter.capture_skip = meter.playback_skip = warmup;
  meter.measured = measured;
  uint32_t actual_speaker = 0u, actual_mic = 0u;
  int rc = h2_pal_audio_set_speaker_volume_percent(audio, speaker);
  if (rc == H2_PAL_OK) rc = h2_pal_audio_set_mic_gain_percent(audio, mic);
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_get_speaker_volume_percent(audio, &actual_speaker);
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_get_mic_gain_percent(audio, &actual_mic);
  const h2_audio_aec_observer_t observer = {.user = &meter,
                                            .on_capture = capture,
                                            .on_playback = playback,
                                            .on_error = observation_error};
  bool registered = false, mic_started = false, speaker_started = false;
  if (rc == H2_PAL_OK) {
    rc = h2_pal_audio_set_aec_observer(audio, &observer);
    registered = rc == H2_PAL_OK;
  }
  const uint64_t start = (uint64_t)esp_timer_get_time();
  if (rc == H2_PAL_OK) {
    rc = h2_pal_audio_start_mic(audio);
    mic_started = rc == H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) {
    rc = h2_pal_audio_start_speaker(audio);
    speaker_started = rc == H2_PAL_OK;
  }
  const h2_audio_pcm_format_t format = {.sample_rate_hz = 16000u,
                                        .frame_samples_per_channel = SAMPLES,
                                        .channels = 1u,
                                        .sample_format = H2_AUDIO_SAMPLE_S16LE};
  const h2_audio_track_config_t config = {.name = "aec-probe/far",
                                          .format = format,
                                          .volume_factor_milli = 1000u,
                                          .buffer_frames = 4u,
                                          .allocator = runtime->mem};
  h2_pal_audio_track_t* track = NULL;
  if (rc == H2_PAL_OK) rc = h2_pal_audio_create_track(audio, &config, &track);
  int16_t out[SAMPLES], in[SAMPLES];
  for (unsigned i = 0u; i < SAMPLES; ++i) out[i] = far ? tone[i % 32u] : 0;
  const uint64_t nominal_us = (uint64_t)(warmup + measured) * 32000u;
  for (unsigned i = 0u; rc == H2_PAL_OK && i < warmup + measured; ++i) {
    h2_audio_frame_t tx = h2_audio_frame_for_buffer(out, sizeof(out), format);
    tx.bytes = sizeof(out);
    rc = h2_pal_audio_track_write(track, &tx, 1000u);
    h2_audio_frame_t rx = h2_audio_frame_for_buffer(in, sizeof(in), format);
    if (rc == H2_PAL_OK) rc = h2_pal_audio_mic_read(audio, &rx, 1000u);
    if (rc == H2_PAL_OK && !valid(&rx, 1u)) rc = H2_PAL_ERR_FORMAT;
    if ((uint64_t)esp_timer_get_time() - start > nominal_us * 5u / 4u + 100000u)
      remember(&rc, H2_PAL_ERR_TIMEOUT);
  }
  if (rc == H2_PAL_OK) rc = h2_pal_audio_track_drain(track, 1000u);
  int cleanup = H2_PAL_OK;
  if (mic_started) remember(&cleanup, h2_pal_audio_stop_mic(audio));
  if (track != NULL) {
    const int closed = h2_pal_audio_track_close(track);
    remember(&cleanup, closed);
    if (closed == H2_PAL_OK) track = NULL;
  }
  /* Speaker stop destroys its mixer and any attached tracks. Close our handle
   * first, and retain it with its speaker if close failed. */
  if (speaker_started && track == NULL)
    remember(&cleanup, h2_pal_audio_stop_speaker(audio));
  if (cleanup == H2_PAL_OK && registered)
    remember(&cleanup, h2_pal_audio_set_aec_observer(audio, NULL));
  /* No concurrent callback reads: both workers have joined at this point.
   * Keep the static observer user and every provider alive on cleanup error. */
  if (cleanup != H2_PAL_OK) {
    retained = true;
    printf("H2_AEC_PROBE_CLEANUP_FAIL profile=%u index=%u rc=%d retained=1\n",
           profile, index, cleanup);
    fflush(stdout);
    return cleanup;
  }
  uint64_t end = meter.capture_end_us > meter.playback_end_us
                     ? meter.capture_end_us
                     : meter.playback_end_us;
  const uint64_t elapsed = end >= start ? end - start : 0u;
  remember(&rc, meter.capture_rc);
  remember(&rc, meter.playback_rc);
  if (meter.frames != measured || meter.playback_frames != measured ||
      meter.active_frames != (far ? measured : 0u) ||
      elapsed + 1000u < nominal_us || elapsed > nominal_us * 5u / 4u + 100000u)
    remember(&rc, H2_PAL_ERR_INVALID_STATE);
  printf(
      "H2_AEC_PROBE_POINT {\"run\":\"%016" PRIx64
      "\",\"profile\":%u,\"index\":%u,"
      "\"speaker\":%u,\"mic\":%u,\"actual_speaker\":%u,\"actual_mic\":%u,"
      "\"frames\":%u,\"playback_frames\":%u,\"active_frames\":%u,\"elapsed_us\":%" PRIu64
      ","
      "\"warmup\":%u,\"measured\":%u,\"mic_lane\":%u,\"mic_mask\":%u,\"ref_lane\":%u,"
      "\"peak\":[%u,%u,%u,%u],\"energy\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64
      ",%" PRIu64
      "],"
      "\"clipped\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
      "],"
      "\"reference_peak\":%u,\"reference_energy\":%" PRIu64
      ",\"reference_clipped\":%" PRIu64
      ","
      "\"output_peak\":%u,\"output_energy\":%" PRIu64
      ",\"output_clipped\":%" PRIu64
      ","
      "\"playback_peak\":%u,\"playback_clipped\":%" PRIu64
      ",\"rc\":%d,\"cleanup\":%d}\n",
      run_id, profile, index, speaker, mic, (unsigned)actual_speaker,
      (unsigned)actual_mic, (unsigned)meter.frames,
      (unsigned)meter.playback_frames, (unsigned)meter.active_frames, elapsed,
      warmup, measured, meter.mic_lane, meter.mic_mask, meter.ref_lane,
      (unsigned)meter.peak[0], (unsigned)meter.peak[1], (unsigned)meter.peak[2],
      (unsigned)meter.peak[3], meter.energy[0], meter.energy[1],
      meter.energy[2], meter.energy[3], meter.clipped[0], meter.clipped[1],
      meter.clipped[2], meter.clipped[3], (unsigned)meter.reference_peak,
      meter.reference_energy, meter.reference_clipped,
      (unsigned)meter.output_peak, meter.output_energy, meter.output_clipped,
      (unsigned)meter.playback_peak, meter.playback_clipped, rc, cleanup);
  fflush(stdout);
  return rc;
}

static void run(void* user) {
  (void)user;
  const unsigned speaker[WINDOWS] = {0u,  75u, 100u, 0u,  0u,
                                     75u, 0u,  0u,   75u, 0u};
  const unsigned mic[WINDOWS] = {63u, 63u, 63u, 63u, 47u,
                                 47u, 47u, 79u, 79u, 79u};
  int result = H2_PAL_OK;
  unsigned completed = 0u;
  for (unsigned p = 0u; p < PROFILES; ++p) {
    const uint8_t microphones = (uint8_t)(p / 2u + 1u);
    const uint8_t ref_db = (uint8_t)(p % 2u * 12u);
    int rc = h2_ws175c_audio_probe_select_aec(microphones, ref_db);
    const h2_pal_audio_t* audio = provider_audio;
    h2_audio_info_t info = {0};
    if (rc == H2_PAL_OK) rc = h2_pal_audio_get_info(audio, &info);
    if (rc == H2_PAL_OK &&
        (info.mic_format.frame_samples_per_channel != SAMPLES ||
         info.playback_format.frame_samples_per_channel != SAMPLES))
      rc = H2_PAL_ERR_FORMAT;
    printf("H2_AEC_PROBE_PROFILE run=%016" PRIx64
           " profile=%u microphones=%u ref_gain_db=%u nlp=normal rc=%d\n",
           run_id, p, (unsigned)microphones, (unsigned)ref_db, rc);
    for (unsigned i = 0u; rc == H2_PAL_OK && i < WINDOWS; ++i) {
      rc = window(audio, p, i, speaker[i], mic[i]);
      if (rc == H2_PAL_OK) ++completed;
    }
    remember(&result, rc);
    if (retained) break;
    /* Deinit is serialized after all workers/observers have been removed.
     * If cleanup fails, retain dependencies and leave managed recovery alive.
     */
    const int cleanup = h2_ws175c_audio_probe_release();
    if (cleanup != H2_PAL_OK) {
      remember(&result, cleanup);
      break;
    }
  }
  const int confirm = result == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                          : H2_PAL_ERR_INVALID_STATE;
  printf("H2_AEC_PROBE_DONE run=%016" PRIx64
         " windows=%u rc=%d confirm=%d acoustic_qualified=0\n",
         run_id, completed, result, confirm);
  fflush(stdout);
  hold();
}

static void entry(void* user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_esp_board_runtime_config(&config);
  provider_audio = config.audio;
  if (rc == H2_PAL_OK)
    rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "aec-probe", 1u,
                                                     3u);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
  uint8_t mac[6] = {0};
  if (rc == H2_PAL_OK && esp_read_mac(mac, ESP_MAC_BT) != ESP_OK)
    rc = H2_PAL_ERR_IO;
  snprintf(uid, sizeof(uid), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  run_id = ((uint64_t)esp_random() << 32u) | esp_random();
  printf(
      "H2_AEC_PROBE_BOOT board=waveshare_esp32s3_touch_amoled_1_75c uid=%s version=%s run=%016" PRIx64
      " scope=single-board-far-echo\n",
      uid, esp_app_get_description()->version, run_id);
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {.name = "aec-probe/e2e/runner"};
    h2_pal_task_t* task = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  }
  if (rc != H2_PAL_OK) printf("H2_AEC_PROBE_SETUP_FAIL rc=%d\n", rc);
  hold();
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("ws175c/aec-probe", entry, NULL);
  if (rc != H2_PAL_OK) printf("H2_AEC_PROBE_SETUP_FAIL rc=%d\n", rc);
}
