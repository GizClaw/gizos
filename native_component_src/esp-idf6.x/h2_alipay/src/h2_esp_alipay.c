#include "h2_esp_alipay.h"
#include "h2_esp_alipay_internal.h"
#include "alipay_bind.h"
#include "h2_alipay_sdk_identity.h"
#include "alipay_account_manage.h"
#include "vendor_os.h"
#include "vendor_device_info.h"
#include "vendor_psa.h"
#include "vendor_cert.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static h2_esp_alipay_config_t s_config;
static SemaphoreHandle_t s_lock;
static h2_alipay_initialization_t s_initialization;
static h2_alipay_trace_t s_trace;
static uint32_t s_parent;
uint64_t h2_esp_alipay_now(void) {
  uint64_t value = 0;
  (void)h2_pal_time_get_monotonic_ms(s_config.time, &value);
  return value;
}
void h2_esp_alipay_trace(const char* operation, const char* boundary,
                         int result, uint32_t item, uint32_t connection,
                         uint64_t started, const char* detail,
                         const void* bytes, size_t length) {
  h2_alipay_trace_event_t event = {0};
  event.call_id = ++s_trace.next_call;
  event.parent_id = s_parent;
  event.connection_id = connection;
  event.result = result;
  event.item = item;
  event.mono_ms = h2_esp_alipay_now();
  event.duration_ms = event.mono_ms >= started ? event.mono_ms - started : 0;
  h2_pal_time_wall_status_t wall = {0};
  if (h2_pal_time_get_wall_status(s_config.time, &wall) == H2_PAL_OK) {
    event.wall_source = (uint32_t)wall.source;
    event.utc_valid =
        h2_pal_time_get_wall_ms(s_config.time, &event.utc_ms) == H2_PAL_OK;
  }
  (void)snprintf(event.operation, sizeof(event.operation), "%s", operation);
  (void)snprintf(event.boundary, sizeof(event.boundary), "%s", boundary);
  if (detail != NULL)
    (void)snprintf(event.detail, sizeof(event.detail), "%s", detail);
  h2_alipay_trace_push(&s_trace, &event, bytes, length);
}

const h2_esp_alipay_config_t* h2_esp_alipay_config(void) { return &s_config; }

static h2_pal_result_t pre_init(void* user, int* sdk_rc, bool* first) {
  (void)user;
  if (sdk_rc == NULL || first == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
    return H2_PAL_ERR_UNAVAILABLE;
  *first = !s_initialization.attempted;
  if (!s_initialization.attempted) {
    const uint64_t started = h2_esp_alipay_now();
    h2_esp_alipay_trace("alipay_pre_init.enter", "sdk", 0, 0, 0, started, NULL,
                        NULL, 0);
    s_parent = s_trace.next_call;
    (void)h2_alipay_initialize_once(&s_initialization, alipay_pre_init, sdk_rc,
                                    first);
    h2_esp_alipay_trace("alipay_pre_init.return", "sdk",
                        s_initialization.sdk_rc, 0, 0, started, NULL, NULL, 0);
    s_parent = 0;
  }
  if (!*first)
    h2_esp_alipay_trace("alipay_pre_init.reused", "sdk",
                        s_initialization.sdk_rc, 0, 0, h2_esp_alipay_now(),
                        NULL, NULL, 0);
  *sdk_rc = s_initialization.sdk_rc;
  xSemaphoreGive(s_lock);
  return H2_PAL_OK;
}

static retval_e copy_text(const char* text, uint8_t* out, uint32_t* length) {
  if (text == NULL || out == NULL || length == NULL)
    return ALIPAY_RV_WRONG_PARAM;
  const size_t size = strlen(text);
  if (size == 0u || size >= *length) return ALIPAY_RV_WRONG_PARAM;
  memcpy(out, text, size + 1u);
  *length = (uint32_t)size;
  return ALIPAY_RV_OK;
}

retval_e alipay_get_companyName(uint8_t* out, uint32_t* length) {
  uint64_t started = h2_esp_alipay_now();
  retval_e rc = copy_text(s_config.company, out, length);
  h2_esp_alipay_trace("identity.company", "hal", rc, rc == 0 ? *length : 0, 0,
                      started, "product_configuration", NULL, 0);
  return rc;
}
retval_e alipay_get_productModel(uint8_t* out, uint32_t* length) {
  uint64_t started = h2_esp_alipay_now();
  retval_e rc = copy_text(s_config.model, out, length);
  h2_esp_alipay_trace("identity.model", "hal", rc, rc == 0 ? *length : 0, 0,
                      started, "product_configuration", NULL, 0);
  return rc;
}
uint32_t alipay_get_productType(void) {
  h2_esp_alipay_trace("identity.product_type", "hal", 0, s_config.product_type,
                      0, h2_esp_alipay_now(), "product_configuration", NULL, 0);
  return s_config.product_type;
}

static retval_e identity(bool imei, uint8_t* out, uint32_t* length) {
  if (out == NULL || length == NULL || *length == 0u)
    return ALIPAY_RV_WRONG_PARAM;
  char value[49] = {0};
  h2_pal_result_t rc;
  if (imei && s_config.has_modem) {
    rc = s_config.read_imei != NULL
             ? s_config.read_imei(s_config.identity_user, value, sizeof(value))
             : H2_PAL_ERR_UNAVAILABLE;
  } else {
    rc = s_config.read_sn(s_config.identity_user, value, sizeof(value));
  }
  const char* end = memchr(value, '\0', sizeof(value));
  if (rc != H2_PAL_OK || end == NULL || end == value ||
      (size_t)(end - value) > (imei ? 48u : 32u))
    return ALIPAY_RV_HAL_ERROR;
  for (const char* c = value; c != end; ++c)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
          (*c >= 'a' && *c <= 'z') || *c == '_'))
      return ALIPAY_RV_HAL_ERROR;
  retval_e result = copy_text(value, out, length);
  h2_esp_alipay_trace(imei ? "identity.imei" : "identity.sn", "hal", result,
                      (uint32_t)(end - value), 0, h2_esp_alipay_now(),
                      imei && s_config.has_modem ? "modem" : "product_sn", NULL,
                      0);
  return result;
}
retval_e alipay_get_sn(uint8_t* out, uint32_t* length) {
  return identity(false, out, length);
}
retval_e alipay_get_imei(uint8_t* out, uint32_t* length) {
  return identity(true, out, length);
}
retval_e alipay_get_mac(uint8_t* out, uint32_t* length) {
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return ALIPAY_RV_HAL_ERROR;
  char text[18];
  (void)snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
  retval_e result = copy_text(text, out, length);
  h2_esp_alipay_trace("identity.mac", "hal", result, 17, 0, h2_esp_alipay_now(),
                      "ESP_MAC_BT", NULL, 0);
  return result;
}

static h2_pal_result_t ready_impl(void* user, uint32_t feature,
                                  h2_alipay_blocker_t* blocker) {
  (void)user;
  if (blocker == NULL) return H2_PAL_ERR_INVALID_ARG;
  *blocker = H2_ALIPAY_BLOCKER_SECURITY_SCHEME;
  const h2_esp_alipay_secure_t* secure = s_config.secure;
  if (secure == NULL || secure->check_ready == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  *blocker = H2_ALIPAY_BLOCKER_SECURE_STORAGE;
  if (secure->read == NULL || secure->write == NULL || secure->remove == NULL ||
      secure->exists == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  const uint32_t shapes = FEATURE_ELLIPSE | FEATURE_SQUARE | FEATURE_CIRCLE;
  const uint32_t functions =
      FEATURE_PAYCODE | FEATURE_TRANSITCODE | FEATURE_NFCPAY | FEATURE_BLEPAY;
  const uint32_t shape = feature & shapes;
  *blocker = H2_ALIPAY_BLOCKER_FEATURES;
  if ((feature & functions) == 0u || (feature & ~(shapes | functions)) != 0u ||
      shape == 0u || (shape & (shape - 1u)) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = secure->check_ready(secure->user, feature, blocker);
  if (rc != H2_PAL_OK) return rc;
  *blocker = H2_ALIPAY_BLOCKER_IDENTITY;
  uint8_t value[49];
  uint32_t length = sizeof(value);
  if (alipay_get_sn(value, &length) != ALIPAY_RV_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  length = sizeof(value);
  if (alipay_get_imei(value, &length) != ALIPAY_RV_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  length = sizeof(value);
  if (alipay_get_mac(value, &length) != ALIPAY_RV_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  *blocker = H2_ALIPAY_BLOCKER_NETWORK;
  if (!alipay_is_network_connected()) return H2_PAL_ERR_UNAVAILABLE;
  *blocker = H2_ALIPAY_BLOCKER_TIME;
  uint64_t ms = 0u;
  rc = h2_pal_time_get_wall_ms(s_config.time, &ms);
  if (rc != H2_PAL_OK || ms / 1000u > UINT32_MAX) return H2_PAL_ERR_UNAVAILABLE;
  *blocker = H2_ALIPAY_BLOCKER_NONE;
  return H2_PAL_OK;
}

static h2_pal_result_t ready(void* user, uint32_t feature,
                             h2_alipay_blocker_t* blocker) {
  const uint64_t started = h2_esp_alipay_now();
  h2_pal_result_t rc = ready_impl(user, feature, blocker);
  char detail[48];
  (void)snprintf(detail, sizeof(detail), "feature=%u", (unsigned)feature);
  h2_esp_alipay_trace("commissioning.check", "hal_secure", rc,
                      blocker != NULL ? (uint32_t)*blocker : 0, 0, started,
                      detail, NULL, 0);
  return rc;
}

static h2_pal_result_t binding_status(void* user, bool* bound) {
  (void)user;
  if (bound == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_pal_result_t rc = H2_PAL_ERR_INVALID_STATE;
  if (s_initialization.attempted && s_initialization.sdk_rc == 0) {
    *bound = alipay_get_binding_status();
    rc = H2_PAL_OK;
  }
  xSemaphoreGive(s_lock);
  return rc;
}

static h2_pal_result_t binding_code(void* user, uint32_t feature, char* code,
                                    size_t capacity, size_t* length,
                                    int* sdk_rc) {
  (void)user;
  if (code == NULL || length == NULL || sdk_rc == NULL || capacity < 2u ||
      capacity > INT_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  memset(code, 0, capacity);
  *length = 0u;
  *sdk_rc = INT_MIN;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_pal_result_t rc = H2_PAL_ERR_INVALID_STATE;
  if (s_initialization.attempted && s_initialization.sdk_rc == 0) {
    h2_alipay_blocker_t blocker;
    rc = ready(NULL, feature, &blocker);
    if (rc == H2_PAL_OK && alipay_get_binding_status())
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) {
      int size = (int)capacity - 1;
      const uint64_t started = h2_esp_alipay_now();
      h2_esp_alipay_trace("alipay_get_binding_code.enter", "sdk", 0, feature, 0,
                          started, NULL, NULL, 0);
      s_parent = s_trace.next_call;
      *sdk_rc = alipay_get_binding_code(feature, code, &size);
      h2_esp_alipay_trace("alipay_get_binding_code.return", "sdk", *sdk_rc,
                          size > 0 ? (uint32_t)size : 0, 0, started, NULL, NULL,
                          0);
      s_parent = 0;
      if (*sdk_rc != ALIPAY_RV_OK)
        rc = H2_PAL_ERR_IO;
      else if (size <= 0 || (size_t)size >= capacity || code[0] == '\0' ||
               memchr(code, '\0', (size_t)size) != NULL)
        rc = H2_PAL_ERR_INVALID_STATE;
      else {
        code[size] = '\0';
        *length = (size_t)size;
      }
    }
  }
  if (rc != H2_PAL_OK) memset(code, 0, capacity);
  xSemaphoreGive(s_lock);
  return rc;
}

static const h2_alipay_vtable_t s_vtable = {
    .pre_init = pre_init,
    .check_ready = ready,
    .binding_status = binding_status,
    .get_binding_code = binding_code,
};
static const h2_alipay_api_t s_api = {
    .vtable = &s_vtable,
    .source = H2_ALIPAY_SOURCE_VENDOR_SDK,
    .trace = &s_trace,
    .sdk_manifest_sha256 = H2_ALIPAY_SDK_MANIFEST_SHA256,
};
h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config) {
  if (s_lock != NULL) return H2_PAL_ERR_INVALID_STATE;
  if (config == NULL || config->mem == NULL || config->time == NULL ||
      config->log == NULL || config->read_sn == NULL ||
      config->company == NULL || config->model == NULL ||
      strlen(config->company) == 0u || strlen(config->company) > 15u ||
      strlen(config->model) == 0u || strlen(config->model) > 23u ||
      config->product_type > 5u)
    return H2_PAL_ERR_INVALID_ARG;
  SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  if (lock == NULL) return H2_PAL_ERR_NO_MEMORY;
  s_config = *config;
  s_lock = lock;
  return H2_PAL_OK;
}
const h2_alipay_api_t* h2_esp_alipay_api(void) {
  return s_lock != NULL ? &s_api : NULL;
}

uint32_t alipay_get_timestamp(void) {
  uint64_t ms = 0u;
  return h2_pal_time_get_wall_ms(s_config.time, &ms) == H2_PAL_OK &&
                 ms / 1000u <= UINT32_MAX
             ? (uint32_t)(ms / 1000u)
             : 0u;
}
void alipay_set_system_time(int32_t seconds) {
  if (seconds > 0)
    (void)h2_pal_time_set_wall_ms(s_config.time, (uint64_t)seconds * 1000u);
}
uint32_t alipay_get_compile_timestamp(void) {
  /* SDK requires a build timestamp, never substitute a runtime clock. */
  struct tm value = {0};
  char month[4];
  int day, year, hour, minute, second;
  if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
      sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    return 0u;
  const char* start = strstr("JanFebMarAprMayJunJulAugSepOctNovDec", month);
  if (start == NULL) return 0u;
  value.tm_year = year - 1900;
  value.tm_mon = (int)(start - "JanFebMarAprMayJunJulAugSepOctNovDec") / 3;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return (uint32_t)timegm(&value);
}
void* alipay_malloc(uint32_t size) {
  void* ptr = h2_pal_mem_alloc(s_config.mem, size);
  if (ptr != NULL) memset(ptr, 0, size);
  return ptr;
}
void* alipay_calloc(uint32_t count, uint32_t size) {
  if (size != 0u && count > UINT32_MAX / size) return NULL;
  return alipay_malloc(count * size);
}
void* alipay_realloc(void* ptr, uint32_t size) {
  return h2_pal_mem_realloc(s_config.mem, ptr, size);
}
void alipay_free(void* ptr) { h2_pal_mem_free(s_config.mem, ptr); }
int32_t get_Rng_number(uint8_t* data, unsigned size) {
  if (data == NULL) return -1;
  esp_fill_random(data, size);
  return 0;
}
int alipay_rand(void) {
  uint32_t value;
  do {
    value = esp_random() & INT_MAX;
  } while (value == 0u);
  return (int)value;
}
void alipay_srand(unsigned int seed) { (void)seed; }
int ALIPAY_mbedtls_hardware_poll(void* user, unsigned char* output,
                                 size_t length, size_t* written) {
  (void)user;
  if (output == NULL || written == NULL) return -1;
  esp_fill_random(output, length);
  *written = length;
  return 0;
}
void alipay_log_ext(const char* format, ...) {
  /* Vendor logs can contain SN, tokens and certificate material. Preserve only
   * caller-owned step/return diagnostics, never SDK payloads in shared logs. */
  char text[256];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n < 0) return;
  const char* header = strstr(text, "Operation-Type:");
  if (header != NULL) {
    header += strlen("Operation-Type:");
    while (*header == ' ' || *header == '\t') ++header;
    char operation[80];
    size_t i = 0;
    while (i + 1 < sizeof(operation) &&
           ((header[i] >= 'a' && header[i] <= 'z') ||
            (header[i] >= 'A' && header[i] <= 'Z') ||
            (header[i] >= '0' && header[i] <= '9') || header[i] == '.' ||
            header[i] == '_' || header[i] == '-')) {
      operation[i] = header[i];
      ++i;
    }
    operation[i] = '\0';
    if (i != 0)
      h2_esp_alipay_trace("rpc.operation_type", "sdk_log_observed", 0, 0, 0,
                          h2_esp_alipay_now(), operation, NULL, 0);
  }
  size_t observed = (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1;
  h2_esp_alipay_trace(
      "vendor.log", "sdk_log", 0, (uint32_t)n, 0, h2_esp_alipay_now(),
      (size_t)n > observed ? "formatted_prefix" : "formatted_message", text,
      observed);
}
void alipay_iot_get_local_time(alipay_iot_local_time* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  const time_t seconds = alipay_get_timestamp();
  struct tm value;
  if (seconds == 0 || gmtime_r(&seconds, &value) == NULL) return;
  out->year = value.tm_year + 1900;
  out->month = value.tm_mon + 1;
  out->day = value.tm_mday;
  out->wday = value.tm_wday;
  out->hour = value.tm_hour;
  out->minute = value.tm_min;
  out->second = value.tm_sec;
  out->isdst = 0;
}
int alipay_is_network_connected(void) {
  esp_netif_t* netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info;
  return netif != NULL && esp_netif_is_netif_up(netif) &&
         esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0u;
}
void alipay_binding_process_notify(int status) { (void)status; }
int alipay_psa_storage_read(uint32_t index, uint8_t* data, uint32_t* size) {
  const h2_esp_alipay_secure_t* s = s_config.secure;
  uint64_t started = h2_esp_alipay_now();
  int rc =
      s != NULL && s->read != NULL ? s->read(s->user, index, data, size) : -1;
  h2_esp_alipay_trace("storage.read", "hal_secure", rc, index, 0, started, NULL,
                      NULL, 0);
  return rc;
}
int alipay_psa_storage_write(uint32_t index, const uint8_t* data,
                             uint32_t size) {
  const h2_esp_alipay_secure_t* s = s_config.secure;
  uint64_t started = h2_esp_alipay_now();
  int rc =
      s != NULL && s->write != NULL ? s->write(s->user, index, data, size) : -1;
  h2_esp_alipay_trace("storage.write", "hal_secure", rc, index, 0, started,
                      NULL, NULL, 0);
  return rc;
}
int alipay_psa_storage_delete(uint32_t index) {
  const h2_esp_alipay_secure_t* s = s_config.secure;
  uint64_t started = h2_esp_alipay_now();
  int rc = s != NULL && s->remove != NULL ? s->remove(s->user, index) : -1;
  h2_esp_alipay_trace("storage.remove", "hal_secure", rc, index, 0, started,
                      NULL, NULL, 0);
  return rc;
}
int alipay_psa_storage_exists(uint32_t index) {
  const h2_esp_alipay_secure_t* s = s_config.secure;
  uint64_t started = h2_esp_alipay_now();
  int rc = s != NULL && s->exists != NULL ? s->exists(s->user, index) : -1;
  h2_esp_alipay_trace("storage.exists", "hal_secure", rc, index, 0, started,
                      NULL, NULL, 0);
  return rc;
}
int alipay_psa_storage_reset(void) { return -1; }
int alipay_secure_save_data(void* data, uint32_t size) {
  (void)data;
  (void)size;
  return -1;
}
