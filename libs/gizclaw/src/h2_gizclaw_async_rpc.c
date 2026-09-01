#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_async_rpc {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_rpc_method_t method;
  uint8_t *payload;
  size_t payload_len;
  uint32_t timeout_ms;
  h2_gizclaw_rpc_request_t *request;
  h2_gizclaw_rpc_response_t response;
  h2_gizclaw_async_rpc_completion_fn completion;
  void *completion_user;
  atomic_bool terminal;
};

#ifdef H2_GIZCLAW_TESTING
static const h2_gizclaw_async_rpc_ops_t *s_rpc_ops;

void h2_gizclaw_async_rpc_test_set_ops(
    const h2_gizclaw_async_rpc_ops_t *ops) {
  s_rpc_ops = ops;
}
#endif

static int rpc_request_start(h2_gizclaw_client_t *client,
                             h2_gizclaw_rpc_method_t method,
                             h2_gizclaw_rpc_bytes_t payload,
                             uint32_t timeout_ms,
                             h2_gizclaw_rpc_request_t **out_request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->start != NULL)
    return s_rpc_ops->start(client, method, payload, timeout_ms, out_request);
#endif
  return h2_gizclaw_client_rpc_request_start(client, method, payload,
                                             timeout_ms, out_request);
}

static int rpc_request_result(h2_gizclaw_rpc_request_t *request,
                              h2_gizclaw_rpc_response_t *out_response) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->result != NULL)
    return s_rpc_ops->result(request, out_response);
#endif
  return h2_gizclaw_rpc_request_result(request, out_response);
}

static void rpc_request_cancel(h2_gizclaw_rpc_request_t *request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->cancel != NULL) {
    s_rpc_ops->cancel(request);
    return;
  }
#endif
  h2_gizclaw_rpc_request_cancel(request);
}

static void rpc_request_destroy(h2_gizclaw_rpc_request_t *request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->destroy != NULL) {
    s_rpc_ops->destroy(request);
    return;
  }
#endif
  h2_gizclaw_rpc_request_destroy(request);
}

static h2_pal_result_t async_rpc_poll(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  (void)client;
  h2_gizclaw_async_rpc_t *rpc = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    rpc_request_cancel(rpc->request);
    rpc_request_destroy(rpc->request);
    rpc->request = NULL;
    return H2_PAL_ERR_CLOSED;
  }
  const h2_pal_result_t rc =
      (h2_pal_result_t)rpc_request_result(rpc->request, &rpc->response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  rpc_request_destroy(rpc->request);
  rpc->request = NULL;
  return rc;
}

static h2_pal_result_t async_rpc_start(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_async_rpc_t *rpc = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  const h2_pal_result_t rc =
      (h2_pal_result_t)rpc_request_start(
          client, rpc->method,
          (h2_gizclaw_rpc_bytes_t){.data = rpc->payload,
                                   .len = rpc->payload_len},
          rpc->timeout_ms, &rpc->request);
  if (rc != H2_PAL_OK)
    return rc;
  return async_rpc_poll(user, client, cancel_token);
}

static void async_rpc_complete(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  (void)result;
  h2_gizclaw_async_rpc_t *rpc = user;
  atomic_store_explicit(&rpc->terminal, true, memory_order_release);
  rpc->completion(rpc->completion_user, rpc);
}

h2_pal_result_t h2_gizclaw_service_rpc_call_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t params_payload,
    uint32_t timeout_ms, h2_gizclaw_async_rpc_completion_fn completion,
    void *user, h2_gizclaw_async_rpc_t **out_rpc) {
  if (out_rpc != NULL)
    *out_rpc = NULL;
  if (service == NULL || method <= 0 || timeout_ms == 0u ||
      completion == NULL || out_rpc == NULL ||
      (params_payload.data == NULL && params_payload.len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator =
      service->config.client_config->allocator;
  h2_gizclaw_async_rpc_t *rpc = h2_pal_mem_alloc(allocator, sizeof(*rpc));
  if (rpc == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(rpc, 0, sizeof(*rpc));
  rpc->service = service;
  rpc->allocator = allocator;
  rpc->method = method;
  rpc->timeout_ms = timeout_ms;
  rpc->completion = completion;
  rpc->completion_user = user;
  if (params_payload.len > 0u) {
    rpc->payload = h2_pal_mem_alloc(allocator, params_payload.len);
    if (rpc->payload == NULL) {
      h2_pal_mem_free(allocator, rpc);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(rpc->payload, params_payload.data, params_payload.len);
    rpc->payload_len = params_payload.len;
  }
  const h2_pal_result_t rc = h2_gizclaw_service_submit_async_internal(
      service, identity, async_rpc_start, async_rpc_poll, async_rpc_complete,
      rpc, &rpc->operation);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, rpc->payload);
    h2_pal_mem_free(allocator, rpc);
    return rc;
  }
  *out_rpc = rpc;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_async_rpc_cancel(h2_gizclaw_async_rpc_t *rpc) {
  if (rpc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(rpc->operation);
}

h2_pal_result_t h2_gizclaw_async_rpc_wait(h2_gizclaw_async_rpc_t *rpc,
                                          uint32_t timeout_ms) {
  if (rpc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_wait(rpc->operation, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_async_rpc_operation_result(const h2_gizclaw_async_rpc_t *rpc) {
  if (rpc == NULL ||
      !atomic_load_explicit(&rpc->terminal, memory_order_acquire)) {
    return NULL;
  }
  return h2_gizclaw_operation_result(rpc->operation);
}

const h2_gizclaw_rpc_response_t *
h2_gizclaw_async_rpc_response(const h2_gizclaw_async_rpc_t *rpc) {
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_async_rpc_operation_result(rpc);
  if (result == NULL || result->result != H2_PAL_OK)
    return NULL;
  return &rpc->response;
}

void h2_gizclaw_async_rpc_release(h2_gizclaw_async_rpc_t *rpc) {
  if (rpc == NULL ||
      !atomic_load_explicit(&rpc->terminal, memory_order_acquire)) {
    return;
  }
  h2_gizclaw_operation_release(rpc->operation);
  h2_pal_mem_free(rpc->allocator, rpc->response.result_payload);
  h2_pal_mem_free(rpc->allocator, rpc->response.error_message);
  h2_pal_mem_free(rpc->allocator, rpc->payload);
  h2_pal_mem_free(rpc->allocator, rpc);
}
