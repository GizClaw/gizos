#include "h2_gizclaw_service_internal.h"

#ifdef H2_GIZCLAW_TESTING
static const h2_gizclaw_async_rpc_ops_t *s_rpc_ops;

void h2_gizclaw_async_rpc_test_set_ops(const h2_gizclaw_async_rpc_ops_t *ops) {
  s_rpc_ops = ops;
}
#endif

int h2_gizclaw_rpc_start_internal(h2_gizclaw_client_t *client,
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

int h2_gizclaw_rpc_start_stream_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_event, void *user,
    h2_gizclaw_rpc_request_t **out_request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->start_stream != NULL)
    return s_rpc_ops->start_stream(client, method, payload, timeout_ms,
                                   on_event, user, out_request);
#endif
  return h2_gizclaw_client_rpc_request_start_stream(
      client, method, payload, timeout_ms, on_event, user, out_request);
}

int h2_gizclaw_rpc_finish_write_internal(h2_gizclaw_rpc_request_t *request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->finish_write != NULL)
    return s_rpc_ops->finish_write(request);
#endif
  return h2_gizclaw_rpc_request_finish_write(request);
}

int h2_gizclaw_rpc_write_internal(h2_gizclaw_rpc_request_t *request,
                                  const uint8_t *data, size_t len) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->write != NULL)
    return s_rpc_ops->write(request, data, len);
#endif
  return h2_gizclaw_rpc_request_write(request, data, len);
}

int h2_gizclaw_rpc_result_internal(h2_gizclaw_rpc_request_t *request,
                                   h2_gizclaw_rpc_response_t *out_response) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->result != NULL)
    return s_rpc_ops->result(request, out_response);
#endif
  return h2_gizclaw_rpc_request_result(request, out_response);
}

bool h2_gizclaw_rpc_set_complete_internal(
    h2_gizclaw_rpc_request_t *request, h2_gizclaw_rpc_complete_fn on_complete,
    void *user) {
#ifdef H2_GIZCLAW_TESTING
  /* Test transports opt in explicitly; legacy fakes remain poll-driven. */
  if (s_rpc_ops != NULL) {
    if (s_rpc_ops->set_complete != NULL) {
      s_rpc_ops->set_complete(request, on_complete, user);
      return true;
    }
    return false;
  }
#endif
  h2_gizclaw_rpc_request_set_complete_handler(request, on_complete, user);
  return true;
}

void h2_gizclaw_rpc_cancel_internal(h2_gizclaw_rpc_request_t *request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->cancel != NULL) {
    s_rpc_ops->cancel(request);
    return;
  }
#endif
  h2_gizclaw_rpc_request_cancel(request);
}

void h2_gizclaw_rpc_destroy_internal(h2_gizclaw_rpc_request_t *request) {
#ifdef H2_GIZCLAW_TESTING
  if (s_rpc_ops != NULL && s_rpc_ops->destroy != NULL) {
    s_rpc_ops->destroy(request);
    return;
  }
#endif
  h2_gizclaw_rpc_request_destroy(request);
}
