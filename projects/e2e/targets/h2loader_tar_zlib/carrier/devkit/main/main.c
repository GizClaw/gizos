#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_carrier_device_inputs.h"
#include "h2_carrier_setup_failure.h"
#include "h2_lingxi_e2e.h"
#include "h2_onenet_e2e.h"
#include "h2_coremqtt.h"
#include "h2_yyjson_json.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define INPUT_PATH "/data/carrier-e2e.json"

static h2_runtime_t* runtime;
static h2_yyjson_json_t* json_provider;
static h2_carrier_device_inputs_t inputs;
static h2_lingxi_e2e_result_t lingxi_result;
static h2_onenet_e2e_result_t onenet_result;
static char run_id[17];
static const h2_pal_net_api_t* mqtt_delegate;
static uint8_t connack[4];
static size_t connack_bytes;
static int connack_code = -1;
static int mqtt_tls_result = H2_PAL_ERR_INVALID_STATE;
static int mqtt_dns_result = H2_PAL_ERR_INVALID_STATE;
static unsigned mqtt_dns_addresses;
static int mqtt_tcp_result = H2_PAL_ERR_INVALID_STATE;
static unsigned mqtt_tcp_attempts;

static h2_pal_result_t observed_resolve(void* user,
                                        h2_pal_net_resolver_t* resolver,
                                        h2_pal_net_addr_list_t* addresses,
                                        uint32_t timeout_ms) {
  if (mqtt_delegate->vtable->resolve_poll_all == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  mqtt_dns_result = mqtt_delegate->vtable->resolve_poll_all(
      user, resolver, addresses, timeout_ms);
  if (mqtt_dns_result == H2_PAL_OK)
    mqtt_dns_addresses = (unsigned)addresses->count;
  return mqtt_dns_result;
}

static h2_pal_result_t observed_connect(void* user, h2_pal_net_socket_t socket,
                                        const h2_pal_net_addr_t* address,
                                        uint32_t timeout_ms) {
  if (mqtt_delegate->vtable->tcp_connect == NULL) return H2_PAL_ERR_UNSUPPORTED;
  ++mqtt_tcp_attempts;
  mqtt_tcp_result =
      mqtt_delegate->vtable->tcp_connect(user, socket, address, timeout_ms);
  return mqtt_tcp_result;
}

/* Observe only the first server MQTT response, never outgoing credentials.
 * Other PAL dispatch and all transport return values remain unchanged. */
static int observed_recv(void* user, h2_pal_net_socket_t socket, uint8_t* bytes,
                         size_t capacity, uint32_t timeout_ms) {
  if (mqtt_delegate->vtable->tcp_recv == NULL) return H2_PAL_ERR_UNSUPPORTED;
  const int rc = mqtt_delegate->vtable->tcp_recv(user, socket, bytes, capacity,
                                                 timeout_ms);
  if (rc > 0 && (size_t)rc <= capacity) {
    for (size_t i = 0u; i < (size_t)rc && connack_bytes < sizeof(connack); ++i)
      connack[connack_bytes++] = bytes[i];
    if (connack_bytes == sizeof(connack) && connack[0] == 0x20u &&
        connack[1] == 2u)
      connack_code = connack[3];
  }
  return rc;
}

static h2_pal_result_t observed_tls(void* user, h2_pal_net_socket_t tcp,
                                    const h2_pal_net_tls_config_t* config,
                                    uint32_t timeout_ms,
                                    h2_pal_net_socket_t* out) {
  if (mqtt_delegate->vtable->tls_wrap == NULL) return H2_PAL_ERR_UNSUPPORTED;
  mqtt_tls_result =
      mqtt_delegate->vtable->tls_wrap(user, tcp, config, timeout_ms, out);
  return mqtt_tls_result;
}

static void wipe(void* p, size_t n) {
  volatile unsigned char* b = p;
  while (n-- != 0u) *b++ = 0u;
}

static void fail(const char* stage, int rc) {
  h2_carrier_setup_fail(&inputs, stage, rc);
}

static int load_inputs(void) {
  h2_pal_fs_stat_t stat = {0};
  int rc = h2_pal_fs_stat(runtime->fs, INPUT_PATH, &stat);
  if (rc != H2_PAL_OK) return rc;
  if (stat.is_dir || stat.size == 0u ||
      stat.size > H2_CARRIER_DEVICE_CONFIG_BYTES)
    return H2_PAL_ERR_FORMAT;
  const size_t length = (size_t)stat.size;
  uint8_t* bytes = h2_pal_mem_alloc(runtime->mem, length);
  if (bytes == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_pal_fs_file_t* file = NULL;
  rc = h2_pal_fs_open(runtime->fs, INPUT_PATH, H2_PAL_FS_OPEN_READ, &file);
  size_t total = 0u;
  while (rc == H2_PAL_OK && total < length) {
    size_t read = 0u;
    rc =
        h2_pal_fs_read(runtime->fs, file, bytes + total, length - total, &read);
    if (rc == H2_PAL_OK && (read == 0u || read > length - total))
      rc = H2_PAL_ERR_IO;
    total += read;
  }
  if (file != NULL) {
    const int closed = h2_pal_fs_close(runtime->fs, file);
    if (rc == H2_PAL_OK) rc = closed;
  }
  if (rc == H2_PAL_OK)
    rc = h2_carrier_device_inputs_parse(h2_yyjson_json_api(json_provider),
                                        bytes, length, &inputs);
  wipe(bytes, length);
  h2_pal_mem_free(runtime->mem, bytes);
  if (rc == H2_PAL_OK) rc = h2_pal_fs_remove(runtime->fs, INPUT_PATH);
  return rc;
}

static int network(void) {
  h2_pal_wifi_sta_config_t saved = {0};
  int rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &saved);
  if (rc == H2_PAL_OK)
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, 20000u);
  wipe(&saved, sizeof(saved));
  if (rc != H2_PAL_OK) return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  while (rc == H2_PAL_OK) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc != H2_PAL_OK) return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid)
      return H2_PAL_OK;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    if (now < start || now - start >= 20000u) return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
  }
  return rc;
}

static void report_lingxi(const h2_lingxi_e2e_result_t* result) {
  for (unsigned i = 0u; i < H2_LINGXI_E2E_CASE_COUNT; ++i) {
    const h2_lingxi_e2e_case_result_t* r = &result->cases[i];
    const char* status = r->status == H2_LINGXI_E2E_PASS   ? "PASS"
                         : r->status == H2_LINGXI_E2E_FAIL ? "FAIL"
                                                           : "BLOCKED";
    printf(
        "H2_LINGXI_E2E_CASE {\"id\":\"%s\",\"status\":\"%s\",\"result\":%d,\"http\":%d,\"code\":%" PRId32
        ",\"success\":%s,\"elapsed_ms\":%llu}\n",
        r->id, status, r->result, r->platform.http_status,
        r->platform.platform_code,
        r->platform.platform_success ? "true" : "false",
        (unsigned long long)r->elapsed_ms);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(30u));
  }
  printf("H2_LINGXI_E2E_SUMMARY passed=%u failed=%u blocked=%u\n",
         result->passed, result->failed, result->blocked);
}

static void run(void* user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc = load_inputs();
  if (rc != H2_PAL_OK) fail("runtime-config", rc);
  rc = h2_pal_time_set_wall_ms(runtime->time, inputs.epoch_ms);
  if (rc != H2_PAL_OK) fail("wall-clock", rc);
  rc = network();
  if (rc != H2_PAL_OK) fail("network", rc);
  uint8_t nonce[8];
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK) fail("run-id", rc);
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0u; i < sizeof(nonce); ++i) {
    run_id[2u * i] = digits[nonce[i] >> 4];
    run_id[2u * i + 1u] = digits[nonce[i] & 15u];
  }
  char ip_text[16];
  h2_pal_wifi_sta_status_t wifi_status = {0};
  rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi_status);
  if (rc != H2_PAL_OK || !wifi_status.ip_valid)
    fail("wifi-address", rc != H2_PAL_OK ? rc : H2_PAL_ERR_UNAVAILABLE);
  uint8_t octets[4];
  h2_pal_wifi_ip4_to_bytes(wifi_status.ip.ip4, octets);
  (void)snprintf(ip_text, sizeof(ip_text), "%u.%u.%u.%u", octets[0], octets[1],
                 octets[2], octets[3]);
  const char* ips[] = {ip_text};
  uint8_t mac[6];
  rc = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (rc != ESP_OK) fail("mac", rc);
  char mac_text[18];
  (void)snprintf(mac_text, sizeof(mac_text), "%02X-%02X-%02X-%02X-%02X-%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  const bool selected_lingxi = inputs.has_lingxi;
  const bool selected_onenet = inputs.has_onenet;
  printf("H2_CARRIER_SELECTION lingxi=%u onenet=%u\n",
         (unsigned)selected_lingxi, (unsigned)selected_onenet);
  int lingxi_rc = H2_PAL_OK;
  if (selected_lingxi) {
    const h2_lingxi_e2e_config_t lingxi = {
        .client = {.allocator = runtime->mem,
                   .http = runtime->http,
                   .json = h2_yyjson_json_api(json_provider),
                   .base_url = inputs.lingxi_base,
                   .json_limits = {16384u, 24u, 512u},
                   .timeout_ms = 15000u},
        .time = runtime->time,
        .product = {inputs.lingxi_product, inputs.lingxi_key},
        .identity = {inputs.device_no_type, inputs.device_no},
        .heartbeat = {.inner_ips = ips,
                      .inner_ip_count = 1u,
                      .net_speed = "0Mbps",
                      .net_type = "wifi",
                      .platform = "FreeRTOS",
                      .sdk_version = "ai_http_1.0",
                      .firmware_version = esp_app_get_description()->version,
                      .imei = inputs.device_no_type == H2_LINGXI_DEVICE_NO_IMEI
                                  ? inputs.device_no
                                  : "",
                      .cmei = inputs.device_no_type == H2_LINGXI_DEVICE_NO_CMEI
                                  ? inputs.device_no
                                  : "",
                      .mac = mac_text},
    };
    lingxi_rc = h2_lingxi_e2e_run(&lingxi, &lingxi_result);
  }
  int onenet_rc = H2_PAL_OK;
  if (selected_onenet) {
    h2_coremqtt_t* mqtt = NULL;
    h2_pal_mqtt_api_t mqtt_api = {0};
    mqtt_delegate = runtime->net;
    h2_pal_net_vtable_t observed_vtable = *mqtt_delegate->vtable;
    observed_vtable.tcp_recv = observed_recv;
    observed_vtable.resolve_poll_all = observed_resolve;
    observed_vtable.tcp_connect = observed_connect;
    observed_vtable.tls_wrap = observed_tls;
    const h2_pal_net_api_t observed_net = {mqtt_delegate->user,
                                           &observed_vtable};
    const h2_coremqtt_config_t mqtt_provider = {.allocator = runtime->mem,
                                                .net = &observed_net,
                                                .time = runtime->time,
                                                .log = runtime->log,
                                                .outgoing_publish_records = 4u,
                                                .incoming_publish_records = 4u};
    onenet_rc = h2_coremqtt_create(&mqtt_provider, &mqtt, &mqtt_api);
    if (onenet_rc == H2_PAL_OK && inputs.onenet_token[0] == '\0')
      onenet_rc = h2_onenet_token_generate(
          runtime->crypto, inputs.onenet_product, inputs.onenet_device,
          inputs.onenet_device_key, inputs.epoch_ms / 1000u + 3600u,
          inputs.onenet_token, sizeof(inputs.onenet_token));
    printf("H2_ONENET_AUTH_PREPARE result=%d\n", onenet_rc);
    const h2_pal_net_tls_config_t tls = {
        .server_name = inputs.onenet_tls_server_name[0] != '\0'
                           ? inputs.onenet_tls_server_name
                           : inputs.onenet_host,
        .root_ca_pem = inputs.onenet_ca_pem[0] != '\0'
                           ? (const uint8_t*)inputs.onenet_ca_pem
                           : NULL,
        .root_ca_pem_len = strlen(inputs.onenet_ca_pem),
        .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
    const h2_pal_net_bind_t wifi_bind = {
        .type = H2_PAL_NET_BIND_SOURCE_ADDR,
        .source_addr = {.family = H2_PAL_NET_FAMILY_IPV4,
                        .ip = {octets[0], octets[1], octets[2], octets[3]}},
    };
    if (onenet_rc == H2_PAL_OK) {
      const h2_onenet_e2e_config_t onenet = {
          .client = {.allocator = runtime->mem,
                     .mqtt = &mqtt_api,
                     .json = h2_yyjson_json_api(json_provider),
                     .time = runtime->time,
                     .product_id = inputs.onenet_product,
                     .device_name = inputs.onenet_device,
                     .endpoint = {{inputs.onenet_host,
                                   strlen(inputs.onenet_host)},
                                  inputs.onenet_port},
                     .bind = &wifi_bind,
                     .transport = H2_PAL_MQTT_TRANSPORT_TLS,
                     .tls = &tls,
                     .limits = {8192u, 24u, 512u, 4u, 4u},
                     .mqtt_buffer_bytes = 16384u,
                     .keepalive_sec = 60u,
                     .operation_timeout_ms = 15000u,
                     .response_timeout_ms = 15000u,
                     .initial_request_id =
                         inputs.epoch_ms % H2_ONENET_MAX_REQUEST_ID + 1u},
          .access_token = inputs.onenet_token,
          .params_json = inputs.onenet_params,
          .timeout_ms = 15000u,
      };
      onenet_rc = h2_onenet_e2e_run(&onenet, &onenet_result);
    } else {
      onenet_result.connect_result = onenet_rc;
      onenet_result.post_result = H2_PAL_ERR_INVALID_STATE;
    }
    h2_coremqtt_destroy(mqtt);
  }
  h2_carrier_device_inputs_clear(&inputs);
  const int overall = lingxi_rc != H2_PAL_OK ? lingxi_rc : onenet_rc;
  int confirm = H2_PAL_ERR_INVALID_STATE;
  for (unsigned replay = 0u;; ++replay) {
    printf("H2_CARRIER_RUN id=%s version=%s replay=%u\n", run_id,
           esp_app_get_description()->version, replay);
    if (selected_lingxi) report_lingxi(&lingxi_result);
    if (selected_onenet)
      printf(
          "H2_ONENET_E2E_SUMMARY connect=%d post=%d cleanup=%d code=%" PRId32
          " responses=%u commands=%u tls=%d connack=%d dns=%d addresses=%u tcp=%d tcp_attempts=%u\n",
          onenet_result.connect_result, onenet_result.post_result,
          onenet_result.cleanup_result, onenet_result.platform_code,
          onenet_result.responses, onenet_result.commands, mqtt_tls_result,
          connack_code, mqtt_dns_result, mqtt_dns_addresses, mqtt_tcp_result,
          mqtt_tcp_attempts);
    printf("H2_CARRIER_READY rc=%d lingxi=%d onenet=%d confirm=%d\n", overall,
           lingxi_rc, onenet_rc, confirm);
    fflush(stdout);
    if (replay == 0u && overall == H2_PAL_OK)
      confirm = h2_esp_h2loader_app_confirm(runtime);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) fail("task-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail("board", rc);
  rc = h2_yyjson_json_create(config.mem, &json_provider);
  if (rc != H2_PAL_OK) fail("json", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "carrier-e2e", 1u,
                                                   3u);
  if (rc != H2_PAL_OK) fail("command-service", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime", rc);
  printf("H2_CARRIER_BOOT board=devkit version=%s\n",
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {.name = "carrier/e2e/runner"};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK) fail("runner", rc);
}
