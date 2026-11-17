#include "h2_alipay_e2e_independent.h"

#include "h2_alipay_context.h"
#include "h2_alipay_payment_bridge.h"
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

static int binding_status(void* user, bool* bound) {
  if (bound == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_binding_result_t result;
  int rc = binding_snapshot(user, &result);
  *bound = rc == H2_PAL_OK && result.state == H2_ALIPAY_BINDING_COMPLETE;
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

h2_pal_result_t h2_alipay_e2e_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync) {
  return h2_alipay_e2e_independent_configure_with_storage(
      config, sync, H2_ALIPAY_STORAGE_PERSISTENT);
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
      .binding_status = binding_status,
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

static int payment_enter(void* user) {
  (void)user;
  return s_mem != NULL && s_initialized && s_init_rc == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static int payment_leave(void* user, int result) {
  (void)user;
  return result;
}

static void payment_record(void* user, const char* operation, int result,
                           uint32_t item) {
  (void)user;
  record(operation, result, item);
}

const h2_alipay_payment_api_t* h2_alipay_e2e_independent_payment_api(void) {
  static const h2_alipay_payment_bridge_t bridge = {
      .enter = payment_enter, .leave = payment_leave, .record = payment_record};
  static h2_alipay_payment_api_t api = {.user = (void*)&bridge};
  api.vtable = h2_alipay_payment_bridge_vtable();
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
