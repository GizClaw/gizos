#include "h2_alipay_sdk.h"
#include "h2_alipay_sdk_internal.h"
#include "h2_alipay_sdk_socket_abi.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_crypto.h"
#include "h2_app_test_time.h"
#include "h2_app_test_pref.h"
#include "alipay_bind.h"
#include "alipay_account_manage.h"
#include "vendor_os.h"
#include "vendor_device_info.h"
#include "vendor_psa.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static unsigned init_calls, code_calls;
static int code_result;
static h2_pal_result_t identity_result;
static int netif_result, dns_result;
static uint32_t netif_flags;
static unsigned entropy_calls;
int32_t get_Rng_number(uint8_t* data, unsigned size);
int ALIPAY_mbedtls_hardware_poll(void*, unsigned char*, size_t, size_t*);

/* Controlled SDK boundary: these tests verify adapter semantics, not service
 * or original binary behavior. Device qualification uses the delivered .a. */
int alipay_pre_init(void) {
  ++init_calls;
  return 0;
}
bool alipay_get_binding_status(void) { return false; }
retval_e alipay_get_binding_code(uint32_t feature, char* code, int* size) {
  assert(feature == (FEATURE_PAYCODE | FEATURE_SQUARE));
  ++code_calls;
  if (code_result != 0) {
    memset(code, 'X', (size_t)*size);
    return code_result;
  }
  assert(*size > 5);
  memcpy(code, "test", 5);
  *size = 4;
  return 0;
}
static h2_pal_result_t text_identity(void* user, char* out, size_t size) {
  (void)user;
  if (identity_result != H2_PAL_OK) return identity_result;
  assert(size > 15);
  memcpy(out, "123456789012345", 16);
  return H2_PAL_OK;
}
static h2_pal_result_t mac_identity(void* user, uint8_t out[6]) {
  (void)user;
  memset(out, 0x12, 6);
  return identity_result;
}
static h2_pal_result_t net_status(void* user, const h2_pal_netif_ref_t* ref,
                                  h2_pal_netif_status_t* out) {
  (void)user;
  assert(h2_pal_netif_ref_is_default(ref));
  if (netif_result != H2_PAL_OK) return netif_result;
  *out = (h2_pal_netif_status_t){.kind = H2_PAL_NETIF_KIND_WIFI_STA,
                                 .flags = netif_flags};
  return H2_PAL_OK;
}
static h2_pal_result_t resolve(void* user, const char* host,
                               h2_pal_net_family_t family,
                               h2_pal_net_addr_list_t* out) {
  (void)user;
  assert(strcmp(host, "sdk-test.invalid") == 0);
  assert(family == H2_PAL_NET_FAMILY_IPV4);
  if (dns_result != H2_PAL_OK) return dns_result;
  out->count = 1;
  out->addrs[0] = (h2_pal_net_addr_t){.family = family, .ip = {127, 0, 0, 1}};
  return H2_PAL_OK;
}
static h2_pal_result_t entropy(void* user, uint8_t* out, size_t length) {
  h2_pal_result_t* result = user;
  ++entropy_calls;
  if (*result != H2_PAL_OK) return *result;
  memset(out, 0xa5, length);
  return H2_PAL_OK;
}
int main(void) {
  h2_app_test_mem_t mem = {0};
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 100);
  assert(h2_pal_time_set_wall_ms(&time.api, 1800000000123u) == H2_PAL_OK);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &mem.api) == H2_PAL_OK);
  static const h2_pal_netif_vtable_t netif_vtable = {.get_status = net_status};
  h2_pal_netif_api_t netif = {NULL, &netif_vtable};
  static const h2_pal_net_vtable_t net_vtable = {.resolve_all = resolve};
  h2_pal_net_api_t net = {NULL, &net_vtable};
  static const h2_pal_crypto_vtable_t crypto_vtable = {.random = entropy};
  h2_pal_result_t random_result = H2_PAL_OK;
  h2_pal_crypto_api_t crypto = {&random_result, &crypto_vtable};
  const h2_pal_log_api_t log = {0};
  h2_alipay_sdk_config_t config = {.mem = &mem.api,
                                   .time = &time.api,
                                   .log = &log,
                                   .sync = &sync.api,
                                   .crypto = &crypto,
                                   .net = &net,
                                   .netif = &netif,
                                   .pref = &pref.api,
                                   .read_sn = text_identity,
                                   .read_imei = text_identity,
                                   .read_mac = mac_identity,
                                   .has_modem = true,
                                   .company = "test",
                                   .model = "test",
                                   .product_type = 3};
  const size_t baseline_blocks = mem.live_blocks;
  /* No SDK call or singleton state remains after a failed configure. */
  mem.fail_at = mem.calls + 1;
  assert(h2_alipay_sdk_configure(&config) == H2_PAL_ERR_NO_MEMORY);
  assert(h2_alipay_sdk_api() == NULL && mem.live_blocks == baseline_blocks);
  mem.fail_at = 0;
  sync.create =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_NO_SPACE, .remaining = 1};
  assert(h2_alipay_sdk_configure(&config) == H2_PAL_ERR_NO_SPACE);
  assert(h2_alipay_sdk_api() == NULL && mem.live_blocks == baseline_blocks);
  assert(h2_alipay_sdk_configure(&config) == H2_PAL_OK);
  assert(h2_alipay_sdk_configure(&config) == H2_PAL_ERR_INVALID_STATE);
  const h2_alipay_api_t* api = h2_alipay_sdk_api();
  assert(api != NULL);
  int rc;
  bool first;
  assert(api->vtable->pre_init(api->user, &rc, &first) == H2_PAL_OK &&
         rc == 0 && first);
  assert(api->vtable->pre_init(api->user, &rc, &first) == H2_PAL_OK && !first &&
         init_calls == 1);
  char code[32];
  size_t length;
  assert(api->vtable->get_binding_code(NULL, FEATURE_PAYCODE | FEATURE_SQUARE,
                                       code, sizeof(code), &length,
                                       &rc) == H2_PAL_OK &&
         rc == 0 && length == 4 && strcmp(code, "test") == 0);
  code_result = 57;
  assert(api->vtable->get_binding_code(NULL, FEATURE_PAYCODE | FEATURE_SQUARE,
                                       code, sizeof(code), &length,
                                       &rc) == H2_PAL_ERR_IO &&
         rc == 57 && length == 0 && code[0] == '\0');
  sync.lock = (h2_app_test_fault_t){.result = H2_PAL_ERR_BUSY, .remaining = 1};
  assert(api->vtable->get_binding_code(NULL, FEATURE_PAYCODE | FEATURE_SQUARE,
                                       code, sizeof(code), &length,
                                       &rc) == H2_PAL_ERR_BUSY &&
         code_calls == 2);
  uint8_t bytes[32];
  uint32_t size = sizeof(bytes);
  assert(alipay_get_imei(bytes, &size) == 0 && size == 15);
  identity_result = H2_PAL_ERR_CLOSED;
  size = sizeof(bytes);
  assert(alipay_get_imei(bytes, &size) == ALIPAY_RV_HAL_ERROR);
  identity_result = H2_PAL_OK;
  size = sizeof(bytes);
  assert(alipay_get_mac(bytes, &size) == 0 && size == 17 &&
         strcmp((char*)bytes, "12:12:12:12:12:12") == 0);
  assert(get_Rng_number(bytes, sizeof(bytes)) == 1 && entropy_calls == 1);
  random_result = H2_PAL_ERR_IO;
  assert(get_Rng_number(bytes, sizeof(bytes)) == 0);
  size_t written = 123;
  assert(ALIPAY_mbedtls_hardware_poll(NULL, bytes, sizeof(bytes), &written) ==
             -1 &&
         written == 0);
  random_result = H2_PAL_OK;
  assert(ALIPAY_mbedtls_hardware_poll(NULL, bytes, sizeof(bytes), &written) ==
             0 &&
         written == sizeof(bytes));
  assert(get_Rng_number(NULL, 32) == 0);
  assert(alipay_get_timestamp() == 1800000000u);
  alipay_iot_timeval tv;
  alipay_iot_gettimeofday(&tv);
  assert(tv.tv_sec == 1800000000L && tv.tv_usec == 123000L);
  alipay_task_sleep(25);
  assert(time.last_sleep_ms == 25);
  alipay_iot_mutex mutex = alipay_iot_mutex_create("sdk/test");
  assert(mutex != NULL);
  assert(alipay_iot_mutex_lock(mutex) == 0 &&
         alipay_iot_mutex_unlock(mutex) == 0);
  assert(alipay_iot_mutex_delete(mutex) == 0 &&
         alipay_iot_mutex_lock(NULL) == -1);
  unsigned char ip[4];
  assert(alipay_iot_dns("sdk-test.invalid", ip) == 0 && ip[0] == 127 &&
         ip[3] == 1);
  dns_result = H2_PAL_ERR_NOT_FOUND;
  assert(alipay_iot_dns("sdk-test.invalid", ip) == -1);
  netif_flags = H2_PAL_NETIF_FLAG_UP | H2_PAL_NETIF_FLAG_LINK_UP |
                H2_PAL_NETIF_FLAG_HAS_IPV4;
  assert(alipay_is_network_connected() == 1);
  netif_flags = 0;
  assert(alipay_is_network_connected() == 0);
  netif_result = H2_PAL_ERR_UNAVAILABLE;
  assert(alipay_is_network_connected() == 0);
  assert(alipay_psa_storage_write(8, (const uint8_t*)"abc", 3) == 0);
  size = sizeof(bytes);
  assert(alipay_psa_storage_read(8, bytes, &size) == 0 && size == 3);
  assert(api->trace->dropped == 0);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  /* This bridge deliberately retains its process-lifetime SDK state. */
  h2_app_test_mem_release_all(&mem);
  return 0;
}
