#include "h2_alipay_e2e_independent.h"

#include "h2_alipay_independent.h"
#include "alipay_bind.h"
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

h2_pal_result_t h2_alipay_e2e_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync) {
  if (s_mem != NULL) return H2_PAL_ERR_INVALID_STATE;
  if (config == NULL || config->mem == NULL || config->http == NULL)
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
  static const h2_alipay_vtable_t vtable = {.pre_init = pre_init,
                                            .get_binding_code = get_code};
  s_api = (h2_alipay_api_t){.vtable = &vtable,
                            .source = H2_ALIPAY_SOURCE_INDEPENDENT,
                            .trace = trace};
  s_mem = config->mem;
  s_time = config->time;
  s_http = config->http;
  return H2_PAL_OK;
}

const h2_alipay_api_t* h2_alipay_e2e_independent_api(void) {
  return s_mem != NULL ? &s_api : NULL;
}

h2_pal_result_t h2_alipay_e2e_independent_release(void) {
  if (s_mem == NULL) return H2_PAL_OK;
  int rc = h2_alipay_independent_release();
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
