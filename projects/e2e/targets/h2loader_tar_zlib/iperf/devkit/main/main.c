#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_iperf_e2e.h"
#include "h2_sctp.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/* Operator-selected LAN peer is injected by the Bazel firmware rule. */
#ifndef H2_IPERF_SERVER
#define H2_IPERF_SERVER "192.0.2.1"
#endif

/* 0 keeps the radio awake, 1 sleeps per DTIM, 2 sleeps per listen interval. */
#ifndef H2_IPERF_POWER_SAVE
#define H2_IPERF_POWER_SAVE 0
#endif

/* 1 keeps the H2Loader App command service advertising over BLE, 0 pauses it
 * before the workload so BLE/Wi-Fi coexistence does not time-slice the radio. */
#ifndef H2_IPERF_BLE_ADV
#define H2_IPERF_BLE_ADV 1
#endif

/* 0 runs the client matrix against H2_IPERF_SERVER, 1 serves iperf3 clients
 * (TCP, UDP and SCTP-over-UDP) so the host can drive each direction itself. */
#ifndef H2_IPERF_ROLE_SERVER
#define H2_IPERF_ROLE_SERVER 0
#endif

#define H2_IPERF_DEVKIT_PORT 5201u
#define H2_IPERF_DEVKIT_SCTP_UDP_PORT 9899u
#define H2_IPERF_DEVKIT_CASE_MS 5000u

static void hold(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void memory_checkpoint(const char *name) {
  printf("H2_IPERF_E2E_MEMORY checkpoint=%s internal_free_kib=%u "
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

static void case_checkpoint(void *user, const char *name) {
  (void)user;
  memory_checkpoint(name);
}

static void fail(const char *stage, int rc, int commands_started) {
  printf("H2_IPERF_E2E_DEVKIT stage=%s status=ERROR rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_started) {
    esp_restart();
  }
  hold();
}

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
  return h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings,
                                                   out_config);
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
          printf("H2_IPERF_E2E_DEVKIT stage=wifi status=READY "
                 "ip=%u.%u.%u.%u rssi=%d channel=%u\n",
                 ip[0], ip[1], ip[2], ip[3], status.rssi,
                 (unsigned)status.channel);
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
    printf("H2_IPERF_E2E_DEVKIT stage=wifi status=WAIT rc=%d retry_ms=3000\n",
           rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
}

/* The selected policy must be in effect before any measurement, otherwise
 * the summary would label results with a mode that was never applied. */
static int apply_power_save(h2_runtime_t *runtime) {
  const h2_pal_wifi_power_save_t mode =
      (h2_pal_wifi_power_save_t)H2_IPERF_POWER_SAVE;
  const int rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta, mode);
  printf("H2_IPERF_E2E_DEVKIT stage=power_save mode=%d rc=%d\n", (int)mode,
         rc);
  fflush(stdout);
  return rc;
}

static void serve_forever(const h2_iperf_config_t *pal) {
  h2_iperf_server_params_t params;
  memset(&params, 0, sizeof(params));
  params.port = H2_IPERF_DEVKIT_PORT;
  params.sctp_udp_port = H2_IPERF_DEVKIT_SCTP_UDP_PORT;
  h2_iperf_server_t *server = NULL;
  int rc = h2_iperf_server_create(pal, &params, &server);
  if (rc != H2_PAL_OK) {
    fail("server_create", rc, 1);
  }
  printf("H2_IPERF_E2E_DEVKIT stage=server status=LISTENING port=%u "
         "sctp_udp_port=%u\n",
         (unsigned)h2_iperf_server_port(server),
         (unsigned)h2_iperf_server_sctp_udp_port(server));
  fflush(stdout);
  for (;;) {
    h2_iperf_result_t result;
    memset(&result, 0, sizeof(result));
    memory_checkpoint("server_test_start");
    rc = h2_iperf_server_run_once(server, 60000u, &result);
    if (rc == H2_PAL_ERR_TIMEOUT) {
      continue;
    }
    memory_checkpoint("server_test_end");
    const h2_iperf_stream_stats_t *receiver =
        result.reverse ? &result.remote : &result.local;
    printf("H2_IPERF_E2E_DEVKIT stage=server_test rc=%d protocol=%d "
           "reverse=%d receiver_bps=%llu bytes=%llu packets=%llu lost=%lld "
           "jitter_us=%u duration_ms=%u\n",
           rc, (int)result.protocol, result.reverse ? 1 : 0,
           (unsigned long long)h2_iperf_stats_bits_per_second(receiver),
           (unsigned long long)receiver->bytes,
           (unsigned long long)receiver->packets,
           (long long)receiver->lost_packets,
           (unsigned)(receiver->jitter_ms > 0.0
                          ? receiver->jitter_ms * 1000.0
                          : 0.0),
           (unsigned)receiver->duration_ms);
    fflush(stdout);
  }
}

/* The board creates the entry task at priority 4 with no core affinity. The
 * measurement task drains sockets from the application side, so it must not
 * be starved by the priority-8 H2Loader command service; run it above that
 * service (still below lwIP at 18 and Wi-Fi at 23). */
#define H2_IPERF_ENTRY_TASK_PRIORITY (tskIDLE_PRIORITY + 9u)

static void image_entry(void *user) {
  (void)user;
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  vTaskPrioritySet(NULL, H2_IPERF_ENTRY_TASK_PRIORITY);
  memory_checkpoint("entry");
  int rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    fail("runtime_config", rc, 0);
  }
  /* Data buffers (iperf blocks, h2sctp windows, H2Peer slots and mailboxes)
   * live in PSRAM; internal RAM stays for Wi-Fi, lwIP and task stacks. */
  runtime_config.mem = h2_esp_platform_psram_allocator();
  rc = h2_esp_h2loader_app_commands_prepare_serial(&runtime_config, "iperf",
                                                   1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_prepare", rc, 0);
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    fail("runtime_init", rc, 0);
  }
  rc = h2_esp_h2loader_app_commands_start(runtime, "iperf", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_start", rc, 0);
  }
  if (!H2_IPERF_ROLE_SERVER && strcmp(H2_IPERF_SERVER, "192.0.2.1") == 0) {
    fail("endpoint_config", H2_PAL_ERR_INVALID_ARG, 1);
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) {
    fail("confirm", rc, 1);
  }
  h2_pal_wifi_sta_config_t wifi;
  memset(&wifi, 0, sizeof(wifi));
  rc = load_wifi_config(runtime, &wifi);
  if (rc != H2_PAL_OK) {
    fail("wifi_settings", rc, 1);
  }
  wait_for_wifi(runtime, &wifi);
  rc = apply_power_save(runtime);
  if (rc != H2_PAL_OK) {
    fail("power_save", rc, 1);
  }
  if (!H2_IPERF_BLE_ADV) {
    rc = h2_esp_h2loader_app_commands_pause_ble_advertising();
    if (rc != H2_PAL_OK) {
      fail("ble_adv", rc, 1);
    }
  }
  printf("H2_IPERF_E2E_DEVKIT stage=ble_adv keep=%d rc=%d\n", (int)H2_IPERF_BLE_ADV, rc);
  fflush(stdout);
  memory_checkpoint("wifi_ready");

  h2_sctp_t *sctp = NULL;
  const h2_sctp_config_t sctp_config = {
      .mem = runtime->mem,
      .crypto = runtime->crypto,
  };
  rc = h2_sctp_create(&sctp_config, &sctp);
  if (rc != H2_PAL_OK) {
    fail("sctp_create", rc, 1);
  }
  const h2_iperf_e2e_config_t config = {
      .pal =
          {
              .mem = runtime->mem,
              .net = runtime->net,
              .time = runtime->time,
              .crypto = runtime->crypto,
              .sctp = h2_sctp_api(sctp),
              .log = runtime->log,
          },
      .target = "devkit",
      .server_host = H2_IPERF_SERVER,
      .port = H2_IPERF_DEVKIT_PORT,
      .sctp_udp_port = H2_IPERF_DEVKIT_SCTP_UDP_PORT,
      .duration_ms = H2_IPERF_DEVKIT_CASE_MS,
      .checkpoint = case_checkpoint,
  };
  if (H2_IPERF_ROLE_SERVER) {
    serve_forever(&config.pal);
  }
  h2_iperf_e2e_report_t report = {0};
  memory_checkpoint("matrix_start");
  rc = h2_iperf_e2e_run(&config, &report);
  memory_checkpoint("matrix_end");
  printf("H2_IPERF_E2E_DEVKIT stage=summary status=%s rc=%d passed=%u "
         "total=%u power_save=%d ble_adv=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, report.passed, report.total,
         (int)H2_IPERF_POWER_SAVE, (int)H2_IPERF_BLE_ADV);
  fflush(stdout);
  hold();
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  const h2_pal_result_t rc =
      h2_esp_board_start_entry_task("devkit/iperf", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    fail("entry_task", rc, 0);
  }
}
