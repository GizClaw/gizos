#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_peer_delete_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_async_rpc_t *rpc;
  h2_gizclaw_peer_delete_completion_fn completion;
  void *completion_user;
  h2_gizclaw_operation_result_t operation_result;
  atomic_bool terminal;
};

static void peer_delete_complete(void *user, h2_gizclaw_async_rpc_t *rpc) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_rpc_operation_result(rpc);
  const h2_gizclaw_rpc_response_t *response =
      h2_gizclaw_async_rpc_response(rpc);
  h2_gizclaw_peer_delete_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK && response->has_error)
    result.result = H2_PAL_ERR_IO;
  request->operation_result = result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

h2_pal_result_t h2_gizclaw_service_delete_peer_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_peer_delete_completion_fn completion, void *user,
    h2_gizclaw_peer_delete_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_peer_delete_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->completion = completion;
  request->completion_user = user;
  const h2_pal_result_t rc = h2_gizclaw_service_rpc_call_async(
      service, identity, H2_GIZCLAW_RPC_SERVER_PEER_DELETE,
      (h2_gizclaw_rpc_bytes_t){0}, 5000u,
      peer_delete_complete, request, &request->rpc);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_peer_delete_request_cancel(
    h2_gizclaw_peer_delete_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_async_rpc_cancel(request->rpc);
}

h2_pal_result_t h2_gizclaw_peer_delete_request_wait(
    h2_gizclaw_peer_delete_request_t *request, uint32_t timeout_ms) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_async_rpc_wait(request->rpc, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_peer_delete_request_operation_result(
    const h2_gizclaw_peer_delete_request_t *request) {
  return request != NULL &&
                 atomic_load_explicit(&request->terminal, memory_order_acquire)
             ? &request->operation_result
             : NULL;
}

void h2_gizclaw_peer_delete_request_release(
    h2_gizclaw_peer_delete_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_async_rpc_release(request->rpc);
  h2_pal_mem_free(request->allocator, request);
}
