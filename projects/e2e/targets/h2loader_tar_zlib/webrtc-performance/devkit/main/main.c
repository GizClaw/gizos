#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_webrtc_performance.h"
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_esp_layout_task_policy.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/* Operator-selected LAN endpoints are injected by the Bazel firmware rule. */
#ifndef H2_WEBRTC_PERF_ENDPOINT
#define H2_WEBRTC_PERF_ENDPOINT "http://192.0.2.1:18080"
#endif

#ifndef H2_WEBRTC_PERF_STUN_URL
#define H2_WEBRTC_PERF_STUN_URL "stun:192.0.2.1:3478"
#endif

#ifndef H2_WEBRTC_PERF_WIFI_SSID
#define H2_WEBRTC_PERF_WIFI_SSID ""
#endif

#ifndef H2_WEBRTC_PERF_WIFI_PASSWORD
#define H2_WEBRTC_PERF_WIFI_PASSWORD ""
#endif

typedef struct h2_webrtc_performance_devkit_context {
  h2_runtime_t *runtime;
  char offer_url[192];
} h2_webrtc_performance_devkit_context_t;

static void hold(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void memory_checkpoint(const char *name) {
  printf("H2_WEBRTC_PERF_MEMORY checkpoint=%s internal_free_kib=%u "
         "internal_min_kib=%u dma_free_kib=%u dma_min_kib=%u "
         "psram_free_kib=%u psram_min_kib=%u\n",
         name,
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                            MALLOC_CAP_8BIT) /
                    1024u),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT) /
                    1024u),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT) /
                    1024u),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_DMA |
                                                    MALLOC_CAP_8BIT) /
                    1024u),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT) /
                    1024u),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM |
                                                    MALLOC_CAP_8BIT) /
                    1024u));
  fflush(stdout);
}

static void benchmark_checkpoint(void *user, const char *name) {
  (void)user;
  memory_checkpoint(name);
}

static int exchange_offer(void *raw, h2_pal_webrtc_str_t offer, char *answer,
                          size_t answer_capacity, size_t *answer_len) {
  h2_webrtc_performance_devkit_context_t *context = raw;
  static const h2_pal_http_header_t headers[] = {
      {{"Content-Type", sizeof("Content-Type") - 1u},
       {"application/sdp", sizeof("application/sdp") - 1u}},
      {{"X-H2-Reverse-Channels", sizeof("X-H2-Reverse-Channels") - 1u},
       {"0", 1u}},
  };
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {context->offer_url, strlen(context->offer_url)},
      .headers = headers,
      .header_count = sizeof(headers) / sizeof(headers[0]),
      .body = (const uint8_t *)offer.data,
      .body_len = offer.len,
      .timeout_ms = 20000,
      .retry_count = 0,
      .response_buf = (uint8_t *)answer,
      .response_buf_cap = answer_capacity,
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  const int rc = h2_pal_http_request(context->runtime->http, &request, &response);
  if (rc != H2_PAL_OK || response.status_code != 200 || response.body == NULL ||
      response.body_len == 0u || response.body_len >= answer_capacity) {
    printf("H2_WEBRTC_PERF stage=signaling status=ERROR rc=%d http=%d len=%zu\n",
           rc, response.status_code, response.body_len);
    return -1;
  }
  if (response.body != (uint8_t *)answer) {
    memcpy(answer, response.body, response.body_len);
  }
  answer[response.body_len] = '\0';
  *answer_len = response.body_len;
  return 0;
}

static void fail(const char *stage, int rc, int commands_started) {
  printf("H2_WEBRTC_PERF_DEVKIT stage=%s status=ERROR rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_started) {
    esp_restart();
  }
  hold();
}

static void wait_for_wifi(h2_runtime_t *runtime,
                          const h2_pal_wifi_sta_config_t *wifi) {
  for (;;) {
    const int rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, wifi, 20000u);
    if (rc == H2_PAL_OK) {
      const TickType_t deadline =
          xTaskGetTickCount() + pdMS_TO_TICKS(20000u);
      do {
        h2_pal_wifi_sta_status_t status = {0};
        const int status_rc =
            h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
        if (status_rc == H2_PAL_OK &&
            status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
            status.ip.ip4 != 0u) {
          uint8_t ip[4];
          h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
          printf("H2_WEBRTC_PERF_DEVKIT stage=wifi status=READY "
                 "ip=%u.%u.%u.%u\n",
                 ip[0], ip[1], ip[2], ip[3]);
          fflush(stdout);
          return;
        }
        if (status_rc != H2_PAL_OK ||
            status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED ||
            status.state == H2_PAL_WIFI_STA_STATE_FAILED) {
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(100u));
      } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    }
    printf("H2_WEBRTC_PERF_DEVKIT stage=wifi status=WAIT rc=%d "
           "retry_ms=3000\n",
           rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
}

static void image_entry(void *user) {
  (void)user;
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  memory_checkpoint("entry");
  int rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    fail("runtime_config", rc, 0);
  }
  rc = h2_esp_h2loader_app_commands_prepare_serial(
      &runtime_config, "webrtc-performance", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_prepare", rc, 0);
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    fail("runtime_init", rc, 0);
  }
  rc = h2_esp_h2loader_app_commands_start(runtime, "webrtc-performance", 1u,
                                           3u);
  if (rc != H2_PAL_OK) {
    fail("command_start", rc, 0);
  }
  if (strcmp(H2_WEBRTC_PERF_ENDPOINT, "http://192.0.2.1:18080") == 0 ||
      strcmp(H2_WEBRTC_PERF_STUN_URL, "stun:192.0.2.1:3478") == 0 ||
      H2_WEBRTC_PERF_WIFI_SSID[0] == '\0' ||
      H2_WEBRTC_PERF_WIFI_PASSWORD[0] == '\0') {
    fail("endpoint_config", H2_PAL_ERR_INVALID_ARG, 1);
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) {
    fail("confirm", rc, 1);
  }
  const h2_pal_wifi_sta_config_t wifi = {
      .ssid = H2_WEBRTC_PERF_WIFI_SSID,
      .ssid_len = sizeof(H2_WEBRTC_PERF_WIFI_SSID) - 1u,
      .password = H2_WEBRTC_PERF_WIFI_PASSWORD,
      .password_len = sizeof(H2_WEBRTC_PERF_WIFI_PASSWORD) - 1u,
  };
  wait_for_wifi(runtime, &wifi);
  memory_checkpoint("wifi_ready");
  h2_webrtc_performance_devkit_context_t context = {.runtime = runtime};
  const int url_len = snprintf(context.offer_url, sizeof(context.offer_url),
                               "%s/offer", H2_WEBRTC_PERF_ENDPOINT);
  if (url_len <= 0 || (size_t)url_len >= sizeof(context.offer_url)) {
    fail("endpoint", H2_PAL_ERR_INVALID_ARG, 1);
  }
  const h2_webrtc_performance_config_t config = {
      .profile = "smoke",
      .target = "devkit",
      .provider = "h2peer",
      .stun_url = {H2_WEBRTC_PERF_STUN_URL,
                   sizeof(H2_WEBRTC_PERF_STUN_URL) - 1u},
      .exchange_offer = exchange_offer,
      .exchange_offer_user = &context,
      .checkpoint = benchmark_checkpoint,
  };
  for (;;) {
    h2_webrtc_performance_result_t result = {0};
    memory_checkpoint("benchmark_start");
    rc = h2_webrtc_performance_run(runtime, &config, &result);
    memory_checkpoint("benchmark_end");
    printf("H2_WEBRTC_PERF_DEVKIT stage=summary status=%s rc=%d "
           "upload_Bps=%llu download_Bps=%llu loaded_Bps=%llu "
           "request_batch_ns=%llu audio_p99_gap_ns=%llu\n",
           rc == 0 ? "PASS" : "FAIL", rc,
           (unsigned long long)result.median_upload_bytes_per_second,
           (unsigned long long)result.median_download_bytes_per_second,
           (unsigned long long)result.median_loaded_bytes_per_second,
           (unsigned long long)result.median_request_batch_ns,
           (unsigned long long)result.median_audio_p99_gap_ns);
    fflush(stdout);
    if (rc == 0) {
      break;
    }
    printf("H2_WEBRTC_PERF_DEVKIT stage=retry wait_ms=3000\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
  hold();
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
  const h2_pal_result_t rc = h2_esp_board_start_entry_task(
      "devkit/webrtc-performance", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    fail("entry_task", rc, 0);
  }
}
