#include "lwip/sockets.h"
/* Independent acoustic probe source. No DUT PCM injection. */
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_target_task_policy.h"
#include "h2_e2e_fixture_wifi.h"
#include <stdio.h>
#include <string.h>
#ifndef H2_AEC_SOURCE_TOKEN
#error An explicit source-control token is required
#endif
/* AMOLED legacy mapping: floor(203 * 70 / 100) = 142, -24.5 dB. */
#define AEC_SOURCE_VOLUME_PERCENT 70u
static h2_runtime_t* runtime;
static const h2_pal_audio_api_t* audio;
static QueueHandle_t commands, replies;
static h2_pal_audio_track_t* track;
static uint32_t original_volume;
typedef struct command {
  int enabled;
  int16_t pcm[512];
} command_t;
static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("AEC_SOURCE_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static int stop(void) {
  int rc = h2_pal_audio_stop_speaker(audio);
  if (!rc && track != NULL) {
    rc = h2_pal_audio_track_close(track);
    if (!rc) track = NULL;
  }
  if (!rc) rc = h2_pal_audio_set_speaker_volume_percent(audio, original_volume);
  return rc;
}
static void source_worker(void* unused) {
  (void)unused;
  command_t cmd = {0}, next;
  const h2_audio_pcm_format_t format = {.sample_rate_hz = 16000,
                                        .frame_samples_per_channel = 512,
                                        .channels = 1,
                                        .sample_format = H2_AUDIO_SAMPLE_S16LE};
  int64_t lease = 0;
  for (;;) {
    if (xQueueReceive(commands, &next, cmd.enabled ? 0 : pdMS_TO_TICKS(500)) ==
        pdTRUE) {
      int rc = stop();
      cmd = next;
      if (!rc && cmd.enabled) {
        rc = h2_pal_audio_set_speaker_volume_percent(audio,
                                                     AEC_SOURCE_VOLUME_PERCENT);
        uint32_t actual = 0;
        if (!rc) rc = h2_pal_audio_get_speaker_volume_percent(audio, &actual);
        if (!rc && actual != AEC_SOURCE_VOLUME_PERCENT) rc = H2_PAL_ERR_IO;
        if (!rc) rc = h2_pal_audio_start_speaker(audio);
        const h2_audio_track_config_t config = {.name = "aec-source/probe",
                                                .format = format,
                                                .volume_factor_milli = 1000,
                                                .buffer_frames = 4,
                                                .allocator = runtime->mem};
        if (!rc) rc = h2_pal_audio_create_track(audio, &config, &track);
        if (!rc) {
          h2_audio_frame_t frame =
              h2_audio_frame_for_buffer(cmd.pcm, sizeof(cmd.pcm), format);
          frame.bytes = sizeof(cmd.pcm);
          rc = h2_pal_audio_track_write(track, &frame, 500u);
        }
        lease = esp_timer_get_time() + 60000000;
      }
      if (rc) {
        cmd.enabled = 0;
        if (stop() != H2_PAL_OK) esp_restart();
      }
      printf("AEC_SOURCE_CONTROL enabled=%d rc=%d volume=%u\n", cmd.enabled, rc,
             AEC_SOURCE_VOLUME_PERCENT);
      fflush(stdout);
      if (xQueueSend(replies, &rc, pdMS_TO_TICKS(100)) != pdTRUE) esp_restart();
    }
    if (cmd.enabled) {
      h2_audio_frame_t frame =
          h2_audio_frame_for_buffer(cmd.pcm, sizeof(cmd.pcm), format);
      frame.bytes = sizeof(cmd.pcm);
      int rc = h2_pal_audio_track_write(track, &frame, 500u);
      if (rc || esp_timer_get_time() >= lease) {
        printf("AEC_SOURCE_STOP reason=%s rc=%d\n", rc ? "write" : "lease", rc);
        fflush(stdout);
        cmd.enabled = 0;
        if (stop()) esp_restart();
      }
    }
  }
}
static esp_err_t control(httpd_req_t* req) {
  /* A control acknowledgement is only two bytes. Do not leave it queued
   * behind the separately sent HTTP headers while waiting for a TCP ACK. */
  const int nodelay = 1;
  if (setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &nodelay,
                 sizeof(nodelay)) != 0)
    return ESP_FAIL;
  char token[80];
  if (httpd_req_get_hdr_value_str(req, "X-AEC-Token", token, sizeof(token)) !=
          ESP_OK ||
      strcmp(token, H2_AEC_SOURCE_TOKEN)) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "DENIED", 6);
  }
  command_t cmd = {0};
  if (!strcmp(req->uri, "/probe")) {
    if (req->content_len != sizeof(cmd.pcm))
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "Expected 512 S16LE samples at 16kHz");
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (received < sizeof(cmd.pcm)) {
      if (esp_timer_get_time() >= deadline) return ESP_FAIL;
      int n = httpd_req_recv(req, (char*)cmd.pcm + received,
                             sizeof(cmd.pcm) - received);
      if (n <= 0) return ESP_FAIL;
      received += (size_t)n;
    }
    cmd.enabled = 1;
  } else if (req->content_len != 0)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected body");
  int rc = H2_PAL_ERR_TIMEOUT;
  if (xQueueSend(commands, &cmd, pdMS_TO_TICKS(100)) != pdTRUE ||
      xQueueReceive(replies, &rc, pdMS_TO_TICKS(3000)) != pdTRUE) {
    /* No stale acknowledgement can qualify a later request. */
    esp_restart();
    return ESP_FAIL;
  }
  if (rc)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Audio state failed");
  return httpd_resp_send(req, "OK", 2);
}
static void entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config = {0};
  int rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
  audio = config.audio;
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "aec-source", 1u,
                                                   3u);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  const h2_pal_wifi_sta_config_t* wifi = h2_e2e_fixture_wifi();
  rc = wifi ? h2_pal_wifi_sta_connect(runtime->wifi_sta, wifi, 20000u)
            : h2_runtime_wifi_connect_saved(runtime, 20000u);
  if (rc) fail("wifi", rc);
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  if (rc) fail("wifi-power-save", rc);
  rc = h2_pal_audio_get_speaker_volume_percent(audio, &original_volume);
  if (rc) fail("volume", rc);
  commands = xQueueCreate(1, sizeof(command_t));
  replies = xQueueCreate(1, sizeof(int));
  if (!commands || !replies) fail("queues", H2_PAL_ERR_NO_MEMORY);
  if (xTaskCreate(source_worker, "aec-source", 8192, NULL, 5, NULL) != pdPASS)
    fail("worker", H2_PAL_ERR_NO_MEMORY);
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 18081;
  cfg.stack_size = 8192;
  cfg.max_open_sockets = 2;
  cfg.recv_wait_timeout = 2;
  cfg.send_wait_timeout = 2;
  httpd_handle_t server = NULL;
  if (httpd_start(&server, &cfg) != ESP_OK) fail("http", H2_PAL_ERR_IO);
  const httpd_uri_t probe = {
      .uri = "/probe", .method = HTTP_POST, .handler = control};
  const httpd_uri_t stop_uri = {
      .uri = "/stop", .method = HTTP_POST, .handler = control};
  if (httpd_register_uri_handler(server, &probe) != ESP_OK ||
      httpd_register_uri_handler(server, &stop_uri) != ESP_OK)
    fail("routes", H2_PAL_ERR_IO);
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc) fail("confirm", rc);
  h2_pal_wifi_sta_status_t status = {0};
  for (unsigned attempt = 0; attempt < 200u; ++attempt) {
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (!rc && status.ip_valid) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (rc || !status.ip_valid) fail("address", rc ? rc : H2_PAL_ERR_TIMEOUT);
  uint8_t ip[4];
  h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
  printf(
      "AEC_SOURCE_READY board=amoled version=%s ip=%u.%u.%u.%u port=18081 rate=16000 samples=512 volume=%u\n",
      esp_app_get_description()->version, ip[0], ip[1], ip[2], ip[3],
      AEC_SOURCE_VOLUME_PERCENT);
  fflush(stdout);
  hold();
}
void app_main(void) {
  if (H2_AEC_SOURCE_TOKEN[0] == '\0')
    fail("source-token", H2_PAL_ERR_INVALID_ARG);
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("policy", rc);
  rc = h2_esp_board_start_entry_task("amoled/aec-source", entry, NULL);
  if (rc) fail("entry", rc);
}
