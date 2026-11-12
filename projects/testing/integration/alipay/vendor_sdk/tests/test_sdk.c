#include "h2_alipay_sdk.h"
#include "h2_alipay_storage.h"
#include "h2_alipay_sdk_internal.h"
#include "h2_alipay_sdk_socket_abi.h"
#include "h2_alipay_binding_storage.h"
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
#include "vendor_cert.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

static unsigned init_calls, code_calls;
static unsigned query_calls;
static bool sdk_bound, query_writes, omit_seed, fail_marker_commit;
static bool change_readback, fail_material_delete, fail_material_reset;
static int query_status = ALIPAY_STATUS_UNKNOWN, query_result;
static h2_app_test_pref_t* binding_pref;
static const h2_alipay_storage_t* binding_storage;
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
bool alipay_get_binding_status(void) { return sdk_bound; }
retval_e alipay_query_binding_result(binding_status_e* status) {
  ++query_calls;
  *status = query_status;
  if (query_writes) {
    uint8_t key[32], seed[48];
    memset(key, 0x23, sizeof(key));
    memset(seed, 0x59, sizeof(seed));
    (void)alipay_psa_storage_write(7u, (const uint8_t*)"TEST_UID", 8u);
    (void)alipay_psa_storage_write(6u, (const uint8_t*)"TEST_TID", 8u);
    (void)alipay_psa_storage_write(1u, key, sizeof(key));
    if (!omit_seed) (void)alipay_psa_storage_write(2u, seed, sizeof(seed));
  }
  if (change_readback)
    assert(h2_alipay_storage_write(binding_storage, 7u,
                                   (const uint8_t*)"OTHER_UID",
                                   9u) == H2_PAL_OK);
  if (fail_material_delete || fail_material_reset) {
    binding_pref->commit =
        (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1u};
    if (fail_material_delete)
      assert(alipay_psa_storage_delete(6u) == H2_PAL_ERR_WRITE);
    else
      assert(alipay_psa_storage_reset() == H2_PAL_ERR_WRITE);
  }
  sdk_bound = query_status == ALIPAY_STATUS_BINDING_OK;
  if (fail_marker_commit) {
    binding_pref->commit =
        (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1u};
    fail_marker_commit = false;
  }
  return query_result;
}
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

static void new_binding(const h2_alipay_api_t* api) {
  char code[32];
  size_t size = 0u;
  int sdk_rc = 1;
  code_result = 0;
  query_result = 0;
  query_status = ALIPAY_STATUS_UNKNOWN;
  query_writes = omit_seed = fail_marker_commit = false;
  change_readback = fail_material_delete = fail_material_reset = false;
  assert(api->vtable->get_binding_code(
             api->user, FEATURE_PAYCODE | FEATURE_SQUARE, code, sizeof(code),
             &size, &sdk_rc) == H2_PAL_OK);
}

static void binding_contract(const h2_alipay_api_t* api,
                             h2_app_test_pref_t* pref,
                             const h2_alipay_storage_t* storage) {
  binding_pref = pref;
  binding_storage = storage;
  h2_alipay_trace_reset(api->trace, false);
  h2_alipay_binding_result_t result;
  sdk_bound = false;
  assert(api->vtable->binding_snapshot(api->user, &result) == H2_PAL_OK);
  assert(result.state == H2_ALIPAY_BINDING_UNBOUND);
  new_binding(api);
  unsigned before = query_calls;
  assert(api->vtable->query_binding_result(api->user, &result) == H2_PAL_OK);
  assert(result.state == H2_ALIPAY_BINDING_PENDING && result.sdk_rc == 0 &&
         result.sdk_status == ALIPAY_STATUS_UNKNOWN);
  query_status = ALIPAY_STATUS_START_BINDING;
  assert(api->vtable->query_binding_result(api->user, &result) == H2_PAL_OK);
  assert(result.state == H2_ALIPAY_BINDING_STARTED);
  query_writes = true;
  query_status = ALIPAY_STATUS_BINDING_OK;
  assert(api->vtable->query_binding_result(api->user, &result) == H2_PAL_OK);
  assert(query_calls == before + 3u);
  assert(result.state == H2_ALIPAY_BINDING_COMPLETE &&
         result.tid_length == 8u && result.uid_length == 8u &&
         result.shared_key_length == 32u && result.seed_length == 48u);
  uint8_t data[128];
  uint32_t size = sizeof(data);
  assert(h2_alipay_storage_read(storage, 6u, data, &size) == H2_PAL_OK &&
         size == 8u && memcmp(data, "TEST_TID", 8u) == 0);
  size = sizeof(data);
  assert(h2_alipay_storage_read(storage, 1u, data, &size) == H2_PAL_OK &&
         size == 32u && data[0] == 0x23);
  assert(api->vtable->binding_snapshot(api->user, &result) == H2_PAL_OK &&
         result.state == H2_ALIPAY_BINDING_COMPLETE);
  before = query_calls;
  assert(api->vtable->query_binding_result(api->user, &result) == H2_PAL_OK &&
         query_calls == before);

  new_binding(api);
  query_writes = true;
  query_status = ALIPAY_STATUS_BINDING_FAIL;
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_IO);
  assert(result.sdk_rc == 0 && result.state == H2_ALIPAY_BINDING_FAILED);
  uint8_t digest[32];
  assert(h2_alipay_binding_storage_read(storage, &result, digest) == H2_PAL_OK);
  assert(h2_alipay_binding_storage_verify(storage, digest) ==
         H2_PAL_ERR_NOT_FOUND);

  h2_alipay_trace_reset(api->trace, false);
  new_binding(api);
  query_writes = omit_seed = true;
  query_status = ALIPAY_STATUS_BINDING_OK;
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result.state == H2_ALIPAY_BINDING_NEEDS_VERIFICATION);

  new_binding(api);
  query_writes = true;
  query_status = ALIPAY_STATUS_BINDING_OK;
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1u};
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_WRITE);
  before = query_calls;
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_WRITE);
  assert(query_calls == before && result.storage_rc == H2_PAL_ERR_WRITE);

  new_binding(api);
  query_writes = change_readback = true;
  query_status = ALIPAY_STATUS_BINDING_OK;
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result.state == H2_ALIPAY_BINDING_NEEDS_VERIFICATION);
  before = query_calls;
  assert(api->vtable->query_binding_result(api->user, &result) ==
             H2_PAL_ERR_INVALID_STATE &&
         query_calls == before);
  assert(h2_alipay_binding_storage_read(storage, &result, digest) == H2_PAL_OK);
  assert(h2_alipay_binding_storage_verify(storage, digest) ==
         H2_PAL_ERR_NOT_FOUND);

  for (unsigned operation = 0u; operation < 2u; ++operation) {
    new_binding(api);
    query_writes = true;
    fail_material_delete = operation == 0u;
    fail_material_reset = operation == 1u;
    query_status = ALIPAY_STATUS_BINDING_OK;
    assert(api->vtable->query_binding_result(api->user, &result) ==
           H2_PAL_ERR_WRITE);
    assert(result.state == H2_ALIPAY_BINDING_NEEDS_VERIFICATION);
    before = query_calls;
    assert(api->vtable->query_binding_result(api->user, &result) ==
               H2_PAL_ERR_WRITE &&
           result.storage_rc == H2_PAL_ERR_WRITE && query_calls == before);
  }

  h2_alipay_trace_reset(api->trace, false);
  new_binding(api);
  query_writes = fail_marker_commit = true;
  query_status = ALIPAY_STATUS_BINDING_OK;
  assert(api->vtable->query_binding_result(api->user, &result) ==
         H2_PAL_ERR_WRITE);
  before = query_calls;
  assert(api->vtable->query_binding_result(api->user, &result) == H2_PAL_OK &&
         query_calls == before && result.state == H2_ALIPAY_BINDING_COMPLETE);
  assert(api->vtable->binding_snapshot(api->user, &result) == H2_PAL_OK);
  assert(h2_alipay_storage_write(storage, 7u, (const uint8_t*)"OTHER_UID",
                                 9u) == H2_PAL_OK);
  assert(api->vtable->binding_snapshot(api->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result.state == H2_ALIPAY_BINDING_NEEDS_VERIFICATION);
  assert(alipay_psa_storage_write(1u, data, 31u) == H2_PAL_OK);
  assert(api->vtable->binding_snapshot(api->user, &result) ==
         H2_PAL_ERR_FORMAT);
  pref->read = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  before = query_calls;
  assert(api->vtable->binding_snapshot(api->user, &result) == H2_PAL_ERR_IO);
  assert(result.state == H2_ALIPAY_BINDING_NEEDS_VERIFICATION &&
         result.storage_rc == H2_PAL_ERR_IO && query_calls == before);
  for (size_t index = 0u; index < api->trace->count; ++index)
    assert(api->trace->events[index].captured == 0u);
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
  h2_alipay_storage_t legacy = {.pref = &pref.api, .mem = &mem.api};
  h2_alipay_storage_t sn = {
      .pref = &pref.api,
      .mem = &mem.api,
      .psa_namespace = "alipay_sn_psa",
      .certificate_namespace = "alipay_sn_cert",
  };
  assert(h2_alipay_storage_write(&legacy, 8, (const uint8_t*)"old", 3) == 0);
  static const h2_pal_netif_vtable_t netif_vtable = {.get_status = net_status};
  h2_pal_netif_api_t netif = {NULL, &netif_vtable};
  static const h2_pal_net_vtable_t net_vtable = {.resolve_all = resolve};
  h2_pal_net_api_t net = {NULL, &net_vtable};
  static const h2_pal_crypto_vtable_t crypto_vtable = {.random = entropy};
  h2_pal_result_t random_result = H2_PAL_OK;
  h2_pal_crypto_api_t crypto = {&random_result, &crypto_vtable};
  const h2_pal_log_api_t log = {0};
  h2_alipay_sdk_config_t config = {
      .mem = &mem.api,
      .time = &time.api,
      .log = &log,
      .sync = &sync.api,
      .crypto = &crypto,
      .net = &net,
      .netif = &netif,
      .pref = &pref.api,
      .psa_namespace = sn.psa_namespace,
      .certificate_namespace = sn.certificate_namespace,
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
  assert(alipay_psa_storage_exists(8) == H2_PAL_ERR_NOT_FOUND);
  assert(alipay_psa_storage_write(8, (const uint8_t*)"abc", 3) == 0);
  size = sizeof(bytes);
  assert(alipay_psa_storage_read(8, bytes, &size) == 0 && size == 3);
  assert(memcmp(bytes, "abc", 3) == 0);
  size = sizeof(bytes);
  assert(h2_alipay_storage_read(&sn, 8, bytes, &size) == 0 && size == 3 &&
         memcmp(bytes, "abc", 3) == 0);
  size = sizeof(bytes);
  assert(h2_alipay_storage_read(&legacy, 8, bytes, &size) == 0 && size == 3 &&
         memcmp(bytes, "old", 3) == 0);
  assert(alipay_secure_save_data("certificate", 11) == 0);
  h2_pal_pref_namespace_t* certificate = NULL;
  assert(h2_pal_pref_open(&pref.api, sn.certificate_namespace,
                          H2_PAL_PREF_OPEN_READ_ONLY,
                          &certificate) == H2_PAL_OK);
  void* certificate_data = NULL;
  size_t certificate_size = 0;
  assert(certificate->get_blob(certificate, &mem.api, "certificate",
                               &certificate_data,
                               &certificate_size) == H2_PAL_OK);
  assert(certificate_size == 11 &&
         memcmp(certificate_data, "certificate", 11) == 0);
  h2_pal_mem_free(&mem.api, certificate_data);
  assert(certificate->close(certificate) == H2_PAL_OK);
  for (unsigned index = 0; index < 32u; ++index)
    alipay_log_ext("diagnostic sample %u", index);
  size_t prior_events = api->trace->count;
  errno = EAGAIN;
  alipay_log_ext(
      "A response. (version=\"HTTP/1.1\", status=200, phrase=\"OK\")");
  alipay_log_ext("Result-Status: 1000\r\n");
  alipay_log_ext("result-status: 4001\r\nsecret=not-exported");
  alipay_log_ext("-------- rpc_id: 7, error_code: -12");
  alipay_log_ext("error_code=42");
  alipay_log_ext("RPC network error! error code=23.");
  alipay_log_ext("alipay_send_rpc_simple error! ret = 7.");
  alipay_log_ext("Header:<Result-Status> = <2000>");
  alipay_log_ext("Header:<result-status> = <4001>");
  alipay_log_ext("status:200 res_code:2000");
  assert(errno == EAGAIN);
  assert(api->trace->count == prior_events + 11u);
  const char* operations[] = {
      "rpc.http_status", "rpc.result_status", "rpc.result_status",
      "rpc.error_code",  "rpc.error_code",    "rpc.transport_error",
      "rpc.sdk_error",   "rpc.result_status", "rpc.result_status",
      "rpc.http_status", "rpc.result_status",
  };
  const int results[] = {200, 1000, 4001, -12, 42,  23,
                         7,   2000, 4001, 200, 2000};
  for (size_t index = 0; index < 11u; ++index) {
    const h2_alipay_trace_event_t* event =
        &api->trace->events[prior_events + index];
    assert(strcmp(event->operation, operations[index]) == 0);
    assert(strcmp(event->boundary, "sdk_log_observed") == 0);
    assert(event->result == results[index]);
    assert(event->length == 0u && event->captured == 0u);
    assert(strstr(event->detail, "not-exported") == NULL);
  }
  prior_events = api->trace->count;
  alipay_log_ext("Result-Status: invalid");
  alipay_log_ext("Result-Status: 1000invalid");
  alipay_log_ext("Result-Status: 1000.5");
  alipay_log_ext("Result-Status:\r\n1000");
  alipay_log_ext("Result-Status: 999999999999999999999999999999999999");
  assert(errno == EAGAIN && api->trace->count == prior_events);
  assert(api->trace->dropped == 0);
  binding_contract(api, &pref, &sn);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  /* This bridge deliberately retains its process-lifetime SDK state. */
  h2_app_test_mem_release_all(&mem);
  return 0;
}
