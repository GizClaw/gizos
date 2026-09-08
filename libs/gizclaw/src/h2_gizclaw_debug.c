#include "h2_gizclaw_debug.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/system.pb.h"
#include "payload/workspace.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

static const char debug_set_tag;
static const char debug_get_tag;

static bool encode_mode(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const h2_gizclaw_str_t *mode = *arg;
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)mode->data, mode->len);
}

static bool decode_mode(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  h2_gizclaw_debug_state_t *state = *arg;
  const size_t len = stream->bytes_left;
  if (len == 0u || len >= sizeof(state->mode))
    return false;
  if (!pb_read(stream, (pb_byte_t *)state->mode, len) ||
      memchr(state->mode, '\0', len) != NULL)
    return false;
  state->mode[len] = '\0';
  return true;
}

h2_pal_result_t h2_gizclaw_req_create_debug_set(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t mode,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (mode.data == NULL || mode.len == 0u || mode.len >= 64u ||
      memchr(mode.data, '\0', mode.len) != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPutRuntimeRequest message =
      gizclaw_rpc_v1_ServerPutRuntimeRequest_init_zero;
  message.debug_mode.funcs.encode = encode_mode;
  message.debug_mode.arg = &mode;
  uint8_t payload[66];
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&stream, gizclaw_rpc_v1_ServerPutRuntimeRequest_fields,
                 &message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_RUNTIME_PUT, &debug_set_tag,
      (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_debug_set(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state) {
  if (out_state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &debug_set_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_debug_state_t decoded = {0};
  gizclaw_rpc_v1_ServerPutRuntimeResponse message =
      gizclaw_rpc_v1_ServerPutRuntimeResponse_init_zero;
  message.debug_mode.funcs.decode = decode_mode;
  message.debug_mode.arg = &decoded;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPutRuntimeResponse_fields,
                 &message) || decoded.mode[0] == '\0')
    return H2_PAL_ERR_FORMAT;
  *out_state = decoded;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_create_debug_get(h2_gizclaw_service_t *service,
                                                uint64_t identity,
                                                uint32_t timeout_ms,
                                                h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  /* ServerGetRuntimeRequest has no fields: an empty payload is the
   * canonical encoding. */
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_RUNTIME_GET, &debug_get_tag,
      (h2_gizclaw_rpc_bytes_t){NULL, 0u}, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_debug_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state) {
  if (out_state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &debug_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_debug_state_t decoded = {0};
  gizclaw_rpc_v1_ServerGetRuntimeResponse message =
      gizclaw_rpc_v1_ServerGetRuntimeResponse_init_zero;
  message.value.debug_mode.funcs.decode = decode_mode;
  message.value.debug_mode.arg = &decoded;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerGetRuntimeResponse_fields,
                 &message) || !message.has_value)
    return H2_PAL_ERR_FORMAT;
  *out_state = decoded;
  return H2_PAL_OK;
}

/* ---- Library-owned snapshot ------------------------------------------- */

static void debug_lock(h2_gizclaw_service_t *service) {
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
}

static void debug_unlock(h2_gizclaw_service_t *service) {
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

/* Fold the library's in-flight request into the snapshot once it reached a
 * terminal state. No completion hook is used: the App reads the snapshot
 * from its own loop, so the request is inspected (never waited for) here,
 * which also survives a full dispatch queue or a stopped Service.
 *
 * Terminal detection uses the parser, not req_wait(0): the parser answers
 * INVALID_STATE only while the request is still in flight and otherwise
 * returns the terminal result, so a request that ended with an execution
 * TIMEOUT is folded (as a failure) instead of staying busy forever.
 *
 * The parser only reads request-local atomics and never takes the Service
 * lock, so the whole inspection runs under the lock; the request is detached
 * from the snapshot before the lock is dropped, which is the same protocol
 * stop uses, so exactly one side ever releases the reference. */
static void debug_fold(h2_gizclaw_service_t *service) {
  debug_lock(service);
  h2_gizclaw_req_t *request = service->debug.request;
  if (request == NULL) {
    debug_unlock(service);
    return;
  }
  h2_gizclaw_debug_state_t state;
  const h2_pal_result_t rc =
      service->debug.request_is_set
          ? h2_gizclaw_resp_parse_debug_set(request, &state)
          : h2_gizclaw_resp_parse_debug_get(request, &state);
  if (rc == H2_PAL_ERR_INVALID_STATE) {
    /* Still in flight. */
    debug_unlock(service);
    return;
  }
  service->debug.request = NULL;
  if (rc == H2_PAL_OK) {
    service->debug.known = true;
    memcpy(service->debug.mode, state.mode, sizeof(service->debug.mode));
  }
  service->debug.last_result = rc;
  if (++service->debug.revision == 0u)
    ++service->debug.revision;
  debug_unlock(service);
  h2_gizclaw_req_release(request);
}

static void (*s_debug_publish_hook)(void *user);
static void *s_debug_publish_hook_user;

void h2_gizclaw_debug_test_set_publish_hook(void (*hook)(void *user),
                                            void *user) {
  s_debug_publish_hook = hook;
  s_debug_publish_hook_user = user;
}

static h2_pal_result_t debug_start(h2_gizclaw_service_t *service, bool is_set,
                                   h2_gizclaw_str_t mode, uint32_t timeout_ms) {
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  debug_fold(service);
  debug_lock(service);
  if (service->debug.request != NULL || service->debug.starting) {
    debug_unlock(service);
    return H2_PAL_ERR_BUSY;
  }
  if (service->stopping || service->stopped) {
    debug_unlock(service);
    return H2_PAL_ERR_CLOSED;
  }
  service->debug.starting = true;
  debug_unlock(service);

  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      is_set ? h2_gizclaw_req_create_debug_set(service, 0u, mode, timeout_ms,
                                               &request)
             : h2_gizclaw_req_create_debug_get(service, 0u, timeout_ms,
                                               &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (s_debug_publish_hook != NULL)
    s_debug_publish_hook(s_debug_publish_hook_user);
  debug_lock(service);
  service->debug.starting = false;
  if (rc == H2_PAL_OK && (service->stopping || service->stopped)) {
    /* Stop ran between submission and publication and could not see the
     * request; it must not be published after stop, so finish it here. */
    rc = H2_PAL_ERR_CLOSED;
  }
  if (rc == H2_PAL_OK) {
    service->debug.request = request;
    service->debug.request_is_set = is_set;
  }
  debug_unlock(service);
  if (rc != H2_PAL_OK && request != NULL) {
    (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_debug_snapshot(h2_gizclaw_service_t *service,
                                          h2_gizclaw_debug_snapshot_t *out) {
  if (service == NULL || out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  debug_fold(service);
  debug_lock(service);
  out->known = service->debug.known;
  memcpy(out->mode, service->debug.mode, sizeof(out->mode));
  out->busy = service->debug.request != NULL || service->debug.starting;
  out->last_result = service->debug.last_result;
  out->revision = service->debug.revision;
  debug_unlock(service);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_debug_refresh(h2_gizclaw_service_t *service,
                                         uint32_t timeout_ms) {
  return debug_start(service, false, (h2_gizclaw_str_t){NULL, 0u}, timeout_ms);
}

h2_pal_result_t h2_gizclaw_debug_set_mode(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t mode,
                                          uint32_t timeout_ms) {
  return debug_start(service, true, mode, timeout_ms);
}

void h2_gizclaw_debug_stop_internal(h2_gizclaw_service_t *service) {
  if (service == NULL)
    return;
  debug_lock(service);
  h2_gizclaw_req_t *request = service->debug.request;
  service->debug.request = NULL;
  debug_unlock(service);
  if (request != NULL) {
    (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  }
}
