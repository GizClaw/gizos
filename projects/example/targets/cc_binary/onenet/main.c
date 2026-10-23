#include "h2_onenet.h"
#include "h2_coremqtt.h"
#include "h2_desktop_platform.h"
#include "h2_wolfssl.h"
#include "h2_yyjson_json.h"

#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct example_state {
  bool response;
  bool failed;
} example_state_t;

static void on_event(void* user, h2_onenet_t* client,
                     const h2_onenet_event_t* event) {
  example_state_t* state = user;
  if (event->type == H2_ONENET_CONNECTED) {
    puts("ONENET connected");
  } else if (event->type == H2_ONENET_RESPONSE) {
    printf("ONENET response result=%d code=%d\n", event->result, event->code);
    state->response = true;
    state->failed = event->result != H2_PAL_OK || event->code != 200;
  } else if (event->type == H2_ONENET_COMMAND) {
    /* This transport example has no product actuator. An explicit error is
     * preferable to pretending that a device command was applied. */
    const h2_pal_result_t rc =
        h2_onenet_reply(client, event->command_handle, 501, NULL, 0u);
    printf("ONENET command operation=%d reply=%d\n", (int)event->operation, rc);
    if (rc != H2_PAL_OK) state->failed = true;
  } else if (event->type == H2_ONENET_ERROR) {
    fprintf(stderr, "ONENET error=%d\n", event->result);
    state->failed = true;
  }
}

static bool number(const char* text, unsigned long maximum,
                   unsigned long* out) {
  if (text == NULL || text[0] < '0' || text[0] > '9') return false;
  char* end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == NULL || *end != '\0' || value == 0u ||
      value > maximum)
    return false;
  *out = value;
  return true;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    fputs(
        "Usage: onenet HOST PORT PRODUCT_ID DEVICE_NAME\n"
        "Set ONENET_PARAMS_JSON and ONENET_ACCESS_TOKEN or ONENET_DEVICE_KEY.\n"
        "TLS is enabled unless ONENET_PLAINTEXT=1; ONENET_CA_FILE is optional.\n",
        stderr);
    return 2;
  }
  unsigned long port = 0u;
  if (!number(argv[2], 65535u, &port)) return 2;
  const char* params = getenv("ONENET_PARAMS_JSON");
  const char* supplied_token = getenv("ONENET_ACCESS_TOKEN");
  const char* key = getenv("ONENET_DEVICE_KEY");
  if (params == NULL || params[0] == '\0' ||
      ((supplied_token == NULL || supplied_token[0] == '\0') &&
       (key == NULL || key[0] == '\0'))) {
    fputs("Missing OneNET params or access credentials\n", stderr);
    return 2;
  }
  const h2_pal_mem_api_t* mem = h2_desktop_platform_default_allocator();
  const h2_pal_time_api_t* clock = h2_desktop_platform_time_api();
  h2_wolfssl_config_t crypto_config = {.mem = *mem, .entropy = host_entropy};
  h2_pal_result_t rc = h2_wolfssl_init(&crypto_config);
  if (rc != H2_PAL_OK) return 2;
  h2_coremqtt_t* mqtt_provider = NULL;
  h2_yyjson_json_t* json_provider = NULL;
  h2_onenet_t* client = NULL;
  h2_pal_mqtt_api_t mqtt = {0};
  char token[H2_ONENET_TOKEN_CAPACITY] = {0};
  uint8_t* ca = NULL;
  size_t ca_len = 0u;
  example_state_t state = {0};
  const h2_coremqtt_config_t mqtt_config = {.allocator = mem,
                                            .net = host_net(),
                                            .time = clock,
                                            .incoming_publish_records = 4u,
                                            .outgoing_publish_records = 4u};
  rc = h2_coremqtt_create(&mqtt_config, &mqtt_provider, &mqtt);
  if (rc != H2_PAL_OK) goto cleanup;
  rc = h2_yyjson_json_create(mem, &json_provider);
  if (rc != H2_PAL_OK) goto cleanup;
  const time_t unix_time = time(NULL);
  if (unix_time <= 0) {
    rc = H2_PAL_TIME_ERR_UNCALIBRATED;
    goto cleanup;
  }
  uint64_t request_nonce = 0u;
  rc = host_entropy(NULL, (uint8_t*)&request_nonce, sizeof(request_nonce));
  if (rc != H2_PAL_OK) goto cleanup;
  if (supplied_token == NULL || supplied_token[0] == '\0') {
    rc = h2_onenet_token_generate(h2_wolfssl_crypto_api(), argv[3], argv[4],
                                  key, (uint64_t)unix_time + 3600u, token,
                                  sizeof(token));
    if (rc != H2_PAL_OK) goto cleanup;
    supplied_token = token;
  }
  const char* ca_path = getenv("ONENET_CA_FILE");
  if (ca_path != NULL && ca_path[0] != '\0') {
    ca = h2_pal_mem_alloc(mem, 16384u);
    if (ca == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto cleanup;
    }
    FILE* file = fopen(ca_path, "rb");
    if (file == NULL) {
      rc = H2_PAL_ERR_IO;
      goto cleanup;
    }
    ca_len = fread(ca, 1u, 16384u, file);
    const bool valid = !ferror(file) && feof(file) && ca_len != 0u;
    fclose(file);
    if (!valid) {
      rc = H2_PAL_ERR_FORMAT;
      goto cleanup;
    }
  }
  const h2_pal_net_tls_config_t tls = {
      .server_name = argv[1],
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .root_ca_pem = ca,
      .root_ca_pem_len = ca_len,
  };
  const char* plaintext = getenv("ONENET_PLAINTEXT");
  const h2_onenet_config_t config = {
      .allocator = mem,
      .mqtt = &mqtt,
      .json = h2_yyjson_json_api(json_provider),
      .time = clock,
      .product_id = argv[3],
      .device_name = argv[4],
      .endpoint = {{argv[1], strlen(argv[1])}, (uint16_t)port},
      .transport = plaintext != NULL && strcmp(plaintext, "1") == 0
                       ? H2_PAL_MQTT_TRANSPORT_TCP
                       : H2_PAL_MQTT_TRANSPORT_TLS,
      .tls = &tls,
      .limits = {16384u, 24u, 2048u, 8u, 8u},
      .mqtt_buffer_bytes = 32768u,
      .keepalive_sec = 60u,
      .operation_timeout_ms = 5000u,
      .response_timeout_ms = 5000u,
      .initial_request_id = request_nonce % H2_ONENET_MAX_REQUEST_ID + 1u,
      .on_event = on_event,
      .event_user = &state,
  };
  rc = h2_onenet_create(&config, &client);
  if (rc != H2_PAL_OK) goto cleanup;
  rc = h2_onenet_connect(client, supplied_token);
  if (rc != H2_PAL_OK) goto cleanup;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(clock, &start);
  if (rc != H2_PAL_OK) goto cleanup;
  bool published = false;
  while (!state.response && !state.failed) {
    rc = h2_onenet_process(client, 50u);
    if (rc != H2_PAL_OK) break;
    if (!published && h2_onenet_is_connected(client)) {
      uint64_t id = 0u;
      rc = h2_onenet_publish(client, H2_ONENET_PROPERTY_POST,
                             (const uint8_t*)params, strlen(params), &id);
      if (rc != H2_PAL_OK) break;
      published = true;
    }
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(clock, &now);
    if (rc != H2_PAL_OK) break;
    if (h2_pal_time_elapsed_ms(start, now) >= 12000u) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
  }
  if (rc == H2_PAL_OK && (!state.response || state.failed))
    rc = H2_PAL_ERR_UNAVAILABLE;
cleanup:
  if (client != NULL) {
    const h2_pal_result_t disconnect = h2_onenet_disconnect(client);
    if (rc == H2_PAL_OK) rc = disconnect;
    (void)h2_onenet_destroy(&client);
  }
  if (json_provider != NULL) {
    const h2_pal_result_t destroyed = h2_yyjson_json_destroy(&json_provider);
    if (rc == H2_PAL_OK) rc = destroyed;
  }
  h2_coremqtt_destroy(mqtt_provider);
  const h2_pal_result_t destroyed = h2_wolfssl_deinit();
  if (rc == H2_PAL_OK) rc = destroyed;
  h2_pal_mem_free(mem, ca);
  /* Prevent the generated token from lingering in this example's stack. */
  volatile char* wipe = token;
  for (size_t i = 0u; i < sizeof(token); ++i) wipe[i] = 0;
  printf("ONENET finished result=%d\n", rc);
  return rc == H2_PAL_OK ? 0 : 1;
}
