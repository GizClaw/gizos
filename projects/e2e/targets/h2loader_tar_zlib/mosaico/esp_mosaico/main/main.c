/* One worker per boot; existing workload registries retain their assertions. */
#include "h2_runtime.h"
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_loader_usb.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#if H2_MOSAICO_SUITE_JSON || H2_MOSAICO_SUITE_CRYPTO
#include "device_runner.h"
#elif H2_MOSAICO_SUITE_AUDIO_DECODER
#include "device_runner.h"
#include "h2_esp_audio_decoder.h"
#elif H2_MOSAICO_SUITE_NET_TLS
#include "h2_pal_net_tls_device.h"
#include "h2_e2e_fixture_wifi.h"
static h2_net_tls_result_t generic_result;
#elif H2_MOSAICO_SUITE_HTTP
#include "h2_pal_http_device.h"
#include "h2_e2e_fixture_wifi.h"
static h2_pal_http_e2e_result_t generic_result;
#elif H2_MOSAICO_SUITE_WEBRTC
#include "h2_pal_webrtc_device.h"
#include "h2_e2e_fixture_wifi.h"
static h2_pal_webrtc_e2e_result_t generic_result;
#elif H2_MOSAICO_SUITE_WIFI
#include "h2_pal_wifi_device.h"
#elif H2_MOSAICO_SUITE_IPV6
#include "h2_pal_ipv6_device.h"
#include "h2_e2e_fixture_wifi.h"
#include "h2_esp_platform_core.h"
static h2_pal_ipv6_result_t generic_result;
#elif H2_MOSAICO_SUITE_ATOMIC
#include "device_runner.h"
#include "h2_esp_platform_core.h"
#include "esp_memory_utils.h"
#elif H2_MOSAICO_SUITE_IPERF
#include "h2_iperf_e2e.h"
#include "h2_sctp.h"
#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#ifndef H2_MOSAICO_IPERF_HOST
#define H2_MOSAICO_IPERF_HOST ""
#endif
#elif H2_MOSAICO_SUITE_IPERF_CLIENT
#include "h2_iperf_client_app.h"
#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#elif H2_MOSAICO_SUITE_LUA_RUNTIME
#include "h2_lua_runtime_e2e.h"
#include "h2_esp_platform_core.h"
#elif H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE
#include "h2_webrtc_performance.h"
#include "esp_heap_caps.h"
#include "h2_esp_platform_core.h"
#include "h2/pal/application/h2_pal_http.h"
#elif H2_MOSAICO_SUITE_STORAGE
#include "device_runner.h"
#elif H2_MOSAICO_SUITE_DISPLAY
#include "device_runner.h"
#include "h2_mosaico_display_capture.h"
#include "bsp/display.h"
#elif H2_MOSAICO_SUITE_AUDIO
#include "h2_pal_audio_e2e.h"
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
#include "esp_system.h"
#endif
#elif H2_MOSAICO_SUITE_LIBCO
#include "h2_libco_smoke.h"
#endif
static h2_runtime_t* runtime;
#if H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE
#ifndef H2_PAL_WEBRTC_OFFER_URL
#define H2_PAL_WEBRTC_OFFER_URL ""
#endif
#ifndef H2_PAL_WEBRTC_STUN_URL
#define H2_PAL_WEBRTC_STUN_URL ""
#endif
static const char perf_offer_url[] = H2_PAL_WEBRTC_OFFER_URL;
static const char perf_stun_url[] = H2_PAL_WEBRTC_STUN_URL;
static h2_webrtc_performance_result_t perf_result;
static int perf_exchange_offer(void* user, h2_pal_webrtc_str_t offer,
                               char* answer, size_t capacity,
                               size_t* answer_len) {
  h2_runtime_t* rt = user;
  static const h2_pal_http_header_t headers[] = {
      {{"Content-Type", sizeof("Content-Type") - 1u},
       {"application/sdp", sizeof("application/sdp") - 1u}},
      {{"X-H2-Reverse-Channels", sizeof("X-H2-Reverse-Channels") - 1u},
       {"0", 1u}},
  };
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {perf_offer_url, sizeof(perf_offer_url) - 1u},
      .headers = headers,
      .header_count = sizeof(headers) / sizeof(headers[0]),
      .body = (const uint8_t*)offer.data,
      .body_len = offer.len,
      .timeout_ms = 20000u,
      .response_buf = (uint8_t*)answer,
      .response_buf_cap = capacity,
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  int rc = h2_pal_http_request(rt->http, &request, &response);
  if (rc || response.status_code != 200 || !response.body ||
      !response.body_len || response.body_len >= capacity)
    return -1;
  if (response.body != (uint8_t*)answer)
    memcpy(answer, response.body, response.body_len);
  answer[response.body_len] = '\0';
  *answer_len = response.body_len;
  return 0;
}
/* Optional per-run AP selection; saved Wi-Fi credentials are unchanged. */
#if defined(H2_MOSAICO_PERF_BSSID)
static int perf_connect_bssid(void) {
  unsigned b[6];
  char tail;
  if (sscanf(H2_MOSAICO_PERF_BSSID, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1],
             &b[2], &b[3], &b[4], &b[5], &tail) != 6)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_wifi_sta_config_t c = {0};
  int rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &c);
  if (rc) return rc;
  for (unsigned i = 0; i < 6; ++i) c.bssid[i] = b[i];
  c.bssid_set = 1;
  c.channel = 0; /* Find the selected AP without assuming its channel. */
  rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &c, 20000u);
  memset(&c, 0, sizeof(c));
  return rc;
}
#endif
static int perf_network(void) {
  if (!perf_offer_url[0] || !perf_stun_url[0]) return H2_PAL_ERR_INVALID_ARG;
#if defined(H2_MOSAICO_PERF_BSSID)
  int rc = perf_connect_bssid();
#else
  int rc = h2_runtime_wifi_connect_saved(runtime, 20000u);
#endif
  if (rc) return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  if (rc) return rc;
  for (;;) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (!rc && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        status.ip_valid && status.ip.ip4 != 0u)
      return H2_PAL_OK;
    if (rc && rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE)
      return rc;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc) return rc;
    if (now - start >= 20000u) return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (rc) return rc;
  }
}
#endif
#if H2_MOSAICO_SUITE_IPERF
static int iperf_network(void) {
  int rc = h2_runtime_wifi_connect_saved(runtime, 20000u);
  if (rc) return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  if (rc) return rc;
  for (;;) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (!rc && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        status.ip_valid && status.ip.ip4 != 0u)
      return H2_PAL_OK;
    if (rc && rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE)
      return rc;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc) return rc;
    if (now - start >= 20000u) return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (rc) return rc;
  }
}
#endif
#if H2_MOSAICO_SUITE_LUA_RUNTIME
static h2_lua_runtime_e2e_report_t lua_report;
static void lua_case_report(void* user,
                            const h2_lua_runtime_e2e_case_result_t* result) {
  (void)user;
  printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n", result->id,
         result->result == H2_PAL_OK ? "PASS" : "FAIL", result->result,
         (unsigned long long)result->evidence);
  fflush(stdout);
}
#endif
#if H2_MOSAICO_SUITE_IPERF_CLIENT || H2_MOSAICO_SUITE_IPERF
static const h2_pal_log_api_t* iperf_board_log;
static int iperf_log_write(void* user, h2_pal_log_level_t level,
                           const char* scope, const char* message) {
  (void)user;
  if (scope && message && strncmp(scope, "iperf", 5u) == 0)
    return printf("%s\n", message) < 0 || fflush(stdout) != 0 ? H2_PAL_ERR_IO
                                                              : H2_PAL_OK;
  return h2_pal_log_write(iperf_board_log, level, scope, message);
}
static const h2_pal_log_vtable_t iperf_log_vtable = {.write = iperf_log_write};
static const h2_pal_log_api_t iperf_log = {.vtable = &iperf_log_vtable};
static void iperf_memory(void* user, const char* checkpoint) {
  (void)user;
  printf(
      "H2_IPERF_CLIENT_MEMORY checkpoint=%s internal_free=%u "
      "internal_min=%u psram_free=%u\n",
      checkpoint,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  fflush(stdout);
}
#endif
#if H2_MOSAICO_SUITE_ATOMIC
static int atomic_current_core(void* unused) {
  (void)unused;
  return (int)xPortGetCoreID();
}
static int atomic_placement(uintptr_t wrapper, uintptr_t storage,
                            bool is_static, void* user) {
  bool psram = *(bool*)user;
  bool valid = (is_static || !psram ? esp_ptr_internal((void*)wrapper)
                                    : esp_ptr_external_ram((void*)wrapper)) &&
               esp_ptr_internal((void*)storage);
  return valid ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
#endif
#if H2_MOSAICO_SUITE_IPV6
static int ipv6_network(void) {
  const h2_pal_wifi_sta_config_t* wifi = h2_e2e_fixture_wifi();
  int rc = wifi ? h2_pal_wifi_sta_connect(runtime->wifi_sta, wifi, 20000u)
                : h2_runtime_wifi_connect_saved(runtime, 20000u);
  if (rc) return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  if (rc) return rc;
  for (;;) {
    h2_pal_net_addr_t local = {0};
    rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                         H2_PAL_NET_FAMILY_IPV6, &local);
    if (!rc && (!h2_pal_ipv6_device_board_fixture() ||
                (local.ip[0] == 0xfd && local.ip[1] == 0x53)))
      return H2_PAL_OK;
    if (rc && rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE)
      return rc;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc) return rc;
    if (now - start >= 20000u) return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (rc) return rc;
  }
}
#endif
#if H2_MOSAICO_SUITE_LIBCO
static const h2_pal_log_api_t* libco_board_log;
/* Keep the maintained libco launcher's result channel visible under the
 * board's normal log filtering policy. */
static int libco_result_write(void* user, h2_pal_log_level_t level,
                              const char* scope, const char* message) {
  (void)user;
  if (scope && message && strcmp(scope, "libco-smoke") == 0)
    return printf("%s\n", message) < 0 || fflush(stdout) != 0 ? H2_PAL_ERR_IO
                                                              : H2_PAL_OK;
  return h2_pal_log_write(libco_board_log, level, scope, message);
}
static const h2_pal_log_vtable_t libco_result_vtable = {.write =
                                                            libco_result_write};
static const h2_pal_log_api_t libco_result_log = {.vtable =
                                                      &libco_result_vtable};
#endif
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
/* Network suites exercise bounded failure paths and multi-minute AP/client
 * leases. Give those cases their own deadline instead of the camera budget. */
#if H2_MOSAICO_SUITE_NET_TLS || H2_MOSAICO_SUITE_HTTP ||      \
    H2_MOSAICO_SUITE_WEBRTC || H2_MOSAICO_SUITE_WIFI ||       \
    H2_MOSAICO_SUITE_IPV6 || H2_MOSAICO_SUITE_IPERF_CLIENT || \
    H2_MOSAICO_SUITE_IPERF || H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE
#define H2_MOSAICO_SUITE_TIMEOUT_S 900u
#else
#define H2_MOSAICO_SUITE_TIMEOUT_S 180u
#endif
static esp_timer_handle_t watchdog;
static void hold(void) {
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("H2_MOSAICO_SETUP stage=%s rc=%d qualified=0\n", stage, rc);
  hold();
}
static void deadline(void* unused) {
  (void)unused;
  printf("H2_MOSAICO_WATCHDOG timeout_s=%u qualified=0 recovery=manual\n",
         H2_MOSAICO_SUITE_TIMEOUT_S);
  fflush(stdout);
}
#endif
#if H2_MOSAICO_SUITE_CAMERA
static h2_pal_camera_frame_t loan;
static int camera_run(void) {
  for (unsigned cycle = 0; cycle < 10; ++cycle) {
    int rc = h2_pal_camera_start(runtime->camera);
    printf("H2_MOSAICO_CAMERA_START cycle=%u rc=%d\n", cycle, rc);
    if (rc) {
      int cleanup = H2_PAL_ERR_IO;
      for (unsigned retry = 0; retry < 3 && cleanup; ++retry)
        cleanup = h2_pal_camera_stop(runtime->camera);
      printf("H2_MOSAICO_CAMERA_START_CLEANUP rc=%d\n", cleanup);
      return rc;
    }
    int first = 0;
    for (unsigned n = 0; n < 30; ++n) {
      int64_t before = esp_timer_get_time();
      rc = h2_pal_camera_acquire(runtime->camera, &loan);
      printf(
          "H2_MOSAICO_CAMERA cycle=%u frame=%u acquire=%d elapsed_us=%" PRId64
          "\n",
          cycle, n, rc, esp_timer_get_time() - before);
      if (rc) {
        first = rc;
        break;
      }
      int valid = loan.data && loan.width == 1280 && loan.height == 720 &&
                  loan.format == H2_PAL_CAMERA_FORMAT_UYVY &&
                  loan.stride >= 2560 &&
                  loan.size >= (size_t)loan.stride * loan.height;
      uint32_t hash = 2166136261u;
      if (valid)
        for (size_t i = 0; i < loan.size; ++i)
          hash = (hash ^ loan.data[i]) * 16777619u;
      printf("H2_MOSAICO_FRAME valid=%d bytes=%zu hash=%08" PRIx32
             " optical_verified=0\n",
             valid, loan.size, hash);
      int busy = h2_pal_camera_stop(runtime->camera);
      for (unsigned retry = 0; retry < 3; ++retry) {
        rc = h2_pal_camera_release(runtime->camera, &loan);
        if (rc)
          printf("H2_MOSAICO_CAMERA_RELEASE_RETRY attempt=%u rc=%d\n",
                 retry + 1, rc);
        if (!rc) break;
      }
      printf("H2_MOSAICO_CAMERA_RELEASE cycle=%u frame=%u busy=%d release=%d\n",
             cycle, n, busy, rc);
      if (rc) return rc; /* Static descriptor and provider remain alive. */
      memset(&loan, 0, sizeof(loan));
      if (!valid || busy != H2_PAL_ERR_BUSY) {
        first = H2_PAL_ERR_IO;
        break;
      }
    }
    for (unsigned retry = 0; retry < 3; ++retry) {
      rc = h2_pal_camera_stop(runtime->camera);
      if (rc)
        printf("H2_MOSAICO_CAMERA_STOP_RETRY attempt=%u rc=%d\n", retry + 1,
               rc);
      if (!rc) break;
    }
    printf("H2_MOSAICO_CAMERA_STOP cycle=%u first=%d stop=%d\n", cycle, first,
           rc);
    if (first || rc) return first ? first : rc;
  }
  return 0;
}
#elif H2_MOSAICO_SUITE_DISPLAY
static uint16_t* transferred;
static size_t chunks;
static int capture_error;
static void capture(void* user, const h2_display_rect_t* r,
                    const uint16_t* pixels) {
  (void)user;
  if (!r || !pixels || r->x < 0 || r->y < 0 || r->width <= 0 ||
      r->height <= 0 || r->x > BSP_LCD_H_RES - r->width ||
      r->y > BSP_LCD_V_RES - r->height) {
    capture_error = 1;
    return;
  }
  const uint8_t* bytes = (const uint8_t*)pixels;
  for (int y = 0; y < r->height; ++y)
    for (int x = 0; x < r->width; ++x) {
      size_t i = ((size_t)y * r->width + x) * 2;
      transferred[(size_t)(r->y + y) * BSP_LCD_H_RES + r->x + x] =
          (uint16_t)((uint16_t)bytes[i] << 8) | bytes[i + 1];
    }
  ++chunks;
}
static int observe(void* user, const uint16_t* expected, int w, int h,
                   uint32_t brightness, const char* id) {
  (void)user;
  int rc =
      capture_error || !chunks || w != BSP_LCD_H_RES || h != BSP_LCD_V_RES
          ? H2_PAL_ERR_IO
          : (memcmp(expected, transferred, (size_t)w * h * 2) ? H2_PAL_ERR_IO
                                                              : 0);
  printf(
      "H2_MOSAICO_DISPLAY case=%s source=completed-SPI-DMA chunks=%zu brightness=%" PRIu32
      " optical_verified=0 rc=%d\n",
      id, chunks, brightness, rc);
  return rc;
}
#elif H2_MOSAICO_SUITE_AUDIO
static void audio_report(void* user, const h2_pal_audio_e2e_case_result_t* r) {
  (void)user;
  printf("H2_MOSAICO_AUDIO case=%s passed=%d blocked=%d detail=%d line=%u\n",
         r->id, r->passed, r->blocked, r->detail, r->line);
  fflush(stdout);
}
/* Separate listening probe; the shared 24-case workload stays unchanged. */
static int audio_listening_probe(void) {
  static int16_t pcm[1024];
  static h2_pal_audio_track_t* track;
  h2_audio_info_t info = {0};
  uint32_t saved_volume = 0;
  int rc = h2_pal_audio_get_info(runtime->audio, &info);
  if (rc) return rc;
  const h2_audio_pcm_format_t format = info.playback_format;
  if (format.sample_rate_hz != 16000 ||
      format.frame_samples_per_channel != 512 ||
      (format.channels != 1 && format.channels != 2) ||
      format.sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_pal_audio_get_speaker_volume_percent(runtime->audio, &saved_volume);
  if (rc) return rc;
  puts("H2_MOSAICO_AUDIO_AUDIBLE phase=prepare wait_ms=5000");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(5000));
  rc = h2_pal_audio_start_speaker(runtime->audio);
  if (rc) return rc;
  rc = h2_pal_audio_set_speaker_volume_percent(runtime->audio, 75);
  const h2_audio_track_config_t config = {
      .name = "mosaico/listening-probe",
      .format = format,
      .volume_factor_milli = 1000,
      .buffer_frames = 4,
  };
  if (!rc) rc = h2_pal_audio_create_track(runtime->audio, &config, &track);
  const size_t samples =
      (size_t)format.frame_samples_per_channel * format.channels;
  for (size_t i = 0; i < samples; ++i) {
    const int phase = (int)((i / format.channels) % 32);
    pcm[i] =
        (int16_t)(phase < 16 ? phase * 512 - 3840 : (31 - phase) * 512 - 3840);
  }
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(pcm, samples * sizeof(pcm[0]), format);
  frame.bytes = frame.capacity;
  unsigned frames = 0;
  while (!rc && frames < 64) {
    rc = h2_pal_audio_track_write(track, &frame, 1000);
    if (!rc) ++frames;
  }
  if (!rc) rc = h2_pal_audio_track_drain(track, 3000);
  int close = 0;
  if (track) {
    for (unsigned retry = 0; retry < 3; ++retry) {
      close = h2_pal_audio_track_close(track);
      if (!close) {
        track = NULL;
        break;
      }
      if (close != H2_AUDIO_ERR_WOULD_BLOCK && close != H2_PAL_ERR_TIMEOUT)
        break;
    }
  }
  const int stop =
      track ? H2_PAL_ERR_BUSY : h2_pal_audio_stop_speaker(runtime->audio);
  const int restore =
      h2_pal_audio_set_speaker_volume_percent(runtime->audio, saved_volume);
  printf(
      "H2_MOSAICO_AUDIO_AUDIBLE frequency_hz=500 frames=%u peak=3840 volume=75 rc=%d close=%d stop=%d restore=%d acoustic_verified=0\n",
      frames, rc, close, stop, restore);
  return rc ? rc : close ? close : stop ? stop : restore;
}

#endif
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
static void run(void* unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(5000));
  int rc = H2_PAL_ERR_UNSUPPORTED;
#if H2_MOSAICO_SUITE_NET_TLS || H2_MOSAICO_SUITE_HTTP || \
    H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE || H2_MOSAICO_SUITE_IPERF
  /* Keep this latency-sensitive fixture workload awake across short TCP/TLS
   * deadlines. This is a test-entry policy, not a Runtime-wide power policy. */
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  if (rc) fail("network-power-save", rc);
  puts("H2_MOSAICO_NETWORK power_save=none");
#endif
#if H2_MOSAICO_SUITE_JSON
  rc = h2_json_device_run(runtime, esp_app_get_description()->version);
  h2_json_device_replay(runtime);
#elif H2_MOSAICO_SUITE_CRYPTO
  rc = h2_crypto_device_run(runtime, esp_app_get_description()->version);
  h2_crypto_device_replay(runtime);
#elif H2_MOSAICO_SUITE_AUDIO_DECODER
  rc = h2_adec_device_run(runtime, esp_app_get_description()->version);

#elif H2_MOSAICO_SUITE_NET_TLS
  rc = h2_pal_net_tls_device_run_with_wifi(runtime, h2_e2e_fixture_wifi(),
                                           &generic_result);
  h2_pal_net_tls_device_report(runtime, &generic_result);
#elif H2_MOSAICO_SUITE_HTTP
  rc = h2_pal_http_device_run_with_wifi(runtime, h2_e2e_fixture_wifi(),
                                        &generic_result);
  h2_pal_http_device_report(runtime, &generic_result);
#elif H2_MOSAICO_SUITE_WEBRTC
  rc = h2_pal_webrtc_device_run_with_wifi(runtime, h2_e2e_fixture_wifi(),
                                          20000u, 0u, &generic_result);
  h2_pal_webrtc_device_report(runtime, &generic_result);
#elif H2_MOSAICO_SUITE_WIFI
  rc = h2_wifi_device_run(runtime, esp_app_get_description()->version,
                          "esp_mosaico");
#elif H2_MOSAICO_SUITE_IPV6
  rc = ipv6_network();
  if (!rc)
    rc = h2_pal_ipv6_device_run(runtime, h2_esp_platform_dtls_api(),
                                &generic_result);
  h2_pal_ipv6_device_report(runtime, &generic_result);
#elif H2_MOSAICO_SUITE_ATOMIC
  h2_atomic_qualification_config_t atomic_config = {
      .mem = h2_esp_platform_internal_allocator(),
      .task = runtime->task,
      .time = runtime->time,
      .current_core = atomic_current_core,
      .expected_core = {0, 1},
      .require_distinct_workers = true,
      .check_placement = atomic_placement,
  };
  rc = h2_atomic_device_run(runtime, esp_app_get_description()->version,
                            &atomic_config, h2_esp_platform_psram_allocator());
#elif H2_MOSAICO_SUITE_IPERF
  if (!H2_MOSAICO_IPERF_HOST[0])
    fail("iperf-peer-input", H2_PAL_ERR_INVALID_ARG);
  rc = iperf_network();
  if (rc) fail("iperf-wifi", rc);
  h2_sctp_t* sctp = NULL;
  const h2_sctp_config_t sctp_config = {.mem = runtime->mem,
                                        .crypto = runtime->crypto};
  rc = h2_sctp_create(&sctp_config, &sctp);
  if (rc) fail("iperf-sctp", rc);
  const h2_iperf_e2e_config_t config = {
      .pal = {.mem = runtime->mem,
              .net = runtime->net,
              .time = runtime->time,
              .crypto = runtime->crypto,
              .sctp = h2_sctp_api(sctp),
              .log = runtime->log},
      .target = "mosaico",
      .server_host = H2_MOSAICO_IPERF_HOST,
      .duration_ms = 5000u,
      .checkpoint = iperf_memory,
  };
  h2_iperf_e2e_report_t report = {0};
  rc = h2_iperf_e2e_run(&config, &report);
  const int cleanup = h2_sctp_destroy(&sctp);
  if (!rc) rc = cleanup;
#elif H2_MOSAICO_SUITE_IPERF_CLIENT
  printf("H2_IPERF_CLIENT_BOOT board=esp_mosaico version=%s\n",
         esp_app_get_description()->version);
  iperf_memory(NULL, "matrix-start");
  rc = h2_iperf_client_app_bench(runtime, "mosaico", iperf_memory, NULL);
  iperf_memory(NULL, "matrix-end");
#elif H2_MOSAICO_SUITE_LUA_RUNTIME
  printf("H2_LUA_E2E_START board=esp_mosaico scheduler=multi-worker total=%u\n",
         (unsigned)H2_LUA_RUNTIME_E2E_CASE_COUNT);
  rc = h2_lua_runtime_e2e_run(runtime,
                              &(h2_lua_runtime_e2e_config_t){
                                  .scheduler = "multi-worker",
                                  .worker_count = 2u,
                                  .report_case = lua_case_report,
                              },
                              &lua_report);
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%u total=%u\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL",
         lua_report.scheduler ? lua_report.scheduler : "unknown",
         (unsigned)lua_report.passed, (unsigned)lua_report.case_count);
#elif H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE
  rc = perf_network();
  if (!rc) {
    const h2_webrtc_performance_config_t config = {
        .profile = "smoke",
        .target = "esp_mosaico",
        .provider = "h2peer",
        .stun_url = {perf_stun_url, sizeof(perf_stun_url) - 1u},
        .exchange_offer = perf_exchange_offer,
        .exchange_offer_user = runtime,
    };
    rc = h2_webrtc_performance_run(runtime, &config, &perf_result);
  }
  printf(
      "H2_WEBRTC_PERF_MOSAICO result=%s rc=%d iterations=%u "
      "upload_Bps=%llu download_Bps=%llu loaded_Bps=%llu\n",
      rc == H2_PAL_OK ? "PASS" : "FAIL", rc, perf_result.iterations,
      (unsigned long long)perf_result.median_upload_bytes_per_second,
      (unsigned long long)perf_result.median_download_bytes_per_second,
      (unsigned long long)perf_result.median_loaded_bytes_per_second);
#elif H2_MOSAICO_SUITE_STORAGE
  rc = h2_storage_device_run(runtime, "/data/mosaico-e2e",
                             esp_app_get_description()->version);
#elif H2_MOSAICO_SUITE_LIBCO
  BaseType_t root_core = xTaskGetCoreID(NULL);
  if (root_core == tskNO_AFFINITY || root_core != xPortGetCoreID())
    fail("libco-root-core", H2_PAL_ERR_INVALID_STATE);
  rc = h2_libco_smoke_run(runtime, NULL);
  if (!rc &&
      (root_core != xTaskGetCoreID(NULL) || root_core != xPortGetCoreID()))
    rc = H2_PAL_ERR_INVALID_STATE;
#elif H2_MOSAICO_SUITE_CAMERA
  rc = camera_run();
#elif H2_MOSAICO_SUITE_AUDIO
  static h2_pal_audio_e2e_result_t result;
  const h2_pal_audio_e2e_config_t config = {.audio = runtime->audio,
                                            .time = runtime->time,
                                            .stability_ms = 30000,
                                            .report = audio_report};
  rc = h2_pal_audio_e2e_run(&config, &result);
  printf(
      "H2_MOSAICO_AUDIO_SUMMARY passed=%u failed=%u blocked=%u mic_frames=%" PRIu32
      " mic_peak=%" PRIu32 " acoustic_verified=0\n",
      result.passed, result.failed, result.blocked, result.mic_frames,
      result.mic_peak);
  if (!rc) rc = audio_listening_probe();
#elif H2_MOSAICO_SUITE_DISPLAY
  transferred =
      h2_pal_mem_alloc(runtime->mem, (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
  if (!transferred) fail("capture-memory", H2_PAL_ERR_NO_MEMORY);
  memset(transferred, 0, (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
  h2_mosaico_display_set_transfer_capture(capture, NULL);
  static h2_pal_display_e2e_result_t result;
  const h2_pal_display_e2e_config_t config = {
      .supported_formats = 1u << H2_DISPLAY_PIXEL_RGB565,
      .clips_rectangles = 0,
      .observe = observe};
  rc = h2_pal_display_e2e_run(runtime, &config, &result);
  h2_mosaico_display_set_transfer_capture(NULL, NULL);
  h2_pal_mem_free(runtime->mem, transferred);
  transferred = NULL;
  if (!rc) rc = h2_display_device_demo(runtime);
  if (!rc) rc = h2_display_device_show_pattern(runtime);
  printf("H2_MOSAICO_DISPLAY_STABLE rc=%d optical_verified=0\n", rc);
#endif
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
#if H2_MOSAICO_SUITE_AUDIO
  if (rc) {
    /* A failed close may leave a borrowed track attached. Preserve the ledger,
     * then reset the unconfirmed trial instead of leaving playback active. */
    for (unsigned i = 0; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i)
      audio_report(NULL, &result.cases[i]);
    printf(
        "H2_MOSAICO_E2E_DONE rc=%d confirm=not-attempted software_pass=0 physical_qualified=0 recovery=reset\n",
        rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
#endif
  int confirm =
      rc ? H2_PAL_ERR_INVALID_STATE : h2_esp_h2loader_app_confirm(runtime);
#if H2_MOSAICO_SUITE_ATOMIC
  h2_atomic_device_admit(rc, confirm);
#endif
  for (;;) {
    printf(
        "H2_MOSAICO_E2E_DONE rc=%d confirm=%d software_pass=%d physical_qualified=0\n",
        rc, confirm, !rc);
#if H2_MOSAICO_SUITE_STORAGE
    h2_storage_device_replay(runtime);
#elif H2_MOSAICO_SUITE_AUDIO
    for (unsigned i = 0; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i)
      audio_report(NULL, &result.cases[i]);
#elif H2_MOSAICO_SUITE_DISPLAY
    h2_pal_display_e2e_print(&result, "esp_mosaico", rc, 0);
#elif H2_MOSAICO_SUITE_ATOMIC
    h2_atomic_device_replay();
#endif
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("task-policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc) fail("usb-console", rc);
  rc = h2_mosaico_loader_usb_init();
  if (rc) fail("usb-transport", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
#if H2_MOSAICO_SUITE_AUDIO_DECODER
  config.audio_decoder = h2_esp_audio_decoder_api();
#endif
#if H2_MOSAICO_SUITE_IPERF_CLIENT || H2_MOSAICO_SUITE_IPERF
  config.mem = h2_esp_platform_psram_allocator();
  iperf_board_log = config.log;
  config.log = &iperf_log;
#endif
#if H2_MOSAICO_SUITE_LUA_RUNTIME
  config.mem = h2_esp_platform_psram_allocator();
  config.periph = h2_lua_runtime_e2e_periph_api();
  config.component_mapper = h2_lua_runtime_e2e_component_mapper();
  config.event_queue_capacity = 64;
#endif
#if H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE
  config.mem = h2_esp_platform_psram_allocator();
#endif
#if H2_MOSAICO_SUITE_LIBCO
  libco_board_log = config.log;
  config.log = &libco_result_log;
#endif
#if H2_MOSAICO_SUITE_WIFI
  config.event_queue_capacity = 64;
#elif !(H2_MOSAICO_SUITE_NET_TLS || H2_MOSAICO_SUITE_HTTP ||       \
        H2_MOSAICO_SUITE_WEBRTC || H2_MOSAICO_SUITE_IPV6 ||        \
        H2_MOSAICO_SUITE_IPERF_CLIENT || H2_MOSAICO_SUITE_IPERF || \
        H2_MOSAICO_SUITE_LUA_RUNTIME || H2_MOSAICO_SUITE_WEBRTC_PERFORMANCE)
  config.system_event = h2_pal_unsupported_system_event_api();
#endif
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "mosaico-e2e",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "mosaico-e2e"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(
          watchdog, (uint64_t)H2_MOSAICO_SUITE_TIMEOUT_S * 1000000u) != ESP_OK)
    fail("watchdog", H2_PAL_ERR_IO);
  printf("H2_MOSAICO_E2E_BOOT version=%s board=esp_mosaico\n",
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {.name = "mosaico/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("worker", rc);
}

#endif
