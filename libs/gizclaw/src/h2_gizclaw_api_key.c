#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include <string.h>
static const char create_tag, revoke_tag;
static bool copy_text(char *out, size_t size, h2_gizclaw_str_t text) {
  if (!text.data || !text.len || text.len >= size ||
      memchr(text.data, 0, text.len))
    return false;
  memcpy(out, text.data, text.len);
  out[text.len] = 0;
  return true;
}
static h2_pal_result_t
create_request(h2_gizclaw_service_t *service, uint64_t identity, int method,
               const void *tag, const pb_msgdesc_t *fields, const void *message,
               uint32_t timeout_ms, h2_gizclaw_req_t **out) {
  uint8_t payload[128];
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&stream, fields, message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, method, tag,
      (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms, out);
}
h2_pal_result_t h2_gizclaw_req_create_api_key_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t display_name, bool manage_api_keys, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request)
    *out_request = NULL;
  gizclaw_rpc_v1_APIKeyCreateRequest message = {.manage_api_keys =
                                                    manage_api_keys};
  if (!copy_text(message.display_name, sizeof(message.display_name),
                 display_name))
    return H2_PAL_ERR_INVALID_ARG;
  return create_request(service, identity, 96, &create_tag,
                        gizclaw_rpc_v1_APIKeyCreateRequest_fields, &message,
                        timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_create(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_api_key_t *out_key) {
  if (!out_key)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_key, 0, sizeof(*out_key));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_APIKeyCreateResponse message = {0};
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_APIKeyCreateResponse_fields,
                 &message) ||
      !message.has_value || !message.value.name[0] || !message.api_key[0])
    return H2_PAL_ERR_FORMAT;
  memcpy(out_key->name, message.value.name, sizeof(out_key->name));
  memcpy(out_key->secret, message.api_key, sizeof(out_key->secret));
  memset(&message, 0, sizeof(message));
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_api_key_revoke(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request)
    *out_request = NULL;
  gizclaw_rpc_v1_APIKeyRevokeRequest message = {0};
  if (!copy_text(message.name, sizeof(message.name), name))
    return H2_PAL_ERR_INVALID_ARG;
  return create_request(service, identity, 98, &revoke_tag,
                        gizclaw_rpc_v1_APIKeyRevokeRequest_fields, &message,
                        timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_revoke(const h2_gizclaw_req_t *request) {
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &revoke_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_APIKeyRevokeResponse message = {0};
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  return pb_decode(&stream, gizclaw_rpc_v1_APIKeyRevokeResponse_fields,
                   &message)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}
static int wait_request(h2_gizclaw_req_t *request) {
  int rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  return rc == H2_PAL_OK
             ? h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER)
             : rc;
}
h2_pal_result_t h2_gizclaw_rpc_api_key_create(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t display_name,
                                              bool manage_api_keys,
                                              uint32_t timeout_ms,
                                              h2_gizclaw_api_key_t *out_key) {
  if (!out_key)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_key, 0, sizeof(*out_key));
  h2_gizclaw_req_t *request = NULL;
  int rc = h2_gizclaw_req_create_api_key_create(
      service, 0, display_name, manage_api_keys, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = wait_request(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_api_key_create(request, out_key);
  h2_gizclaw_req_release(request);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_api_key_revoke(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t name,
                                              uint32_t timeout_ms) {
  h2_gizclaw_req_t *request = NULL;
  int rc = h2_gizclaw_req_create_api_key_revoke(service, 0, name, timeout_ms,
                                                &request);
  if (rc == H2_PAL_OK)
    rc = wait_request(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_api_key_revoke(request);
  h2_gizclaw_req_release(request);
  return rc;
}

/* The owner submits and dispatches; readers may expire a generation under the
 * same mutex. Each accepted request owns its context until its hook returns. */
typedef struct api_key_pending {
  h2_gizclaw_api_key_state_t *state;
  h2_gizclaw_req_t *request;
  uint64_t generation;
  bool revoke;
  bool orphan;
  /* Revoked key name, used to reconcile a detached revoke with the snapshot. */
  char name[sizeof(((h2_gizclaw_api_key_t *)0)->name)];
} api_key_pending_t;

struct h2_gizclaw_api_key_state {
  h2_gizclaw_api_key_state_config_t config;
  h2_pal_mutex_t *mutex;
  h2_gizclaw_api_key_snapshot_t snapshot;
  char display_name[sizeof(((gizclaw_rpc_v1_APIKeyCreateRequest *)0)->display_name)];
  uint64_t generation;
  uint64_t started_ms;
  size_t pending_count;
  bool revoke_after;
  bool refresh;
  api_key_pending_t *current;
};

static void api_key_erase(void *data, size_t size) {
  volatile unsigned char *bytes = data;
  while (size-- != 0u)
    *bytes++ = 0;
}

static void api_key_finish(h2_gizclaw_api_key_state_t *state,
                           h2_pal_result_t result) {
  state->snapshot.busy = false;
  state->snapshot.last_error = result;
  ++state->snapshot.revision;
}

/* Detach without cancelling; successful late creates must be revoked. */
static h2_pal_result_t api_key_expire(h2_gizclaw_api_key_state_t *state) {
  if (!state->snapshot.busy)
    return H2_PAL_OK;
  uint64_t now = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(state->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (now - state->started_ms < state->config.timeout_ms)
    return H2_PAL_OK;
  ++state->generation;
  state->current->orphan = true;
  state->current = NULL;
  api_key_finish(state, H2_PAL_ERR_TIMEOUT);
  return rc;
}

static void api_key_complete(void *user, h2_gizclaw_req_t *request,
                             const h2_gizclaw_operation_result_t *result);

/* Caller holds the state mutex. Failed submission never owns a completion. */
static h2_pal_result_t api_key_submit(h2_gizclaw_api_key_state_t *state,
                                      bool revoke, const char *revoke_name,
                                      bool orphan) {
  api_key_pending_t *pending =
      h2_pal_mem_alloc(state->config.mem, sizeof(*pending));
  if (pending == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  *pending = (api_key_pending_t){
      .state = state, .generation = state->generation, .revoke = revoke,
      .orphan = orphan};
  h2_pal_result_t rc;
  if (revoke) {
    h2_gizclaw_str_t name = {revoke_name, strlen(revoke_name)};
    if (name.len < sizeof(pending->name))
      memcpy(pending->name, revoke_name, name.len + 1u);
    rc = h2_gizclaw_req_create_api_key_revoke(
        state->config.service, state->generation, name,
        state->config.timeout_ms, &pending->request);
  } else {
    rc = h2_gizclaw_req_create_api_key_create(
        state->config.service, state->generation, state->config.display_name,
        state->config.manage_api_keys, state->config.timeout_ms,
        &pending->request);
  }
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(pending->request, pending, NULL, NULL,
                           api_key_complete);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_req_release(pending->request);
    h2_pal_mem_free(state->config.mem, pending);
    return rc;
  }
  if (!orphan)
    state->current = pending;
  ++state->pending_count;
  return H2_PAL_OK;
}

static void api_key_complete(void *user, h2_gizclaw_req_t *request,
                             const h2_gizclaw_operation_result_t *result) {
  api_key_pending_t *pending = user;
  h2_gizclaw_api_key_state_t *state = pending->state;
  (void)h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (!pending->orphan && pending->generation == state->generation &&
      !state->snapshot.closed) {
    state->current = NULL;
    h2_pal_result_t rc = result->result;
    if (pending->revoke) {
      if (rc == H2_PAL_OK)
        rc = h2_gizclaw_resp_parse_api_key_revoke(request);
      if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) {
        api_key_erase(&state->snapshot.key, sizeof(state->snapshot.key));
        state->snapshot.valid = false;
        ++state->snapshot.revision;
        if (!state->refresh) {
          state->snapshot.stale = false;
          api_key_finish(state, H2_PAL_OK);
          goto drained;
        }
        rc = api_key_submit(state, false, NULL, false);
        if (rc != H2_PAL_OK)
          api_key_finish(state, rc);
      } else {
        api_key_finish(state, rc);
      }
    } else {
      api_key_erase(&state->snapshot.key, sizeof(state->snapshot.key));
      h2_gizclaw_api_key_t key = {0};
      if (rc == H2_PAL_OK)
        rc = h2_gizclaw_resp_parse_api_key_create(request, &key);
      if (rc == H2_PAL_OK && state->revoke_after) {
        state->snapshot.valid = false;
        ++state->snapshot.revision;
        api_key_erase(key.secret, sizeof(key.secret));
        state->refresh = false;
        rc = api_key_submit(state, true, key.name, false);
        api_key_erase(&key, sizeof(key));
        if (rc != H2_PAL_OK)
          api_key_finish(state, rc);
        goto drained;
      }
      if (rc == H2_PAL_OK)
        state->snapshot.key = key;
      api_key_erase(&key, sizeof(key));
      state->snapshot.valid = rc == H2_PAL_OK;
      state->snapshot.stale = rc != H2_PAL_OK;
      api_key_finish(state, rc);
    }
  } else if (pending->revoke) {
    /* A detached revoke still settles the snapshot key it targeted. */
    h2_pal_result_t rc = result->result;
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_resp_parse_api_key_revoke(request);
    if ((rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) &&
        state->snapshot.valid && pending->name[0] != '\0' &&
        strcmp(state->snapshot.key.name, pending->name) == 0) {
      api_key_erase(&state->snapshot.key, sizeof(state->snapshot.key));
      state->snapshot.valid = false;
      ++state->snapshot.revision;
    }
  } else if (result->result == H2_PAL_OK) {
    h2_gizclaw_api_key_t key = {0};
    h2_pal_result_t rc = h2_gizclaw_resp_parse_api_key_create(request, &key);
    api_key_erase(key.secret, sizeof(key.secret));
    if (rc == H2_PAL_OK)
      (void)api_key_submit(state, true, key.name, true);
    api_key_erase(&key, sizeof(key));
  }
drained:
  h2_gizclaw_req_release(request);
  h2_pal_mem_free(state->config.mem, pending);
  --state->pending_count;
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
}

h2_pal_result_t
h2_gizclaw_api_key_state_create(const h2_gizclaw_api_key_state_config_t *config,
                                h2_gizclaw_api_key_state_t **out_state) {
  if (out_state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_state = NULL;
  if (config == NULL || config->service == NULL || config->mem == NULL ||
      config->sync == NULL || config->time == NULL ||
      config->timeout_ms == 0u || config->display_name.data == NULL ||
      config->display_name.len == 0u ||
      config->display_name.len >=
          sizeof(((h2_gizclaw_api_key_state_t *)0)->display_name) ||
      memchr(config->display_name.data, 0, config->display_name.len) != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_api_key_state_t *state =
      h2_pal_mem_alloc(config->mem, sizeof(*state));
  if (state == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(state, 0, sizeof(*state));
  state->config = *config;
  memcpy(state->display_name, config->display_name.data,
         config->display_name.len);
  state->config.display_name.data = state->display_name;
  state->snapshot.stale = true;
  h2_pal_mutex_config_t mutex_config = {.name = "gizclaw-api-key",
                                        .allocator = config->mem};
  h2_pal_result_t rc =
      h2_pal_mutex_create(config->sync, &mutex_config, &state->mutex);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->mem, state);
    return rc;
  }
  *out_state = state;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_api_key_state_request_refresh(h2_gizclaw_api_key_state_t *state,
                                         bool revoke_current) {
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  rc = api_key_expire(state);
  if (state->snapshot.closed)
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    state->revoke_after = false;
  if (rc == H2_PAL_OK && !state->snapshot.busy) {
    rc = h2_pal_time_get_monotonic_ms(state->config.time, &state->started_ms);
    if (rc == H2_PAL_OK) {
      ++state->generation;
      state->snapshot.busy = true;
      state->snapshot.stale = true;
      ++state->snapshot.revision;
      state->refresh = true;
      bool revoke = revoke_current && state->snapshot.valid;
      rc = api_key_submit(state, revoke, state->snapshot.key.name, false);
      if (rc != H2_PAL_OK) {
        if (!revoke) {
          api_key_erase(&state->snapshot.key, sizeof(state->snapshot.key));
          state->snapshot.valid = false;
        }
        api_key_finish(state, rc);
      }
    }
  }
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
  return rc;
}

h2_pal_result_t
h2_gizclaw_api_key_state_request_revoke(h2_gizclaw_api_key_state_t *state) {
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (state->snapshot.closed) {
    rc = H2_PAL_ERR_CLOSED;
  } else if (state->snapshot.busy) {
    if (state->refresh)
      state->revoke_after = true;
  } else if (state->snapshot.valid) {
    rc = h2_pal_time_get_monotonic_ms(state->config.time, &state->started_ms);
    if (rc == H2_PAL_OK) {
      ++state->generation;
      state->refresh = false;
      state->revoke_after = false;
      state->snapshot.busy = true;
      state->snapshot.stale = true;
      ++state->snapshot.revision;
      rc = api_key_submit(state, true, state->snapshot.key.name, false);
      if (rc != H2_PAL_OK)
        api_key_finish(state, rc);
    }
  }
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
  return rc;
}

h2_pal_result_t
h2_gizclaw_api_key_state_snapshot(h2_gizclaw_api_key_state_t *state,
                                  h2_gizclaw_api_key_snapshot_t *out_snapshot) {
  if (out_snapshot == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_snapshot, 0, sizeof(*out_snapshot));
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  rc = api_key_expire(state);
  if (rc == H2_PAL_OK)
    *out_snapshot = state->snapshot;
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
  return rc;
}

h2_pal_result_t
h2_gizclaw_api_key_state_close(h2_gizclaw_api_key_state_t *state) {
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (!state->snapshot.closed) {
    ++state->generation;
    if (state->current != NULL)
      state->current->orphan = true;
    state->current = NULL;
    state->snapshot.closed = true;
    state->snapshot.stale = true;
    api_key_finish(state, H2_PAL_ERR_CLOSED);
  }
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
  return rc;
}

h2_pal_result_t
h2_gizclaw_api_key_state_destroy(h2_gizclaw_api_key_state_t **ptr) {
  if (ptr == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_api_key_state_t *state = *ptr;
  if (state == NULL)
    return H2_PAL_OK;
  h2_pal_result_t rc = h2_pal_mutex_lock(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  bool busy = state->pending_count != 0u;
  (void)h2_pal_mutex_unlock(state->config.sync, state->mutex);
  if (busy)
    return H2_PAL_ERR_BUSY;
  rc = h2_pal_mutex_destroy(state->config.sync, state->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = state->config.mem;
  api_key_erase(state, sizeof(*state));
  h2_pal_mem_free(mem, state);
  *ptr = NULL;
  return H2_PAL_OK;
}
