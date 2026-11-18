#include "h2_alipay_e2e_independent.h"

#include "h2_alipay_context.h"
#include "h2_alipay_api.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const h2_pal_mem_api_t* s_mem;
static const h2_pal_time_api_t* s_time;
static const h2_pal_http_api_t* s_http;
static h2_pal_http_api_t s_observed_http;
static h2_alipay_api_t s_api;
static bool s_initialized;
static int s_init_rc;

static void record(const char* operation, int result, uint32_t item) {
  h2_alipay_trace_event_t event = {
      .call_id = ++s_api.trace->next_call,
      .result = result,
      .item = item,
  };
  (void)h2_pal_time_get_monotonic_ms(s_time, &event.mono_ms);
  event.utc_valid = h2_pal_time_get_wall_ms(s_time, &event.utc_ms) == H2_PAL_OK;
  (void)snprintf(event.operation, sizeof(event.operation), "%s", operation);
  (void)snprintf(event.boundary, sizeof(event.boundary), "independent");
  h2_alipay_trace_push(s_api.trace, &event, NULL, 0u);
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  (void)user;
  int rc = h2_pal_http_request(s_http, request, response);
  record("independent.http.return", rc, (uint32_t)response->status_code);
  return rc;
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  (void)user;
  h2_pal_http_response_free(s_http, response);
}

static h2_pal_result_t pre_init(void* user, int* implementation_rc,
                                bool* first) {
  (void)user;
  if (implementation_rc == NULL || first == NULL) return H2_PAL_ERR_INVALID_ARG;
  *first = !s_initialized;
  if (!s_initialized) {
    s_initialized = true;
    s_init_rc = alipay_pre_init();
  }
  *implementation_rc = s_init_rc;
  record(*first ? "independent.pre_init" : "independent.pre_init.reused",
         s_init_rc, 0u);
  return s_init_rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t get_code(void* user, uint32_t feature, char* code,
                                size_t capacity, size_t* length,
                                int* implementation_rc) {
  (void)user;
  if (code == NULL || capacity == 0u || capacity > INT_MAX || length == NULL ||
      implementation_rc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  code[0] = '\0';
  *length = 0u;
  if (!s_initialized || s_init_rc != 0) {
    *implementation_rc = H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_ERR_INVALID_STATE;
  }
  int size = (int)capacity;
  *implementation_rc = alipay_get_binding_code(feature, code, &size);
  int rc = *implementation_rc == ALIPAY_RV_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK && (size <= 0 || (size_t)size >= capacity ||
                          memchr(code, '\0', (size_t)size) != NULL))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK)
    *length = (size_t)size;
  else
    memset(code, 0, capacity);
  record("independent.binding_code.return", rc, (uint32_t)*length);
  return rc;
}

static void binding_result(const h2_alipay_client_binding_t* binding,
                           h2_alipay_binding_result_t* out) {
  out->tid_length = binding->tid_length;
  out->uid_length = binding->uid_length;
  out->shared_key_length = binding->shared_key_length;
  out->seed_length = binding->seed_length;
  out->storage_rc = binding->storage_rc;
  switch (binding->state) {
    case H2_ALIPAY_CLIENT_UNBOUND:
      out->state = H2_ALIPAY_BINDING_UNBOUND;
      break;
    case H2_ALIPAY_CLIENT_PENDING:
      out->state = H2_ALIPAY_BINDING_PENDING;
      break;
    case H2_ALIPAY_CLIENT_COMPLETE:
      out->state = H2_ALIPAY_BINDING_COMPLETE;
      break;
    case H2_ALIPAY_CLIENT_FAILED:
      out->state = H2_ALIPAY_BINDING_FAILED;
      break;
    default:
      out->state = H2_ALIPAY_BINDING_NEEDS_VERIFICATION;
      break;
  }
}

static int binding_snapshot(void* user, h2_alipay_binding_result_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_alipay_client_binding_t binding;
  int rc = h2_alipay_independent_binding_snapshot(&binding);
  binding_result(&binding, out);
  out->sdk_rc = rc == H2_PAL_OK ? ALIPAY_RV_OK : ALIPAY_RV_HAL_ERROR;
  out->sdk_status = out->state == H2_ALIPAY_BINDING_COMPLETE
                        ? ALIPAY_STATUS_BINDING_OK
                        : ALIPAY_STATUS_UNBINDED;
  record("independent.binding_snapshot", rc, (uint32_t)out->state);
  return rc;
}

static int query_binding_result(void* user, h2_alipay_binding_result_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  binding_status_e status = ALIPAY_STATUS_UNKNOWN;
  out->sdk_rc = alipay_query_binding_result(&status);
  out->sdk_status = status;
  if (out->sdk_rc != ALIPAY_RV_OK) {
    out->state = H2_ALIPAY_BINDING_FAILED;
    return H2_PAL_ERR_IO;
  }
  h2_alipay_independent_binding_result_t diagnostics;
  int rc = h2_alipay_independent_last_binding_result(&diagnostics);
  if (rc != H2_PAL_OK) return rc;
  binding_result(&diagnostics.binding, out);
  record("independent.binding_query", diagnostics.result, (uint32_t)status);
  /* The delivered SDK wrapper reports transient network errors as UNKNOWN;
   * preserve that polling behavior while retaining the actual PAL diagnostic.
   */
  return out->state == H2_ALIPAY_BINDING_PENDING ? H2_PAL_OK
                                                 : diagnostics.result;
}

h2_pal_result_t h2_alipay_e2e_independent_configure_with_storage(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync,
    h2_alipay_storage_scope_t scope) {
  if (s_mem != NULL) return H2_PAL_ERR_INVALID_STATE;
  if (config == NULL || config->mem == NULL || config->http == NULL ||
      (scope != H2_ALIPAY_STORAGE_PERSISTENT &&
       scope != H2_ALIPAY_STORAGE_SESSION))
    return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_trace_t* trace = h2_pal_mem_alloc(config->mem, sizeof(*trace));
  if (trace == NULL) return H2_PAL_ERR_NO_MEMORY;
  static const h2_pal_http_vtable_t http = {.request = request,
                                            .response_free = response_free};
  s_observed_http = (h2_pal_http_api_t){.vtable = &http};
  h2_alipay_client_config_t observed = *config;
  observed.http = &s_observed_http;
  int rc = h2_alipay_independent_configure(&observed, sync);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->mem, trace);
    return rc;
  }
  h2_alipay_trace_reset(trace, false);
  static const h2_alipay_vtable_t vtable = {
      .pre_init = pre_init,
      .get_binding_code = get_code,
      .binding_snapshot = binding_snapshot,
      .query_binding_result = query_binding_result};
  s_api = (h2_alipay_api_t){.vtable = &vtable,
                            .source = H2_ALIPAY_SOURCE_INDEPENDENT,
                            .trace = trace,
                            .storage_scope = scope};
  s_mem = config->mem;
  s_time = config->time;
  s_http = config->http;
  return H2_PAL_OK;
}

const h2_alipay_api_t* h2_alipay_e2e_independent_api(void) {
  return s_mem != NULL ? &s_api : NULL;
}

static h2_alipay_tap_transport_t s_transport;
static h2_alipay_payment_result_t s_tap;
static bool s_tap_active;

static void payment_wipe(void* data, size_t size) {
  volatile uint8_t* p = data;
  while (size-- != 0u) *p++ = 0u;
}

static void payment_state(h2_alipay_payment_result_t* out) {
  out->state = H2_ALIPAY_PAYMENT_UNKNOWN;
  if (out->sdk_rc != ALIPAY_RV_OK) return;
  switch (out->sdk_status) {
    case ALIPAY_PAYMENT_STATUS_SUCCESS:
      out->state = H2_ALIPAY_PAYMENT_PAID;
      break;
    case ALIPAY_PAYMENT_STATUS_NOTHING:
    case ALIPAY_PAYMENT_STATUS_PROCESSING:
      out->state = H2_ALIPAY_PAYMENT_PENDING;
      break;
    case ALIPAY_PAYMENT_STATUS_FAIL:
      out->state = H2_ALIPAY_PAYMENT_REJECTED;
      break;
    case ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER:
      out->state = H2_ALIPAY_PAYMENT_UNBOUND;
      break;
    case ALIPAY_PAYMENT_STATUS_DISABLED_BY_USER:
      out->state = H2_ALIPAY_PAYMENT_DISABLED;
      break;
    default:
      break;
  }
}

static int payment_enter(void) {
  return s_mem != NULL && s_initialized && s_init_rc == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static int payment_get_code(void* user, uint8_t* code, uint32_t* length,
                            int* sdk_rc) {
  (void)user;
  if (code == NULL || length == NULL || sdk_rc == NULL || *length < 20u)
    return H2_PAL_ERR_INVALID_ARG;
  uint32_t capacity = *length;
  *sdk_rc = INT_MIN;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) {
    payment_wipe(code, capacity);
    *length = 0u;
    return rc;
  }
  *sdk_rc = alipay_get_paycode(code, length);
  if (*sdk_rc != ALIPAY_RV_OK) {
    payment_wipe(code, capacity);
    *length = 0u;
  } else if ((*length != 18u && *length != 19u) || *length >= capacity)
    rc = H2_PAL_ERR_FORMAT;
  else {
    for (uint32_t i = 0u; i < *length; ++i)
      if (code[i] < '0' || code[i] > '9') rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK && *sdk_rc == 0)
    code[*length] = '\0';
  else {
    payment_wipe(code, capacity);
    *length = 0u;
  }
  record("alipay_get_paycode.return", *sdk_rc, *length);
  return rc;
}

static int payment_query_code(void* user, const char* code,
                              h2_alipay_payment_result_t* out) {
  (void)user;
  if (code == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_payment_result_t){
      .sdk_rc = INT_MIN, .sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  size_t length = 0u;
  while (length < 20u && code[length] >= '0' && code[length] <= '9') ++length;
  if ((length != 18u && length != 19u) || code[length] != '\0')
    return H2_PAL_ERR_INVALID_ARG;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  char copy[20], ext[256];
  memcpy(copy, code, length + 1u);
  uint32_t ext_length = sizeof(ext);
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  out->sdk_rc = alipay_query_order_result_string(copy, &status, out->amount,
                                                 ext, &ext_length);
  out->sdk_status = (int)status;
  out->amount[sizeof(out->amount) - 1u] = '\0';
  payment_state(out);
  if (out->state != H2_ALIPAY_PAYMENT_PAID)
    payment_wipe(out->amount, sizeof(out->amount));
  record("alipay_query_order.return", out->sdk_rc, (uint32_t)status);
  payment_wipe(copy, sizeof(copy));
  payment_wipe(ext, sizeof(ext));
  return H2_PAL_OK;
}

static int payment_write_reply(const uint8_t* data, uint32_t length) {
  if (!s_tap_active) return H2_PAL_ERR_CLOSED;
  if (s_transport.write_reply == NULL) return H2_PAL_ERR_UNSUPPORTED;
  s_tap.write_attempted = true;
  s_tap.nfc_write_rc =
      s_transport.write_reply != NULL
          ? s_transport.write_reply(s_transport.user, data, length)
          : H2_PAL_ERR_UNSUPPORTED;
  s_tap.response_sent |= s_tap.nfc_write_rc == H2_PAL_OK;
  s_tap.transport_rc = s_tap.nfc_write_rc;
  return s_tap.nfc_write_rc;
}

static int payment_processed(alipay_payment_status_t* status) {
  if (status == NULL || !s_tap_active) return H2_PAL_ERR_INVALID_STATE;
  s_tap.processing_status = (int)status->status;
  s_tap.processing_error = (int)status->code;
  /* Internal callback success is not a financial success result. */
  return H2_PAL_OK;
}

static int payment_start_tap(void* user,
                             const h2_alipay_tap_transport_t* transport,
                             h2_alipay_payment_result_t* out) {
  (void)user;
  if (transport == NULL || transport->write_reply == NULL || out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  if (s_tap_active) return H2_PAL_ERR_INVALID_STATE;
  s_transport = *transport;
  s_tap =
      (h2_alipay_payment_result_t){.sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  s_tap_active = true;
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = payment_write_reply,
      .notify_payment_result_handle = payment_processed,
  };
  alipay_nfc_init_callbacks(callbacks);
  *out = s_tap;
  return H2_PAL_OK;
}

static int payment_receive_ndef(void* user, const uint8_t* data, size_t length,
                                h2_alipay_payment_result_t* out) {
  (void)user;
  if (data == NULL || length == 0u || length > UINT32_MAX || out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || s_tap.write_attempted || s_tap.response_sent)
    return H2_PAL_ERR_INVALID_STATE;
  s_tap =
      (h2_alipay_payment_result_t){.sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  s_tap.sdk_rc = alipay_nfc_recv_data_handle(data, (uint32_t)length);
  s_tap.state = s_tap.response_sent ? H2_ALIPAY_PAYMENT_PENDING
                                    : H2_ALIPAY_PAYMENT_UNKNOWN;
  *out = s_tap;
  record("alipay_nfc_receive.return", out->sdk_rc, (uint32_t)out->transport_rc);
  if (out->response_sent) return H2_PAL_OK;
  return out->transport_rc != H2_PAL_OK ? out->transport_rc : H2_PAL_ERR_IO;
}

static int payment_query_tap(void* user, h2_alipay_payment_result_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_payment_result_t){
      .sdk_rc = INT_MIN, .sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || !s_tap.response_sent) return H2_PAL_ERR_INVALID_STATE;
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  char ext[256];
  uint32_t ext_length = sizeof(ext);
  uint64_t started = 0u, ended = 0u;
  bool timed = h2_pal_time_get_monotonic_ms(s_time, &started) == H2_PAL_OK;
  out->sdk_rc =
      alipay_query_last_nfc_pay_result(&status, out->amount, ext, &ext_length);
  if (timed && h2_pal_time_get_monotonic_ms(s_time, &ended) == H2_PAL_OK &&
      ended >= started)
    out->query_elapsed_ms = ended - started;
  h2_alipay_client_payment_t diagnostics;
  if (h2_alipay_independent_last_payment_result(&diagnostics) == H2_PAL_OK) {
    out->query_diagnostics_valid = true;
    out->query_attempted = diagnostics.query.attempted;
    out->query_token_present = diagnostics.query.nfc_token_present;
    out->query_code_length = diagnostics.query.code_length;
    out->query_tid_length = diagnostics.query.tid_length;
    out->query_uid_length = diagnostics.query.uid_length;
    out->query_result = diagnostics.result;
    out->query_request_result = diagnostics.query.request_result;
    out->query_http_status = diagnostics.query.http_status;
    out->query_rpc_status = diagnostics.query.rpc_status;
    out->query_ack_result = diagnostics.ack_result;
    out->query_action = (unsigned)diagnostics.query.action;
    out->query_body_length = diagnostics.query.body_length;
    out->query_action_length = diagnostics.query.action_length;
    out->query_action_hash_valid = diagnostics.query.action_hash_valid;
    out->query_action_identifier = diagnostics.query.action_identifier;
    memcpy(out->query_action_sha256, diagnostics.query.action_sha256,
           sizeof(out->query_action_sha256));
    out->query_response_fields = diagnostics.query.response_fields;
    out->query_business_type = diagnostics.query.business_type;
    out->query_business_length = diagnostics.query.business_length;
    out->query_response_code_valid = diagnostics.query.response_code_valid;
    out->query_response_code = diagnostics.query.response_code;
    payment_wipe(&diagnostics, sizeof(diagnostics));
  }
  out->sdk_status = (int)status;
  out->response_sent = true;
  out->amount[sizeof(out->amount) - 1u] = '\0';
  payment_state(out);
  if (out->state != H2_ALIPAY_PAYMENT_PAID)
    payment_wipe(out->amount, sizeof(out->amount));
  payment_wipe(ext, sizeof(ext));
  return H2_PAL_OK;
}

static int payment_stop_tap(void* user) {
  (void)user;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  s_tap_active = false;
  payment_wipe(&s_transport, sizeof(s_transport));
  payment_wipe(&s_tap, sizeof(s_tap));
  return H2_PAL_OK;
}

static int payment_unbind(void* user, int* sdk_rc) {
  (void)user;
  if (sdk_rc == NULL) return H2_PAL_ERR_INVALID_ARG;
  *sdk_rc = INT_MIN;
  int rc = payment_enter();
  if (rc != H2_PAL_OK) return rc;
  if (s_tap_active) return H2_PAL_ERR_BUSY;
  *sdk_rc = alipay_unbinding();
  record("alipay_unbinding.return", *sdk_rc, 0u);
  if (*sdk_rc != ALIPAY_RV_OK) return H2_PAL_ERR_IO;
  h2_alipay_client_binding_t binding;
  rc = h2_alipay_independent_binding_snapshot(&binding);
  if (rc == H2_PAL_OK && binding.state != H2_ALIPAY_CLIENT_UNBOUND)
    rc = H2_PAL_ERR_INVALID_STATE;
  payment_wipe(&binding, sizeof(binding));
  return rc;
}

const h2_alipay_payment_api_t* h2_alipay_e2e_independent_payment_api(void) {
  static const h2_alipay_payment_vtable_t vtable = {
      .get_code = payment_get_code,
      .query_code = payment_query_code,
      .start_tap = payment_start_tap,
      .receive_ndef = payment_receive_ndef,
      .query_tap = payment_query_tap,
      .stop_tap = payment_stop_tap,
      .unbind = payment_unbind,
  };
  static const h2_alipay_payment_api_t api = {.vtable = &vtable};
  return &api;
}

h2_pal_result_t h2_alipay_e2e_independent_release(void) {
  if (s_mem == NULL) return H2_PAL_OK;
  int rc = H2_PAL_OK;
  if (s_initialized && s_init_rc == 0) {
    const h2_alipay_payment_api_t* payment =
        h2_alipay_e2e_independent_payment_api();
    rc = payment->vtable->stop_tap(payment->user);
  }
  if (rc == H2_PAL_OK) rc = h2_alipay_independent_release();
  if (rc != H2_PAL_OK) return rc;
  h2_pal_mem_free(s_mem, s_api.trace);
  memset(&s_api, 0, sizeof(s_api));
  s_mem = NULL;
  s_time = NULL;
  s_http = NULL;
  s_initialized = false;
  s_init_rc = 0;
  return H2_PAL_OK;
}
