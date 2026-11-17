#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_net_rtp_trace.h"
#define H2_BK_PERF_OBSERVER_IMPLEMENTATION 1
#include "h2_bk_webrtc_perf_hooks.h"
#include "h2_bk_target_task_policy.h"
#include "h2_device_service.h"
#include "h2_webrtc_perf_config.h"
#include "h2_webrtc_performance.h"

#include "application/h2_pal_http.h"
#include "hal/h2_pal_wifi.h"
#include "hal/h2_pal_wifi_settings.h"

#include <common/sys_config.h>
#include <driver/uart.h>
#include <os/os.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static h2_device_app_commands_t management = {
    .config =
        {
            .active_name = "app",
            .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
            .h2loader_partition_id = 1u,
            .app_partition_id = 2u,
            .coredump_partition_id = 0x7a0000u,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
        },
};

static h2_runtime_t* runtime;
static const h2_pal_uart_io_stream_api_t* console;
static int console_active;
static int console_error;
static h2_pal_firmware_info_t firmware_info;
static char execution[33] = "unavailable";
static h2_webrtc_performance_result_t result;

enum { API_TRACE_CAPACITY = 1024, NET_TRACE_CAPACITY = 2048 };
enum {
  API_TRACE_SEND = 1,
  API_TRACE_RECEIVE = 2,
  API_TRACE_SLOW_POLL = 3,
  API_TRACE_LOSS = 4
};
typedef struct api_trace_record {
  uint64_t started_us, completed_us, source_ns;
  int result;
  uint16_t sequence;
  uint8_t kind;
} api_trace_record_t;
static api_trace_record_t* api_trace;
static h2_bk_net_rtp_trace_event_t* net_trace;
static size_t api_trace_count, api_trace_overflow;
static unsigned trace_clock_errors;
static struct {
  h2_runtime_t* runtime;
  char offer_url[192];
} signaling;

static _Noreturn void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}

static int console_result(int rc) {
  if (rc != H2_PAL_OK && console_error == H2_PAL_OK) console_error = rc;
  return rc;
}

/* The scenario and its callbacks print on the entry/poll thread. One native
 * UART write retains the complete record, including the scenario's own LF.
 * No SDK logger is called on write failure: it cannot recurse into stdio. */
static int write_record(const char* data, size_t size) {
  if (!console_active) return console_result(H2_PAL_ERR_INVALID_STATE);
  size_t written = 0u;
  int rc = h2_pal_uart_io_stream_write(console, data, size, &written, 5000u);
  if (rc == H2_PAL_OK && written != size) rc = H2_PAL_ERR_IO;
  return console_result(rc);
}

static int format_record(const char* format, va_list args) {
  char line[4096];
  int size = vsnprintf(line, sizeof(line), format, args);
  if (size < 0 || (size_t)size >= sizeof(line)) {
    (void)console_result(H2_PAL_ERR_TRUNCATED);
    return -1;
  }
  if (size == 0) return 0;
  if (write_record(line, (size_t)size) != H2_PAL_OK) return -1;
  return size;
}

/* Only h2_webrtc_performance.c is compiled with these printf/puts aliases. */
int h2_zero_bk_webrtc_perf_printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  int size = format_record(format, args);
  va_end(args);
  return size;
}

int h2_zero_bk_webrtc_perf_puts(const char* text) {
  char line[4096];
  size_t size = strlen(text);
  if (size >= sizeof(line) - 1u) {
    (void)console_result(H2_PAL_ERR_TRUNCATED);
    return -1;
  }
  memcpy(line, text, size);
  line[size++] = '\n';
  return write_record(line, size) == H2_PAL_OK ? 0 : -1;
}

static void emit(const char* format, ...)
    __attribute__((format(__printf__, 1, 2)));
static void emit(const char* format, ...) {
  va_list args;
  va_start(args, format);
  if (console_active) {
    (void)format_record(format, args);
  } else {
    /* Setup/restore failures can leave only the SDK console available. This
     * translation unit uses ordinary SDK stdio, never the scenario aliases. */
    char line[4096];
    int size = vsnprintf(line, sizeof(line), format, args);
    if (size >= 0 && (size_t)size < sizeof(line)) {
      printf("%s", line);
      fflush(stdout);
    } else {
      (void)console_result(H2_PAL_ERR_TRUNCATED);
    }
  }
  va_end(args);
}

static uint64_t trace_now_us(void) {
  uint64_t value = 0u;
  if (h2_pal_time_get_monotonic_us(runtime->time, &value) != H2_PAL_OK)
    ++trace_clock_errors;
  return value;
}

static void trace_api_record(uint8_t kind, const uint8_t* payload, size_t size,
                             uint64_t started_us, uint64_t completed_us,
                             int rc) {
  if (api_trace == NULL) return;
  if (api_trace_count == API_TRACE_CAPACITY) {
    ++api_trace_overflow;
    return;
  }
  api_trace_record_t value = {.kind = kind,
                              .sequence = UINT16_MAX,
                              .started_us = started_us,
                              .completed_us = completed_us,
                              .result = rc};
  if (payload != NULL && size == 10u) {
    value.sequence = (uint16_t)((uint16_t)payload[0] << 8u | payload[1]);
    for (size_t i = 2u; i < 10u; ++i)
      value.source_ns = (value.source_ns << 8u) | payload[i];
  }
  api_trace[api_trace_count++] = value;
}

h2_pal_result_t h2_bk_perf_observe_send_opus(const h2_pal_webrtc_api_t* api,
                                             h2_pal_webrtc_peer_t* peer,
                                             const uint8_t* opus,
                                             size_t opus_len) {
  if (api_trace == NULL)
    return h2_pal_webrtc_peer_send_opus(api, peer, opus, opus_len);
  uint64_t started_us = trace_now_us();
  h2_pal_result_t rc = h2_pal_webrtc_peer_send_opus(api, peer, opus, opus_len);
  uint64_t completed_us = trace_now_us();
  if (rc == H2_PAL_OK || completed_us - started_us >= 20000u)
    trace_api_record(API_TRACE_SEND, opus, opus_len, started_us, completed_us,
                     rc);
  return rc;
}

h2_pal_result_t h2_bk_perf_observe_poll(const h2_pal_webrtc_api_t* api,
                                        h2_pal_webrtc_peer_t* peer,
                                        int timeout_ms,
                                        h2_pal_webrtc_event_t* out_event) {
  if (api_trace == NULL)
    return h2_pal_webrtc_peer_poll(api, peer, timeout_ms, out_event);
  uint64_t started_us = trace_now_us();
  h2_pal_result_t rc =
      h2_pal_webrtc_peer_poll(api, peer, timeout_ms, out_event);
  uint64_t completed_us = trace_now_us();
  if (rc == H2_PAL_OK && out_event != NULL &&
      out_event->kind == H2_PAL_WEBRTC_EVENT_OPUS_FRAME) {
    trace_api_record(
        out_event->data_len == 0u ? API_TRACE_LOSS : API_TRACE_RECEIVE,
        out_event->data, out_event->data_len, started_us, completed_us, rc);
  } else if (completed_us - started_us >= 20000u) {
    trace_api_record(API_TRACE_SLOW_POLL, NULL, 0u, started_us, completed_us,
                     rc);
  }
  return rc;
}

static int trace_start(void) {
  api_trace_count = 0u;
  api_trace_overflow = 0u;
  trace_clock_errors = 0u;
  api_trace =
      h2_pal_mem_alloc(runtime->mem, API_TRACE_CAPACITY * sizeof(*api_trace));
  net_trace =
      h2_pal_mem_alloc(runtime->mem, NET_TRACE_CAPACITY * sizeof(*net_trace));
  if (api_trace == NULL || net_trace == NULL) {
    h2_pal_mem_free(runtime->mem, api_trace);
    h2_pal_mem_free(runtime->mem, net_trace);
    api_trace = NULL;
    net_trace = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  int rc = h2_bk_net_rtp_trace_start(net_trace, NET_TRACE_CAPACITY);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(runtime->mem, api_trace);
    h2_pal_mem_free(runtime->mem, net_trace);
    api_trace = NULL;
    net_trace = NULL;
  }
  return rc;
}

/* Dump after the workload, never while measuring cadence or throughput. */
static int trace_dump(void) {
  h2_bk_net_rtp_trace_stats_t stats;
  int rc = h2_bk_net_rtp_trace_stop(&stats);
  if (rc != H2_PAL_OK) return rc;
  emit(
      "H2_WEBRTC_PERF_TIMING execution=%s api_count=%zu api_overflow=%zu "
      "net_count=%zu net_overflow=%zu clock_errors=%u udp_send_calls=%llu "
      "udp_recv_calls=%llu max_udp_send_us=%llu max_udp_recv_us=%llu "
      "max_send_started_us=%llu max_recv_started_us=%llu\r\n",
      execution, api_trace_count, api_trace_overflow, stats.count,
      stats.overflow, trace_clock_errors, (unsigned long long)stats.send_calls,
      (unsigned long long)stats.receive_calls,
      (unsigned long long)stats.max_send_us,
      (unsigned long long)stats.max_receive_us,
      (unsigned long long)stats.max_send_started_us,
      (unsigned long long)stats.max_receive_started_us);
  for (size_t i = 0u; i < api_trace_count; ++i) {
    const api_trace_record_t* value = &api_trace[i];
    emit(
        "H2_WEBRTC_PERF_API_IO execution=%s kind=%u seq=%u source_ns=%llu "
        "begin_us=%llu end_us=%llu rc=%d\r\n",
        execution, (unsigned)value->kind, (unsigned)value->sequence,
        (unsigned long long)value->source_ns,
        (unsigned long long)value->started_us,
        (unsigned long long)value->completed_us, value->result);
  }
  for (size_t i = 0u; i < stats.count; ++i) {
    const h2_bk_net_rtp_trace_event_t* value = &net_trace[i];
    emit(
        "H2_WEBRTC_PERF_NET_IO execution=%s dir=%u seq=%u rtp_ts=%u "
        "ssrc=%u bytes=%u begin_us=%llu end_us=%llu rc=%d errno=%d\r\n",
        execution, (unsigned)value->direction, (unsigned)value->sequence,
        (unsigned)value->rtp_timestamp, (unsigned)value->ssrc,
        (unsigned)value->packet_bytes, (unsigned long long)value->started_us,
        (unsigned long long)value->completed_us, value->result,
        value->socket_errno);
  }
  h2_pal_mem_free(runtime->mem, api_trace);
  h2_pal_mem_free(runtime->mem, net_trace);
  api_trace = NULL;
  net_trace = NULL;
  return api_trace_overflow != 0u || stats.overflow != 0u ||
                 trace_clock_errors != 0u
             ? H2_PAL_ERR_TRUNCATED
             : H2_PAL_OK;
}

static void memory_checkpoint(void* user, const char* name) {
  (void)user;
  h2_bk_platform_resource_stats_t stats = {0};
  int rc = h2_bk_platform_get_resource_stats(&stats);
  emit(
      "H2_WEBRTC_PERF_MEMORY target=zero_bk_2_0 execution=%s checkpoint=%s "
      "internal_free=%u internal_min=%u psram_free=%u resource_rc=%d "
      "allocations=%u bytes=%u live_tasks=%u task_stack_bytes=%u\r\n",
      execution, name, (unsigned)rtos_get_free_heap_size(),
      (unsigned)rtos_get_minimum_free_heap_size(),
      (unsigned)rtos_get_psram_free_heap_size(), rc,
      (unsigned)stats.allocations, (unsigned)stats.allocation_bytes,
      (unsigned)stats.live_tasks, (unsigned)stats.task_stack_bytes);
}

static int claim_console(void) {
  console = h2_bk_platform_uart_io_stream_api();
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_UART_PRINT_BAUD_RATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 8192u,
      .tx_buffer_size = 2048u,
  };
  int rc = h2_pal_uart_io_stream_configure(console, &config);
  if (rc == H2_PAL_OK) console_active = 1;
  return rc;
}

static int release_console(void) {
  if (console_active) {
    uint32_t started = rtos_get_time();
    while (!bk_uart_is_tx_over(CONFIG_UART_PRINT_PORT)) {
      if ((uint32_t)(rtos_get_time() - started) >= 5000u) {
        (void)console_result(H2_PAL_ERR_TIMEOUT);
        break;
      }
      rtos_delay_milliseconds(1u);
    }
  }
  console_active = 0;
  return console == NULL ? H2_PAL_OK : h2_pal_uart_io_stream_release(console);
}

static _Noreturn void finish(const char* stage, int run_rc) {
  int rc = run_rc;
  if (rc == H2_PAL_OK && console_error != H2_PAL_OK) rc = console_error;
  /* Commit the complete workload evidence before considering confirmation.
   * The final RC below also covers transport restoration and confirmation. */
  emit(
      "H2_WEBRTC_PERF_SUMMARY target=zero_bk_2_0 provider=h2peer "
      "execution=%s profile=smoke stage=%s status=%s run_rc=%d "
      "console_rc=%d iterations=%u upload_Bps=%llu download_Bps=%llu "
      "loaded_Bps=%llu request_batch_ns=%llu audio_p99_gap_ns=%llu "
      "audio_max_gap_ns=%llu power_save=0 ble_adv=0\r\n",
      execution, stage, rc == H2_PAL_OK ? "PASS" : "FAIL", run_rc,
      console_error, result.iterations,
      (unsigned long long)result.median_upload_bytes_per_second,
      (unsigned long long)result.median_download_bytes_per_second,
      (unsigned long long)result.median_loaded_bytes_per_second,
      (unsigned long long)result.median_request_batch_ns,
      (unsigned long long)result.median_audio_p99_gap_ns,
      (unsigned long long)result.median_audio_max_gap_ns);
  if (rc == H2_PAL_OK && console_error != H2_PAL_OK) rc = console_error;
  int release_rc = release_console();
  if (rc == H2_PAL_OK && release_rc != H2_PAL_OK) rc = release_rc;
  int restore_rc = H2_PAL_ERR_INVALID_STATE;
  int confirm_rc = H2_PAL_ERR_INVALID_STATE;
  if (runtime != NULL && release_rc == H2_PAL_OK) {
    restore_rc = h2_device_app_commands_start_serial_named(
        &management, runtime, "webrtc-performance",
        H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
    if (rc == H2_PAL_OK && restore_rc != H2_PAL_OK) rc = restore_rc;
    /* Serial startup synchronously configures this same static UART API.
     * The observer sends no management traffic until these final records. */
    if (restore_rc == H2_PAL_OK) {
      console_active = 1;
      emit(
          "H2_WEBRTC_PERF_ZERO_BK stage=control_restored execution=%s "
          "run_rc=%d console_rc=%d\r\n",
          execution, run_rc, console_error);
    }
    if (rc == H2_PAL_OK && console_error != H2_PAL_OK) rc = console_error;
    if (rc == H2_PAL_OK) {
      confirm_rc = h2_device_app_confirm(&management, runtime);
      if (confirm_rc != H2_PAL_OK) rc = confirm_rc;
    }
  }
  if (rc == H2_PAL_OK && console_error != H2_PAL_OK) rc = console_error;
  emit(
      "H2_WEBRTC_PERF_RC target=zero_bk_2_0 execution=%s rc=%d "
      "console_rc=%d release_rc=%d restore_rc=%d confirm_rc=%d "
      "control_restored=%d\r\n",
      execution, rc, console_error, release_rc, restore_rc, confirm_rc,
      restore_rc == H2_PAL_OK);
  console_active = 0;
  /* Manager, Runtime, provider and all borrowed state have boot lifetime,
   * including when confirmation fails. */
  hold();
}

static int network_ready(void) {
  h2_pal_wifi_sta_status_t status = {0};
  int rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
  if (rc != H2_PAL_OK) return rc;
  if (status.state != H2_PAL_WIFI_STA_STATE_GOT_IP || !status.ip_valid ||
      status.ip.ip4 == 0u) {
    h2_pal_wifi_sta_config_t saved = {0};
    rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings,
                                                   &saved);
    if (rc == H2_PAL_OK)
      rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, 20000u);
    memset(&saved, 0, sizeof(saved));
    if (rc != H2_PAL_OK) return rc;
    uint32_t started = rtos_get_time();
    for (;;) {
      rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
      if (rc != H2_PAL_OK) return rc;
      if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
          status.ip.ip4 != 0u)
        break;
      if (status.state == H2_PAL_WIFI_STA_STATE_FAILED ||
          status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED)
        return H2_PAL_ERR_IO;
      if ((uint32_t)(rtos_get_time() - started) >= 20000u)
        return H2_PAL_ERR_TIMEOUT;
      rtos_delay_milliseconds(100u);
    }
  }
  uint8_t ip[4];
  h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
  emit(
      "H2_WEBRTC_PERF_ZERO_BK stage=wifi status=READY "
      "ip=%u.%u.%u.%u\r\n",
      ip[0], ip[1], ip[2], ip[3]);
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  emit("H2_WEBRTC_PERF_ZERO_BK stage=power_save mode=0 rc=%d\r\n", rc);
  return rc;
}

static int exchange_offer(void* user, h2_pal_webrtc_str_t offer, char* answer,
                          size_t answer_capacity, size_t* answer_len) {
  (void)user;
  static const h2_pal_http_header_t headers[] = {
      {{"Content-Type", sizeof("Content-Type") - 1u},
       {"application/sdp", sizeof("application/sdp") - 1u}},
      {{"X-H2-Reverse-Channels", sizeof("X-H2-Reverse-Channels") - 1u},
       {"0", 1u}},
  };
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {signaling.offer_url, strlen(signaling.offer_url)},
      .headers = headers,
      .header_count = sizeof(headers) / sizeof(headers[0]),
      .body = (const uint8_t*)offer.data,
      .body_len = offer.len,
      .timeout_ms = 20000,
      .connect_timeout_ms = 20000,
      .retry_count = 0,
      .response_buf = (uint8_t*)answer,
      .response_buf_cap = answer_capacity,
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  int rc = h2_pal_http_request(signaling.runtime->http, &request, &response);
  if (rc != H2_PAL_OK || response.status_code != 200 || response.body == NULL ||
      response.body_len == 0u || response.body_len >= answer_capacity) {
    emit(
        "H2_WEBRTC_PERF stage=signaling status=ERROR rc=%d http=%d "
        "len=%zu\r\n",
        rc, response.status_code, response.body_len);
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
  }
  if (response.body != (uint8_t*)answer)
    memcpy(answer, response.body, response.body_len);
  answer[response.body_len] = '\0';
  *answer_len = response.body_len;
  return H2_PAL_OK;
}

static void entry(void* user) {
  (void)user;
  int console_rc = claim_console();
  /* The host reconnects immediately after Loader accepts reboot/upgrade.
   * This window precedes all network/benchmark deadlines and measurements. */
  if (console_rc == H2_PAL_OK) rtos_delay_milliseconds(5000u);
  uint8_t nonce[16];
  int nonce_rc =
      h2_pal_crypto_random(h2_bk_platform_crypto_api(), nonce, sizeof(nonce));
  if (nonce_rc == H2_PAL_OK) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(nonce); ++i) {
      execution[i * 2u] = hex[nonce[i] >> 4u];
      execution[i * 2u + 1u] = hex[nonce[i] & 15u];
    }
    execution[32] = '\0';
  }
  int version_rc = h2_pal_firmware_info_get_current(
      h2_bk_platform_firmware_info_api(), &firmware_info);
  emit(
      "H2_WEBRTC_PERF_BOOT target=zero_bk_2_0 provider=h2peer "
      "execution=%s version=%s profile=smoke power_save=0 ble_adv=0 "
      "console_rc=%d\r\n",
      execution, version_rc == H2_PAL_OK ? firmware_info.version : "unknown",
      console_rc);
  memory_checkpoint(NULL, "entry");

  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) finish("runtime_config", rc);
  config.mem = h2_bk_platform_psram_allocator();
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) finish("runtime_init", rc);
  memory_checkpoint(NULL, "runtime_ready");
  if (console_rc != H2_PAL_OK) finish("console", console_rc);
  if (nonce_rc != H2_PAL_OK) finish("execution", nonce_rc);
  if (version_rc != H2_PAL_OK) finish("version", version_rc);
  if (console_error != H2_PAL_OK) finish("console", console_error);
  if (strcmp(H2_WEBRTC_PERF_ENDPOINT, "http://192.0.2.1:18080") == 0 ||
      strcmp(H2_WEBRTC_PERF_STUN_URL, "stun:192.0.2.1:3478") == 0 ||
      strncmp(H2_WEBRTC_PERF_ENDPOINT, "http://", 7u) != 0 ||
      strncmp(H2_WEBRTC_PERF_STUN_URL, "stun:", 5u) != 0)
    finish("endpoint_config", H2_PAL_ERR_INVALID_ARG);
  signaling.runtime = runtime;
  int url_size = snprintf(signaling.offer_url, sizeof(signaling.offer_url),
                          "%s/offer", H2_WEBRTC_PERF_ENDPOINT);
  if (url_size <= 0 || (size_t)url_size >= sizeof(signaling.offer_url))
    finish("endpoint", H2_PAL_ERR_INVALID_ARG);
  rc = network_ready();
  if (rc != H2_PAL_OK) finish("network", rc);
  memory_checkpoint(NULL, "wifi_ready");
  if (console_error != H2_PAL_OK) finish("console", console_error);
  const h2_webrtc_performance_config_t benchmark = {
      .profile = "smoke",
      .target = "zero_bk_2_0",
      .provider = "h2peer",
      .stun_url = {H2_WEBRTC_PERF_STUN_URL,
                   sizeof(H2_WEBRTC_PERF_STUN_URL) - 1u},
      .exchange_offer = exchange_offer,
      .checkpoint = memory_checkpoint,
  };
  rc = trace_start();
  if (rc != H2_PAL_OK) finish("trace_setup", rc);
  memory_checkpoint(NULL, "benchmark_start");
  rc = h2_webrtc_performance_run(runtime, &benchmark, &result);
  memory_checkpoint(NULL, "benchmark_end");
  int trace_rc = trace_dump();
  if (rc == H2_PAL_OK && trace_rc != H2_PAL_OK) rc = trace_rc;
  memory_checkpoint(NULL, "trace_released");
  finish("benchmark", rc);
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/webrtc-performance", entry, NULL);
  if (rc != H2_PAL_OK) finish("entry", rc);
  return 0;
}
