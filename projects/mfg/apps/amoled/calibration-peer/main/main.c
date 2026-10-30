#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg_aec_fixture.h"

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static h2_runtime_t* runtime;
static char uid[13];
/* Shared factory identity configuration; supplied by H2_MFG_PEER_DUT_UID. */
extern const char* h2_mfg_peer_dut_uid(void);

enum { SOURCE_VOLUME = 95, SOURCE_WARMUP_MS = 2000 };
typedef struct source_meter {
  uint64_t started_ms, frames;
  uint32_t run_hi, run_lo, sequence;
  int confirmed;
} source_meter_t;

static int feed_source(h2_pal_audio_track_t* track, int16_t* probe,
                       h2_audio_pcm_format_t format, source_meter_t* meter) {
  for (unsigned i = 0; i < 8; ++i) {
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        probe, H2_MFG_AEC_PROBE_SAMPLES * sizeof(int16_t), format);
    frame.bytes = frame.capacity;
    int rc = h2_pal_audio_track_write(track, &frame, 0);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) return 0;
    if (rc != 0) return rc;
    ++meter->frames;
  }
  return 0;
}

static int stop_source(h2_pal_audio_track_t** track, int* started,
                       source_meter_t* meter) {
  int rc = 0;
  if (*track != NULL) {
    rc = h2_pal_audio_track_close(*track);
    if (rc != 0) return rc;
    *track = NULL;
  }
  if (*started) {
    rc = h2_pal_audio_stop_speaker(runtime->audio);
    if (rc == 0) {
      *started = 0;
      uint64_t now = 0;
      rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
      uint64_t elapsed = now >= meter->started_ms ? now - meter->started_ms : 0;
      uint64_t pcm_ms = meter->frames * 32;
      printf("H2_MFG_AEC_PEER_PLAYBACK run=%08" PRIx32 "%08" PRIx32
             " sequence=%" PRIu32 " queued_frames=%" PRIu64 " pcm_ms=%" PRIu64
             " elapsed_ms=%" PRIu64 " volume=%u\n",
             meter->run_hi, meter->run_lo, meter->sequence, meter->frames,
             pcm_ms, elapsed, SOURCE_VOLUME);
      fflush(stdout);
      /* Queue acceptance is not a DAC/drop counter. The DUT independently
       * checks actual acoustic input stability. */
      if (rc == 0 && meter->confirmed &&
          (pcm_ms + 32 < elapsed * 95 / 100 || pcm_ms > elapsed + 512))
        rc = H2_PAL_ERR_IO;
    }
  }
  return rc;
}

static void run_source(void* unused) {
  (void)unused;
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int rc = fd >= 0 ? 0 : H2_PAL_ERR_IO;
  struct sockaddr_in local = {.sin_family = AF_INET,
                              .sin_port = htons(H2_MFG_AEC_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (rc == 0 && bind(fd, (struct sockaddr*)&local, sizeof(local)) != 0)
    rc = H2_PAL_ERR_IO;
  h2_audio_info_t info = {0};
  if (rc == 0) rc = h2_pal_audio_get_info(runtime->audio, &info);
  h2_audio_pcm_format_t format = info.playback_format;
  if (rc == 0 &&
      (format.sample_rate_hz != 16000 || format.channels != 1 ||
       format.frame_samples_per_channel != H2_MFG_AEC_PROBE_SAMPLES ||
       format.sample_format != H2_AUDIO_SAMPLE_S16LE))
    rc = H2_PAL_ERR_UNSUPPORTED;
  uint32_t original_volume = 0;
  if (rc == 0)
    rc = h2_pal_audio_get_speaker_volume_percent(runtime->audio,
                                                 &original_volume);
  h2_pal_audio_track_t* track = NULL;
  source_meter_t meter = {0};
  int started = 0, shutdown = 0;
  uint64_t last_control = 0;
  h2_mfg_aec_packet_t request, response = {0}, last_request = {0};
  h2_mfg_aec_control_guard_t controls = {0};
  int have_last = 0;
  int16_t probe[H2_MFG_AEC_PROBE_SAMPLES] = {0};
  printf(
      "H2_MFG_AEC_PEER_READY uid=%s version=%s port=%u volume=%u"
      " source_warmup_ms=%u rc=%d\n",
      uid, esp_app_get_description()->version, H2_MFG_AEC_PORT, SOURCE_VOLUME,
      SOURCE_WARMUP_MS, rc);
  fflush(stdout);
  while (rc == 0 && !shutdown) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 5000};
    int selected = select(fd + 1, &readable, NULL, NULL, &timeout);
    if (selected < 0) {
      rc = H2_PAL_ERR_IO;
      break;
    }
    if (selected > 0) {
      struct sockaddr_in remote;
      socklen_t length = sizeof(remote);
      ssize_t bytes = recvfrom(fd, &request, sizeof(request), 0,
                               (struct sockaddr*)&remote, &length);
      if (bytes < 0) {
        rc = H2_PAL_ERR_IO;
        break;
      }
      if (!h2_mfg_aec_packet_valid(&request, (size_t)bytes) ||
          memcmp(request.target_uid, uid, 12) != 0 ||
          memcmp(request.source_uid, h2_mfg_peer_dut_uid(), 12) != 0 ||
          request.operation > H2_MFG_AEC_SOURCE_SHUTDOWN)
        continue;
      int duplicate = have_last &&
                      bytes == (ssize_t)h2_mfg_aec_packet_size(&last_request) &&
                      memcmp(&request, &last_request, (size_t)bytes) == 0;
      if (!duplicate && !h2_mfg_aec_control_accept(&controls, &request,
                                                   started || track != NULL))
        continue;
      if (!duplicate) {
        int control = 0;
        if (request.operation == H2_MFG_AEC_SOURCE_START) {
          if (request.samples != H2_MFG_AEC_PROBE_SAMPLES ||
              request.sample_rate != 16000 || started)
            control = H2_PAL_ERR_INVALID_STATE;
          if (control == 0) {
            meter = (source_meter_t){.run_hi = request.run_hi,
                                     .run_lo = request.run_lo,
                                     .sequence = request.sequence};
            memcpy(probe, request.pcm, sizeof(probe));
            control = h2_pal_audio_set_speaker_volume_percent(runtime->audio,
                                                              SOURCE_VOLUME);
          }
          if (control == 0) {
            control = h2_pal_audio_start_speaker(runtime->audio);
            if (control == 0) started = 1;
          }
          if (control == 0)
            control =
                h2_pal_time_get_monotonic_ms(runtime->time, &meter.started_ms);
          const h2_audio_track_config_t config = {.name = "mfg/aec-near-source",
                                                  .format = format,
                                                  .volume_factor_milli = 1000,
                                                  .buffer_frames = 8,
                                                  .allocator = runtime->mem};
          if (control == 0)
            control =
                h2_pal_audio_create_track(runtime->audio, &config, &track);
          /* Settle the physical source before ACK. This is source control
           * time, separately reported by the unchanged calibration runner. */
          uint64_t now = meter.started_ms;
          while (control == 0 && now - meter.started_ms < SOURCE_WARMUP_MS) {
            control = feed_source(track, probe, format, &meter);
            if (control == 0) control = h2_pal_time_sleep_ms(runtime->time, 5);
            if (control == 0)
              control = h2_pal_time_get_monotonic_ms(runtime->time, &now);
          }
          if (control == 0) meter.confirmed = 1;
        } else {
          control = stop_source(&track, &started, &meter);
          if (control == 0 && request.operation == H2_MFG_AEC_SOURCE_SHUTDOWN)
            shutdown = 1;
        }
        response = request;
        response.operation |= H2_MFG_AEC_SOURCE_ACK;
        response.samples = 0;
        response.result = control;
        memcpy(response.source_uid, uid, 12);
        memcpy(response.target_uid, request.source_uid, 12);
        h2_mfg_aec_packet_seal(&response);
        last_request = request;
        have_last = 1;
        printf("H2_MFG_AEC_PEER_CONTROL run=%08" PRIx32 "%08" PRIx32
               " sequence=%" PRIu32 " operation=%" PRIu32
               " probe_checksum=%08" PRIx32 " active=%d rc=%d\n",
               request.run_hi, request.run_lo, request.sequence,
               request.operation, request.checksum, started, control);
        fflush(stdout);
      }
      ssize_t sent = sendto(fd, &response, h2_mfg_aec_packet_size(&response), 0,
                            (struct sockaddr*)&remote, length);
      if (sent != (ssize_t)h2_mfg_aec_packet_size(&response))
        rc = H2_PAL_ERR_IO;
      (void)h2_pal_time_get_monotonic_ms(runtime->time, &last_control);
    }
    if (rc == 0 && started) {
      rc = feed_source(track, probe, format, &meter);
      uint64_t now = 0;
      if (rc == 0) rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
      if (rc == 0 && now - last_control > 40000) {
        rc = stop_source(&track, &started, &meter);
        /* An expired START must not replay its old successful ACK. */
        have_last = 0;
        printf("H2_MFG_AEC_PEER_LEASE_EXPIRED cleanup=%d\n", rc);
        fflush(stdout);
      }
    }
  }
  int cleanup = stop_source(&track, &started, &meter);
  if (cleanup == 0)
    cleanup = h2_pal_audio_set_speaker_volume_percent(runtime->audio,
                                                      original_volume);
  if (fd >= 0) (void)close(fd);
  printf("H2_MFG_AEC_PEER_END uid=%s rc=%d cleanup=%d retained=%d\n", uid, rc,
         cleanup, cleanup != 0);
  fflush(stdout);
}

static void entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == 0)
    rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "mfg-aec-source",
                                                     1, 3);
  if (rc == 0) rc = h2_runtime_init(&config, &runtime);
  uint8_t mac[6];
  if (rc == 0 && esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) rc = H2_PAL_ERR_IO;
  if (rc == 0)
    (void)snprintf(uid, sizeof(uid), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
                   mac[2], mac[3], mac[4], mac[5]);
  h2_pal_wifi_sta_config_t saved = {0};
  if (rc == 0)
    rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings,
                                                   &saved);
  if (rc == 0) rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, 30000);
  memset(&saved, 0, sizeof(saved));
  h2_pal_wifi_sta_status_t status;
  uint64_t started = 0, now = 0;
  if (rc == 0) rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  while (rc == 0) {
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc != 0 || (h2_pal_wifi_sta_status_has_ip(&status) && status.ip_valid))
      break;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc == 0 && now - started >= 30000) rc = H2_PAL_ERR_TIMEOUT;
    if (rc == 0) rc = h2_pal_time_sleep_ms(runtime->time, 20);
  }
  if (rc == 0 && strlen(h2_mfg_peer_dut_uid()) != 12)
    rc = H2_PAL_ERR_INVALID_ARG;
  if (rc == 0) rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc == 0) {
    printf("H2_MFG_AEC_PEER_BOOT uid=%s ip=%u.%u.%u.%u version=%s dut_uid=%s\n",
           uid, (unsigned)(status.ip.ip4 >> 24),
           (unsigned)((status.ip.ip4 >> 16) & 255),
           (unsigned)((status.ip.ip4 >> 8) & 255),
           (unsigned)(status.ip.ip4 & 255), esp_app_get_description()->version,
           h2_mfg_peer_dut_uid());
    run_source(NULL);
  }
  if (rc != 0) printf("H2_MFG_AEC_PEER_SETUP_FAIL rc=%d\n", rc);
  /* Keep Runtime and serial recovery alive after the fixture worker stopped.
   * The board entry wrapper deinitializes providers when this function exits.
   */
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc == 0) rc = h2_esp_board_start_entry_task("mfg/aec-peer", entry, NULL);
  if (rc != 0) printf("H2_MFG_AEC_PEER_SETUP_FAIL rc=%d\n", rc);
}
