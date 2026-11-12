#include "h2_alipay_tiga_protocol.h"

#include "h2_alipay_client.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static h2_runtime_t* s_runtime;
static const h2_alipay_api_t* s_reference;
static h2_alipay_api_t s_api;
static h2_alipay_client_config_t s_config;
static h2_alipay_client_result_t s_result;
static char s_sn[49], s_mac[18];
static bool s_initialized;
static bool s_reference_ready;

static void record(const char* operation, int result, uint32_t item,
                   const char* detail) {
  h2_alipay_trace_event_t event = {
      .call_id = ++s_api.trace->next_call,
      .result = result,
      .item = item,
  };
  (void)h2_pal_time_get_monotonic_ms(s_runtime->time, &event.mono_ms);
  event.utc_valid =
      h2_pal_time_get_wall_ms(s_runtime->time, &event.utc_ms) == H2_PAL_OK;
  (void)snprintf(event.operation, sizeof(event.operation), "%s", operation);
  (void)snprintf(event.boundary, sizeof(event.boundary), "independent");
  if (detail != NULL)
    (void)snprintf(event.detail, sizeof(event.detail), "%s", detail);
  h2_alipay_trace_push(s_api.trace, &event, NULL, 0u);
}

static h2_pal_result_t pre_init(void* user, int* implementation_rc,
                                bool* first) {
  (void)user;
  if (implementation_rc == NULL || first == NULL) return H2_PAL_ERR_INVALID_ARG;
  *first = !s_initialized;
  if (!s_initialized) {
    s_initialized = true;
    if (s_reference != NULL) {
      bool reference_first = false;
      int sdk_rc = 0;
      int rc = s_reference->vtable->pre_init(s_reference->user, &sdk_rc,
                                             &reference_first);
      printf(
          "H2_ALIPAY_COMPARE phase=pre_init source=vendor_sdk rc=%d sdk_rc=%d\n",
          rc, sdk_rc);
      s_reference_ready = rc == H2_PAL_OK && sdk_rc == 0;
    }
  }
  *implementation_rc = H2_PAL_OK;
  record(*first ? "independent.pre_init" : "independent.pre_init.reused",
         H2_PAL_OK, 0u, NULL);
  return H2_PAL_OK;
}

static h2_pal_result_t get_code(void* user, uint32_t feature, char* code,
                                size_t capacity, size_t* length,
                                int* implementation_rc) {
  (void)user;
  if (code == NULL || capacity == 0u || length == NULL ||
      implementation_rc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  code[0] = '\0';
  *length = 0u;
  if (!s_initialized) {
    *implementation_rc = H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (s_reference_ready) {
    char reference_code[H2_ALIPAY_BINDING_CODE_CAPACITY];
    size_t reference_length = 0u;
    int sdk_rc = 0;
    int rc = s_reference->vtable->get_binding_code(
        s_reference->user, feature, reference_code, sizeof(reference_code),
        &reference_length, &sdk_rc);
    printf(
        "H2_ALIPAY_COMPARE phase=binding_code source=vendor_sdk "
        "rc=%d sdk_rc=%d length=%zu\n",
        rc, sdk_rc, reference_length);
    memset(reference_code, 0, sizeof(reference_code));
  }
  s_config.feature = feature;
  int rc = h2_alipay_client_get_binding_code(&s_config, &s_result);
  printf(
      "H2_ALIPAY_COMPARE phase=binding_code source=independent "
      "rc=%d stage=%d http_status=%d rpc_status=%d registered=%u requests=%u length=%zu\n",
      rc, (int)s_result.stage, s_result.http_status, s_result.rpc_status,
      (unsigned)s_result.registered, s_result.requests, s_result.length);
  record("independent.http_status", s_result.http_status, 0u, NULL);
  record("independent.rpc_status", s_result.rpc_status, 0u, NULL);
  if (rc == H2_PAL_OK) {
    if (s_result.length >= capacity) {
      rc = H2_PAL_ERR_NO_SPACE;
    } else {
      memcpy(code, s_result.code, s_result.length + 1u);
      *length = s_result.length;
    }
  }
  if (rc != H2_PAL_OK) h2_alipay_client_result_clear(&s_result);
  *implementation_rc = rc;
  record("independent.binding_code.return", rc, (uint32_t)*length, NULL);
  fflush(stdout);
  return rc;
}

static uint32_t build_timestamp(void) {
  struct tm value = {0};
  char month[4];
  int day, year, hour, minute, second;
  if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
      sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    return 0u;
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* start = strstr(months, month);
  if (start == NULL) return 0u;
  value.tm_year = year - 1900;
  value.tm_mon = (int)(start - months) / 3;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return (uint32_t)timegm(&value);
}

h2_pal_result_t h2_alipay_tiga_protocol_configure(
    h2_runtime_t* runtime, const h2_alipay_api_t* reference) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_runtime != NULL) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_alipay_tiga_read_sn(runtime, s_sn, sizeof(s_sn));
  uint8_t mac[6];
  if (rc == H2_PAL_OK)
    rc = esp_read_mac(mac, ESP_MAC_BT) == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK) return rc;
  (void)snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                 (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  s_config = (h2_alipay_client_config_t){
      .mem = runtime->mem,
      .time = runtime->time,
      .http = runtime->http,
      .pref = runtime->pref,
      .crypto = runtime->crypto,
      .identity = {.sn = s_sn, .imei = s_sn, .mac = s_mac, .model = "H-106TG"},
      .company = "HAIVIVI",
      .pref_namespace = "alipay_direct",
      .feature = 16385u,
      .compile_timestamp_s = build_timestamp(),
  };
  h2_alipay_trace_t* trace =
      reference != NULL ? reference->trace
                        : h2_pal_mem_alloc(runtime->mem, sizeof(*trace));
  if (trace == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (reference == NULL) h2_alipay_trace_reset(trace, false);
  static const h2_alipay_vtable_t vtable = {.pre_init = pre_init,
                                            .get_binding_code = get_code};
  s_api = (h2_alipay_api_t){
      .vtable = &vtable,
      .source = H2_ALIPAY_SOURCE_INDEPENDENT,
      .trace = trace,
      .sdk_manifest_sha256 =
          reference != NULL ? reference->sdk_manifest_sha256 : NULL,
  };
  s_runtime = runtime;
  s_reference = reference;
  return H2_PAL_OK;
}

const h2_alipay_api_t* h2_alipay_e2e_target_api(void) {
  return s_runtime != NULL ? &s_api : NULL;
}
