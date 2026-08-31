#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_peer_delete_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_peer_delete_completion_fn completion;
  void *completion_user;
  atomic_bool terminal;
};

static h2_pal_result_t peer_delete_run(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  (void)user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  return (h2_pal_result_t)h2_gizclaw_client_delete_peer(client);
}

static void peer_delete_complete(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_peer_delete_request_t *request = user;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, result);
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
  const h2_pal_result_t rc = h2_gizclaw_service_submit(
      service, identity, peer_delete_run, peer_delete_complete, request,
      &request->operation);
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
  return h2_gizclaw_operation_cancel(request->operation);
}

void h2_gizclaw_peer_delete_request_release(
    h2_gizclaw_peer_delete_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_pal_mem_free(request->allocator, request);
}
