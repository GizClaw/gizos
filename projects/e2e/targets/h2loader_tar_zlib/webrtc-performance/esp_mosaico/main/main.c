#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/idf_additions.h"
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mosaico_e2e_screen.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_webrtc_performance.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#ifndef H2_WEBRTC_PERF_MBOX_DIAGNOSTICS
#define H2_WEBRTC_PERF_MBOX_DIAGNOSTICS 0
#endif
#if H2_WEBRTC_PERF_MBOX_DIAGNOSTICS
#include "lwip/sys.h"
#include <stdint.h>
static uintptr_t diagnostic_udp_mbox;
static unsigned int diagnostic_udp_created, diagnostic_udp_freed;
static unsigned int diagnostic_udp_posts, diagnostic_udp_drops;
extern err_t __real_sys_mbox_new(sys_mbox_t *mbox, int size);
extern err_t __real_sys_mbox_trypost(sys_mbox_t *mbox, void *message);
extern void __real_sys_mbox_free(sys_mbox_t *mbox);
err_t __wrap_sys_mbox_new(sys_mbox_t *mbox, int size) {
  const err_t result = __real_sys_mbox_new(mbox, size);
  if (result == ERR_OK && size == CONFIG_LWIP_UDP_RECVMBOX_SIZE &&
      strcmp(pcTaskGetName(NULL), "$h2peer/net") == 0) {
    __atomic_store_n(&diagnostic_udp_mbox, (uintptr_t)*mbox, __ATOMIC_RELEASE);
    __atomic_fetch_add(&diagnostic_udp_created, 1u, __ATOMIC_RELAXED);
  }
  return result;
}
err_t __wrap_sys_mbox_trypost(sys_mbox_t *mbox, void *message) {
  const int target = (uintptr_t)*mbox ==
      __atomic_load_n(&diagnostic_udp_mbox, __ATOMIC_ACQUIRE);
  const err_t result = __real_sys_mbox_trypost(mbox, message);
  if (target) {
    __atomic_fetch_add(&diagnostic_udp_posts, 1u, __ATOMIC_RELAXED);
    if (result != ERR_OK)
      __atomic_fetch_add(&diagnostic_udp_drops, 1u, __ATOMIC_RELAXED);
  }
  return result;
}
void __wrap_sys_mbox_free(sys_mbox_t *mbox) {
  if ((uintptr_t)*mbox == __atomic_load_n(&diagnostic_udp_mbox, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&diagnostic_udp_mbox, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&diagnostic_udp_freed, 1u, __ATOMIC_RELAXED);
  }
  __real_sys_mbox_free(mbox);
}
static void diagnostic_udp_snapshot(void) {
  printf("H2_WEBRTC_PERF_UDP_MBOX created=%u freed=%u posts=%u drops=%u\n",
      __atomic_load_n(&diagnostic_udp_created, __ATOMIC_RELAXED),
      __atomic_load_n(&diagnostic_udp_freed, __ATOMIC_RELAXED),
      __atomic_load_n(&diagnostic_udp_posts, __ATOMIC_RELAXED),
      __atomic_load_n(&diagnostic_udp_drops, __ATOMIC_RELAXED));
  fflush(stdout);
}
#endif

/* Operator-selected LAN endpoints are injected by the Bazel firmware rule. */
#ifndef H2_WEBRTC_PERF_ENDPOINT
#define H2_WEBRTC_PERF_ENDPOINT "http://192.0.2.1:18080"
#endif

#ifndef H2_WEBRTC_PERF_STUN_URL
#define H2_WEBRTC_PERF_STUN_URL "stun:192.0.2.1:3478"
#endif

/* 0 keeps the radio awake, 1 sleeps per DTIM, 2 sleeps per listen interval. */
#ifndef H2_WEBRTC_PERF_POWER_SAVE
#define H2_WEBRTC_PERF_POWER_SAVE 0
#endif

/* 1 keeps the H2Loader App command service advertising over BLE, 0 pauses it
 * before the workload so BLE/Wi-Fi coexistence does not time-slice the radio.
 */
#ifndef H2_WEBRTC_PERF_BLE_ADV
#define H2_WEBRTC_PERF_BLE_ADV 1
#endif

#ifndef H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS
#define H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS 0
#endif

#if H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS < 0 || H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS > 60000
#error "network preflight must be between 0 and 60000 ms"
#endif

typedef struct h2_webrtc_performance_esp_mosaico_context {
  h2_runtime_t *runtime;
  char offer_url[192];
} h2_webrtc_performance_esp_mosaico_context_t;

static void report_rx_policy(void) {
#ifdef CONFIG_LWIP_L2_TO_L3_COPY
  const int copy = 1;
#else
  const int copy = 0;
#endif
#ifdef CONFIG_ESP_COEX_SW_COEXIST_ENABLE
  const int coexist = 1;
#else
  const int coexist = 0;
#endif
  printf("H2_WEBRTC_PERF_MOSAICO stage=sdk_rx copy=%d udp_mbox=%d coex_sw=%d\n",
         copy, CONFIG_LWIP_UDP_RECVMBOX_SIZE, coexist);
  printf("H2_WEBRTC_PERF_MOSAICO stage=cpu_layout cores=%d tcpip_core=%d\n",
         configNUMBER_OF_CORES, CONFIG_LWIP_TCPIP_TASK_AFFINITY);
  fflush(stdout);
}

static void hold(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void memory_checkpoint(const char *name) {
  printf(
      "H2_WEBRTC_PERF_MEMORY checkpoint=%s internal_free_kib=%u "
      "internal_min_kib=%u dma_free_kib=%u dma_min_kib=%u "
      "psram_free_kib=%u psram_min_kib=%u "
      "internal_largest_kib=%u dma_largest_kib=%u\n",
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
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) /
                 1024u),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT) /
                 1024u),
      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT) /
                 1024u),
      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA |
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
  h2_webrtc_performance_esp_mosaico_context_t *context = raw;
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
  const int rc =
      h2_pal_http_request(context->runtime->http, &request, &response);
  if (rc != H2_PAL_OK || response.status_code != 200 || response.body == NULL ||
      response.body_len == 0u || response.body_len >= answer_capacity) {
    printf(
        "H2_WEBRTC_PERF stage=signaling status=ERROR rc=%d http=%d len=%zu\n",
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
  (void)h2_mosaico_e2e_screen_result(rc, H2_PAL_ERR_INVALID_STATE);
  printf("H2_WEBRTC_PERF_MOSAICO stage=%s status=ERROR rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_started) {
    esp_restart();
  }
  hold();
}

#ifndef H2_WEBRTC_PERF_WIFI_BSSID_HEX
#define H2_WEBRTC_PERF_WIFI_BSSID_HEX ""
#endif

/* The image never carries credentials: the Loader's `wifi connect` stores the
 * STA configuration and the App reconnects with it. */
static int load_wifi_config(h2_runtime_t *runtime,
                            h2_pal_wifi_sta_config_t *out_config) {
  int has_config = 0;
  int rc = h2_pal_wifi_settings_has_saved_sta_config(runtime->wifi_settings,
                                                     &has_config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (!has_config) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings,
                                                 out_config);
  if (rc != H2_PAL_OK)
    return rc;
  /* A private test fixture can select an AP without changing saved settings. */
  const char *bssid = H2_WEBRTC_PERF_WIFI_BSSID_HEX;
  if (bssid[0] == '\0')
    return H2_PAL_OK;
  if (strlen(bssid) != 12u)
    return H2_PAL_ERR_INVALID_ARG;
  unsigned int nonzero = 0u;
  memset(out_config->bssid, 0, sizeof(out_config->bssid));
  for (size_t i = 0u; i < 12u; ++i) {
    const char c = bssid[i];
    const int nibble = c >= '0' && c <= '9' ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                     : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (nibble < 0)
      return H2_PAL_ERR_INVALID_ARG;
    out_config->bssid[i / 2u] |= (uint8_t)(nibble << (i % 2u == 0u ? 4u : 0u));
    nonzero |= (unsigned int)nibble;
  }
  if (nonzero == 0u || (out_config->bssid[0] & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  out_config->bssid_set = 1u;
  out_config->channel = 0u;
  return H2_PAL_OK;
}

/* The selected policy must be in effect before any measurement, otherwise
 * the summary would label results with a mode that was never applied. */
static int apply_power_save(h2_runtime_t *runtime) {
  const h2_pal_wifi_power_save_t mode =
      (h2_pal_wifi_power_save_t)H2_WEBRTC_PERF_POWER_SAVE;
  const int rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta, mode);
  printf("H2_WEBRTC_PERF_MOSAICO stage=power_save mode=%d rc=%d\n", (int)mode,
         rc);
  fflush(stdout);
  return rc;
}

static void wait_for_wifi(h2_runtime_t *runtime,
                          const h2_pal_wifi_sta_config_t *wifi) {
  for (;;) {
    const int rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, wifi, 20000u);
    if (rc == H2_PAL_OK) {
      const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(20000u);
      do {
        h2_pal_wifi_sta_status_t status = {0};
        const int status_rc =
            h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
        if (status_rc == H2_PAL_OK &&
            status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
            status.ip.ip4 != 0u) {
          if (wifi->bssid_set &&
              (!status.bssid_set || memcmp(wifi->bssid, status.bssid, 6u) != 0))
            fail("wifi_bssid", H2_PAL_ERR_INVALID_STATE, 1);
          printf("H2_WEBRTC_PERF_MOSAICO stage=wifi_link "
                 "bssid=%02x%02x%02x%02x%02x%02x channel=%u rssi=%d pinned=%u\n",
                 status.bssid[0], status.bssid[1], status.bssid[2],
                 status.bssid[3], status.bssid[4], status.bssid[5],
                 (unsigned int)status.channel, status.rssi,
                 (unsigned int)wifi->bssid_set);
          uint8_t ip[4];
          h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
          printf("H2_WEBRTC_PERF_MOSAICO stage=wifi status=READY "
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
    printf("H2_WEBRTC_PERF_MOSAICO stage=wifi status=WAIT rc=%d "
           "retry_ms=3000\n",
           rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
}

/* The shared runner owns 20-ms media pacing. Keep it above the priority-8
 * command service while preserving the higher transport/lwIP worker order. */
#define H2_WEBRTC_PERF_ENTRY_TASK_PRIORITY (tskIDLE_PRIORITY + 9u)

static void image_entry(void *user) {
  (void)user;
  vTaskPrioritySet(NULL, H2_WEBRTC_PERF_ENTRY_TASK_PRIORITY);
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  memory_checkpoint("entry");
  int rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    fail("runtime_config", rc, 0);
  }
  /* Data buffers (iperf blocks, h2sctp windows, H2Peer slots and mailboxes)
   * live in PSRAM; internal RAM stays for Wi-Fi, lwIP and task stacks. */
  runtime_config.mem = h2_esp_platform_psram_allocator();
  rc = h2_mosaico_e2e_screen_begin(runtime_config.display, runtime_config.log,
                                   "WEBRTC-PERFORMANCE", 18u);
  if (rc != H2_PAL_OK)
    fail("screen", rc, 0);
  rc = h2_esp_h2loader_app_commands_prepare_serial(
      &runtime_config, "webrtc-performance", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_prepare", rc, 0);
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    fail("runtime_init", rc, 0);
  }
  rc =
      h2_esp_h2loader_app_commands_start(runtime, "webrtc-performance", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_start", rc, 0);
  }
  if (strcmp(H2_WEBRTC_PERF_ENDPOINT, "http://192.0.2.1:18080") == 0 ||
      strcmp(H2_WEBRTC_PERF_STUN_URL, "stun:192.0.2.1:3478") == 0) {
    fail("endpoint_config", H2_PAL_ERR_INVALID_ARG, 1);
  }
  h2_pal_wifi_sta_config_t wifi;
  memset(&wifi, 0, sizeof(wifi));
  rc = load_wifi_config(runtime, &wifi);
  if (rc != H2_PAL_OK) {
    fail("wifi_settings", rc, 1);
  }
  (void)h2_mosaico_e2e_screen_phase("WAITING FOR WIFI");
  wait_for_wifi(runtime, &wifi);
  printf("H2_WEBRTC_PERF_MOSAICO stage=entry_priority priority=%u\n",
         (unsigned)uxTaskPriorityGet(NULL));
  fflush(stdout);
  rc = apply_power_save(runtime);
  if (rc != H2_PAL_OK) {
    fail("power_save", rc, 1);
  }
  if (!H2_WEBRTC_PERF_BLE_ADV) {
    rc = h2_esp_h2loader_app_commands_pause_ble_advertising();
    if (rc != H2_PAL_OK) {
      fail("ble_adv", rc, 1);
    }
  }
  printf("H2_WEBRTC_PERF_MOSAICO stage=ble_adv keep=%d rc=%d\n",
         (int)H2_WEBRTC_PERF_BLE_ADV, rc);
  fflush(stdout);
  report_rx_policy();
  memory_checkpoint("wifi_ready");
#if H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS > 0
  /* Independent host probes run before any measured WebRTC workload. */
  printf("H2_WEBRTC_PERF_MOSAICO stage=network_preflight status=START duration_ms=%d\n",
         H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS);
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS));
  printf("H2_WEBRTC_PERF_MOSAICO stage=network_preflight status=END\n");
  fflush(stdout);
#endif
  h2_webrtc_performance_esp_mosaico_context_t context = {.runtime = runtime};
  const int url_len = snprintf(context.offer_url, sizeof(context.offer_url),
                               "%s/offer", H2_WEBRTC_PERF_ENDPOINT);
  if (url_len <= 0 || (size_t)url_len >= sizeof(context.offer_url)) {
    fail("endpoint", H2_PAL_ERR_INVALID_ARG, 1);
  }
  const h2_webrtc_performance_config_t config = {
      .profile = "smoke",
      .target = "esp_mosaico",
      .provider = "h2peer",
      .stun_url = {H2_WEBRTC_PERF_STUN_URL,
                   sizeof(H2_WEBRTC_PERF_STUN_URL) - 1u},
      .exchange_offer = exchange_offer,
      .exchange_offer_user = &context,
      .checkpoint = benchmark_checkpoint,
  };
  (void)h2_mosaico_e2e_screen_phase("RUNNING WEBRTC WORKLOAD");
  h2_webrtc_performance_result_t result = {0};
  memory_checkpoint("benchmark_start");
  rc = h2_webrtc_performance_run(runtime, &config, &result);
  memory_checkpoint("benchmark_end");
#if H2_WEBRTC_PERF_MBOX_DIAGNOSTICS
  diagnostic_udp_snapshot();
#endif
  if (rc == H2_PAL_OK)
    (void)h2_mosaico_e2e_screen_phase("CONFIRMING");
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  (void)h2_mosaico_e2e_screen_result(rc, confirm);
  printf("H2_WEBRTC_PERF_MOSAICO stage=admission rc=%d confirm=%d\n", rc,
         confirm);
  fflush(stdout);
  printf("H2_WEBRTC_PERF_MOSAICO stage=summary status=%s rc=%d "
         "upload_Bps=%llu download_Bps=%llu loaded_Bps=%llu "
         "request_batch_ns=%llu audio_p99_gap_ns=%llu power_save=%d "
         "ble_adv=%d\n",
         rc == 0 && confirm == 0 ? "PASS" : "FAIL", rc,
         (unsigned long long)result.median_upload_bytes_per_second,
         (unsigned long long)result.median_download_bytes_per_second,
         (unsigned long long)result.median_loaded_bytes_per_second,
         (unsigned long long)result.median_request_batch_ns,
         (unsigned long long)result.median_audio_p99_gap_ns,
         (int)H2_WEBRTC_PERF_POWER_SAVE, (int)H2_WEBRTC_PERF_BLE_ADV);
  fflush(stdout);
  hold();
}

void app_main(void) {
  int rc = h2_mosaico_loader_usb_init();
  if (rc == H2_PAL_OK)
    rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) {
    printf("H2_MOSAICO_WORKLOAD_SETUP_FAIL suite=webrtc-performance rc=%d\n",
           rc);
    fflush(stdout);
    return;
  }
  if (xTaskCreatePinnedToCoreWithCaps(image_entry, "mosaico/webrtc-performance",
                                      65536u, NULL, tskIDLE_PRIORITY + 4u, NULL,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
      pdPASS) {
    printf("H2_MOSAICO_WORKLOAD_SETUP_FAIL suite=webrtc-performance stage=task "
           "rc=%d\n",
           H2_PAL_ERR_TASK);
    fflush(stdout);
  }
}
