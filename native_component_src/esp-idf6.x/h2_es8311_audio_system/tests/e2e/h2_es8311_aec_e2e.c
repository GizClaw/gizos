#include "h2_es8311_aec_e2e.h"
#include "driver/i2s_std.h"
#include "esp_aec.h"
#include "esp_private/esp_clk.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define N 512
#define FRAME_BUDGET_US (N * 1000000LL / 16000)
static int16_t raw[N * 2];
static int16_t output[N];
static int16_t delay_line[32];
static unsigned rng;
static int16_t noise(void) {
  rng = rng * 1664525u + 1013904223u;
  return (int16_t)((int)(rng >> 18) - 8192);
}
int h2_es8311_aec_e2e(void) {
  int failures = esp_clk_cpu_freq() != 160000000;
  printf("AEC_E2E CLOCK configured_mhz=%d actual_hz=%d\n",
         CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_clk_cpu_freq());
  printf(
      "AEC_E2E BEGIN sr=2.6.0 sample_rate=16000 mode=platform_default filter=4 pal_samples=%d\n",
      N);
  for (int cycle = 0; cycle < 3; ++cycle) {
    h2_esp_es8311_audio_system_config_t config = {
        .sample_rate_hz = 16000,
        .frame_samples_per_channel = N,
        .raw_channels = 2,
        .processed_channels = 1,
        .mic_channel_index = 0,
        .ref_channel_index = 1,
        .enable_aec = 1,
    };
    h2_esp_es8311_sr_state_t state = {0};
    size_t before = heap_caps_get_free_size(CAPS);
    int rc = h2_esp_es8311_sr_init(&state, &config);
    printf(
        "AEC_E2E INIT cycle=%d rc=%d chunk=%u free_before=%u free_after=%u largest=%u\n",
        cycle, rc, (unsigned)state.frame_samples, (unsigned)before,
        (unsigned)heap_caps_get_free_size(CAPS),
        (unsigned)heap_caps_get_largest_free_block(CAPS));
    if (rc != H2_AUDIO_OK || !state.available) {
      ++failures;
      h2_esp_es8311_sr_deinit(&state);
      break;
    }
    h2_audio_pcm_format_t rf = {.sample_rate_hz = 16000,
                                .channels = 2,
                                .sample_format = H2_AUDIO_SAMPLE_S16LE,
                                .frame_samples_per_channel = N};
    h2_audio_pcm_format_t of = rf;
    of.channels = 1;
    h2_audio_frame_t input = h2_audio_frame_for_buffer(raw, sizeof(raw), rf);
    h2_audio_frame_t out =
        h2_audio_frame_for_buffer(output, sizeof(output), of);
    input.bytes = sizeof(raw);
    input.samples_per_channel = N;
    rng = 1;
    memset(delay_line, 0, sizeof(delay_line));
    for (int phase = 0; phase < 3; ++phase) {
      double in_energy = 0, out_energy = 0, near_sin = 0, near_cos = 0;
      int64_t total_us = 0, max_us = 0;
      unsigned changed = 0, over_budget = 0, measured_samples = 0,
               processed_frames = 0;
      for (int frame = 0; frame < 250; ++frame) {
        for (int i = 0; i < N; ++i) {
          unsigned sample = (unsigned)(frame * N + i);
          int16_t ref = phase == 1 ? 0 : noise();
          int16_t echo = delay_line[sample % 32] / 2;
          delay_line[sample % 32] = ref;
          int near = phase == 0 ? 0
                                : (int)(3000 * sin(6.283185307179586 * 997.0 *
                                                   sample / 16000.0));
          raw[2 * i] = echo + near;
          raw[2 * i + 1] = ref;
        }
        int64_t start = esp_timer_get_time();
        rc = h2_esp_es8311_sr_process(&state, &input, &out, 0);
        int64_t elapsed = esp_timer_get_time() - start;
        total_us += elapsed;
        if (elapsed > max_us) max_us = elapsed;
        if (elapsed > FRAME_BUDGET_US) ++over_budget;
        if (rc || out.bytes != sizeof(output) || out.samples_per_channel != N) {
          ++failures;
          break;
        }
        ++processed_frames;
        if (frame >= 125)
          for (int i = 0; i < N; ++i) {
            double m = raw[i * 2], o = output[i];
            in_energy += m * m;
            out_energy += o * o;
            changed += output[i] != raw[i * 2];
            double angle =
                6.283185307179586 * 997.0 * (frame * N + i) / 16000.0;
            near_sin += o * sin(angle);
            near_cos += o * cos(angle);
            ++measured_samples;
          }
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      double ratio = out_energy / (in_energy + 1);
      /* Magnitude is insensitive to the algorithm's fixed delay. */
      double near_gain =
          measured_samples
              ? 2 * sqrt(near_sin * near_sin + near_cos * near_cos) /
                    (measured_samples * 3000.0)
              : 0;
      int signal_ok =
          phase == 0 ? ratio < 0.25 : near_gain > 0.1 && near_gain < 2;
      int pass = rc == 0 && processed_frames == 250 && changed != 0 &&
                 signal_ok &&
                 total_us < (int64_t)processed_frames * FRAME_BUDGET_US;
      if (!pass) ++failures;
      printf(
          "AEC_E2E PHASE cycle=%d phase=%d result=%s rc=%d ratio=%.6f near_gain=%.6f avg_us=%lld max_us=%lld over_frame_budget=%u changed=%u free=%u minimum=%u largest=%u stack_free=%u\n",
          cycle, phase, pass ? "PASS" : "FAIL", rc, ratio, near_gain,
          (long long)(processed_frames ? total_us / processed_frames : 0),
          (long long)max_us, over_budget, changed,
          (unsigned)heap_caps_get_free_size(CAPS),
          (unsigned)heap_caps_get_minimum_free_size(CAPS),
          (unsigned)heap_caps_get_largest_free_block(CAPS),
          (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    input.channels = 1;
    rc = h2_esp_es8311_sr_process(&state, &input, &out, 0);
    if (rc == H2_AUDIO_OK || out.bytes != 0) ++failures;
    h2_esp_es8311_sr_reset(&state);
    h2_esp_es8311_sr_deinit(&state);
    h2_esp_es8311_sr_deinit(&state);
    vTaskDelay(pdMS_TO_TICKS(30));
    size_t after = heap_caps_get_free_size(CAPS);
    if (after < before) ++failures;
    printf(
        "AEC_E2E CLEANUP cycle=%d before=%u after=%u largest=%u available=%d\n",
        cycle, (unsigned)before, (unsigned)after,
        (unsigned)heap_caps_get_largest_free_block(CAPS), state.available);
  }
  printf("AEC_E2E END result=%s failures=%d acoustic=NOT_TESTED\n",
         failures ? "FAIL" : "PASS", failures);
  fflush(stdout);
  return failures;
}
/* Native fixture owns the system and reads I2S only while its mic worker is
 * off. */
int h2_es8311_aec_live_e2e(const h2_esp_es8311_audio_system_config_t* config) {
  if (config == NULL || !config->enable_aec ||
      config->sample_rate_hz != 16000 ||
      config->frame_samples_per_channel != N || config->raw_channels != 2 ||
      config->processed_channels != 1 || config->mic_channel_index >= 2 ||
      config->ref_channel_index >= 2 ||
      config->mic_channel_index == config->ref_channel_index) {
    printf("AEC_LIVE END result=FAIL reason=invalid_config\n");
    return 1;
  }
  int failures = 0;
  for (int cycle = 0; cycle < 3; ++cycle) {
    size_t before = heap_caps_get_free_size(CAPS);
    h2_esp_es8311_audio_system_t* system = calloc(1, sizeof(*system));
    if (!system) return 1;
    int rc = h2_esp_es8311_audio_system_init(system, config);
    if (!rc) rc = h2_esp_es8311_audio_system_prepare(system);
    h2_pal_audio_t* audio =
        rc ? NULL : h2_esp_es8311_audio_system_audio(system);
    h2_audio_info_t info = {0};
    if (!rc) rc = h2_pal_audio_get_info(audio, &info);
    h2_pal_audio_track_t* track = NULL;
    h2_audio_track_config_t tc = {.name = "aec-e2e",
                                  .format = info.playback_format,
                                  .volume_factor_milli = 1000,
                                  .buffer_frames = 2};
    if (!rc) rc = h2_pal_audio_create_track(audio, &tc, &track);
    if (!rc) rc = h2_pal_audio_set_speaker_volume_percent(audio, 30);
    if (!rc) rc = h2_pal_audio_start_speaker(audio);
    int16_t tone[N];
    for (int i = 0; i < N; ++i)
      tone[i] = (int16_t)(1024 * sin(6.283185307179586 * i / 16));
    h2_audio_frame_t playback =
        h2_audio_frame_for_buffer(tone, sizeof(tone), info.playback_format);
    playback.bytes = sizeof(tone);
    playback.samples_per_channel = N;
    h2_audio_pcm_format_t rf = info.mic_format;
    rf.channels = 2;
    h2_audio_frame_t input = h2_audio_frame_for_buffer(raw, sizeof(raw), rf);
    h2_audio_frame_t out =
        h2_audio_frame_for_buffer(output, sizeof(output), info.mic_format);
    double mic_energy = 0, ref_energy = 0, processed_energy = 0;
    int64_t total = 0, maximum = 0;
    unsigned frames = 0;
    for (; !rc && frames < 100; ++frames) {
      rc = h2_pal_audio_track_write(track, &playback, 1000);
      if (rc) break;
      size_t got = 0;
      int err = i2s_channel_read(system->rx_chan, raw, sizeof(raw), &got, 1000);
      if (err || got != sizeof(raw)) {
        rc = -100;
        break;
      }
      input.bytes = got;
      input.samples_per_channel = N;
      int64_t start = esp_timer_get_time();
      rc = h2_esp_es8311_audio_system_process_mic(system, &input, &out, 0);
      int64_t us = esp_timer_get_time() - start;
      if (rc || out.bytes != sizeof(output) || out.samples_per_channel != N) {
        if (!rc) rc = H2_AUDIO_ERR_INVALID_STATE;
        break;
      }
      total += us;
      if (us > maximum) maximum = us;
      for (int i = 0; i < N; ++i) {
        double m = raw[i * 2 + config->mic_channel_index];
        double r = raw[i * 2 + config->ref_channel_index];
        double o = output[i];
        mic_energy += m * m;
        ref_energy += r * r;
        processed_energy += o * o;
      }
    }
    printf(
        "AEC_LIVE capture cycle=%d rc=%d frames=%u mic_energy=%.0f ref_energy=%.0f output_energy=%.0f avg_us=%lld max_us=%lld free=%u largest=%u\n",
        cycle, rc, frames, mic_energy, ref_energy, processed_energy,
        (long long)(frames ? total / frames : 0), (long long)maximum,
        (unsigned)heap_caps_get_free_size(CAPS),
        (unsigned)heap_caps_get_largest_free_block(CAPS));
    if (rc || frames != 100 || ref_energy == 0 || mic_energy == 0 ||
        processed_energy == 0 || total >= (int64_t)frames * FRAME_BUDGET_US)
      ++failures;
    for (int session = 0; !rc && session < 2; ++session) {
      rc = h2_pal_audio_start_mic(audio);
      if (!rc) rc = h2_pal_audio_start_mic(audio);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
      TaskStatus_t task_before = {0}, task_after = {0};
      if (!rc) vTaskGetInfo(system->mic_task, &task_before, pdFALSE, eInvalid);
#endif
      int64_t session_start = esp_timer_get_time();
      for (int frame = 0; !rc && frame < 50; ++frame) {
        rc = h2_pal_audio_track_write(track, &playback, 1000);
        if (!rc) rc = h2_pal_audio_mic_read(audio, &out, 1000);
        if (!rc && out.bytes != sizeof(output)) rc = -101;
      }
      int stop = h2_pal_audio_stop_mic(audio);
      int64_t session_us = esp_timer_get_time() - session_start;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
      if (system->mic_task) {
        vTaskGetInfo(system->mic_task, &task_after, pdFALSE, eInvalid);
        printf(
            "AEC_LIVE cpu cycle=%d session=%d worker_us=%lu elapsed_us=%lld worker_cpu_percent=%.2f\n",
            cycle, session,
            (unsigned long)(task_after.ulRunTimeCounter -
                            task_before.ulRunTimeCounter),
            (long long)session_us,
            100.0 *
                (task_after.ulRunTimeCounter - task_before.ulRunTimeCounter) /
                session_us);
      }
#else
      printf("AEC_LIVE cpu=NOT_MEASURED elapsed_us=%lld\n",
             (long long)session_us);
#endif
      printf(
          "AEC_LIVE worker cycle=%d session=%d rc=%d stop=%d stack_free=%u\n",
          cycle, session, rc, stop,
          system->mic_task
              ? (unsigned)uxTaskGetStackHighWaterMark(system->mic_task)
              : 0);
      if (rc || stop) ++failures;
    }
    if (track && h2_pal_audio_track_close(track)) ++failures;
    if (audio && h2_pal_audio_stop_mic(audio)) ++failures;
    if (audio && h2_pal_audio_stop_speaker(audio)) ++failures;
    int cleanup = h2_esp_es8311_audio_system_power_down(system);
    if (cleanup) {
      ++failures;
      return failures;
    }
    if (h2_esp_es8311_audio_system_power_down(system)) ++failures;
    free(system);
    vTaskDelay(pdMS_TO_TICKS(100));
    size_t after = heap_caps_get_free_size(CAPS);
    /* The first codec open may retain SDK-wide interrupt/driver state.
     * Require stable per-instance cleanup after that explicitly logged warmup.
     */
    if (cycle != 0 && after < before) ++failures;
    printf("AEC_LIVE cleanup cycle=%d rc=%d before=%u after=%u largest=%u\n",
           cycle, cleanup, (unsigned)before, (unsigned)after,
           (unsigned)heap_caps_get_largest_free_block(CAPS));
  }
  printf("AEC_LIVE END result=%s failures=%d acoustic=NOT_QUALIFIED\n",
         failures ? "FAIL" : "PASS", failures);
  return failures;
}
int h2_es8311_aec_modes_e2e(void) {
  static const aec_mode_t modes[] = {
      AEC_MODE_SR_LOW_COST,    AEC_MODE_SR_HIGH_PERF, AEC_MODE_VOIP_LOW_COST,
      AEC_MODE_VOIP_HIGH_PERF, AEC_MODE_FD_LOW_COST,  AEC_MODE_FD_HIGH_PERF};
  int succeeded = 0;
  for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    size_t before = heap_caps_get_free_size(CAPS);
    (void)heap_caps_monitor_local_minimum_free_size_start();
    aec_config_t config = {.mic_num = 1,
                           .ref_num = 1,
                           .out_num = 1,
                           .filter_length = 4,
                           .sample_rate = 16000,
                           .caps = CAPS,
                           .mode = modes[i],
                           .nlp_level = AEC_NLP_LEVEL_NORMAL};
    aec_handle_t* handle = aec_create_from_config(&config);
    size_t active = heap_caps_get_free_size(CAPS);
    int chunk = handle ? aec_get_chunksize(handle) : 0;
    int64_t elapsed = 0;
    int16_t* buffers = NULL;
    if (handle && chunk > 0 && chunk <= 512) {
      buffers = heap_caps_aligned_calloc(16, (size_t)chunk * 3, sizeof(int16_t),
                                         CAPS);
      if (buffers) {
        int64_t start = esp_timer_get_time();
        for (int frame = 0; frame < 10; ++frame)
          aec_process(handle, buffers, buffers + chunk, buffers + chunk * 2);
        elapsed = esp_timer_get_time() - start;
        ++succeeded;
      }
    }
    heap_caps_free(buffers);
    if (handle) aec_destroy(handle);
    size_t minimum = heap_caps_get_minimum_free_size(CAPS);
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    printf(
        "AEC_MODE mode=%d filter=4 chunk=%d before=%u active=%u minimum=%u after=%u avg_us=%lld result=%s\n",
        (int)modes[i], chunk, (unsigned)before, (unsigned)active,
        (unsigned)minimum, (unsigned)heap_caps_get_free_size(CAPS),
        (long long)(elapsed / 10), elapsed ? "EXECUTED" : "FAILED");
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return succeeded;
}
