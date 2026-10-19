#include "gzc.h"
#include "h2_atomic.h"
#include "h2_gizclaw_local_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_device_internal.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_mhs_internal.h"
#include "payload/tool.pb.h"
#include "pb_encode.h"
#include <limits.h>
#include <string.h>

/* The pinned SDK's canonical dispatcher, also used by gzc_rpc inbound.
 * Keep this dependency inside the GizOS adapter and exercise the actual symbol
 * in compiled tests. No method registry or protobuf validator is duplicated. */
int gzc_client_dispatch_rpc_internal(gzc_client_t *, int, gzc_str_t,
                                     gzc_rpc_provider_respond_fn, void *);

typedef struct tool_binding {
  struct h2_gizclaw_local *rpc;
  const h2_gizclaw_tool_handler_t *handler;
} tool_binding_t;
struct h2_gizclaw_local_call {
  struct h2_gizclaw_local *rpc;
  struct h2_gizclaw_local_call *next;
  h2_gizclaw_rpc_method_t method;
  h2_gizclaw_tool_t inner_tool;
  uint8_t *request, *storage;
  size_t request_size;
  uint64_t deadline_ms;
  h2_gizclaw_local_result_t view;
  h2_gizclaw_rpc_response_complete_fn on_complete;
  void *complete_user;
  bool executing, terminal, responded, canceled, consumed;
  bool caller_reference, completion_requested, completion_running, completed;
  h2_pal_result_t delivery_result;
};
struct h2_gizclaw_local {
  h2_gizclaw_local_config_t platform;
  h2_gizclaw_config_t config;
  size_t borrowers;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *changed;
  h2_pal_task_t *task;
  h2_gizclaw_local_call_t *calls, *tail, *executing;
  size_t count, capacity, limit;
  bool accepting, stopping, failure_ready, task_joined;
  h2_atomic_int_t failure;
  gzc_client_t *dispatcher;
  gzc_platform_t sdk_platform;
  gzc_tool_handler_t tools[H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL];
  h2_gizclaw_tool_handler_t network_tools[H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL];
  tool_binding_t bindings[H2_GIZCLAW_TOOL_LUA_APP_UNINSTALL];
};
static int rpc_lock(h2_gizclaw_local_t *rpc) {
  return h2_pal_mutex_lock(rpc->platform.sync, rpc->mutex);
}
static bool owner_lock(h2_gizclaw_local_t *rpc) {
  int rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    h2_atomic_store(&rpc->failure, rc);
  return rc == H2_PAL_OK;
}
static void rpc_unlock(h2_gizclaw_local_t *rpc) {
  (void)h2_pal_mutex_unlock(rpc->platform.sync, rpc->mutex);
}
static void rpc_wake(h2_gizclaw_local_t *rpc) {
  (void)h2_pal_cond_broadcast(rpc->platform.sync, rpc->changed);
}
static void *sdk_alloc(void *user, size_t size) {
  return h2_pal_mem_alloc(((h2_gizclaw_local_t *)user)->config.allocator,
                          size);
}
static void *sdk_resize(void *user, void *ptr, size_t size) {
  return h2_pal_mem_realloc(
      ((h2_gizclaw_local_t *)user)->config.allocator, ptr, size);
}
static void sdk_free(void *user, void *ptr) {
  h2_pal_mem_free(((h2_gizclaw_local_t *)user)->config.allocator, ptr);
}
static int64_t sdk_now(void *user) {
  uint64_t value = 0;
  (void)h2_pal_time_get_monotonic_ms(
      ((h2_gizclaw_local_t *)user)->config.time, &value);
  return value > INT64_MAX ? INT64_MAX : (int64_t)value;
}
/* The registry-only SDK instance never connects or allocates media. These
 * explicit unsupported transports satisfy the SDK constructor's network
 * shape, and fail closed if accidentally called. Product HTTP tools still
 * use the device's real injected PAL, independently of this dispatcher. */
static int no_http(void *user, const gzc_http_request_t *request,
                   gzc_http_response_t *response) {
  (void)user;
  (void)request;
  (void)response;
  return GZC_ERR_UNSUPPORTED;
}
static int no_peer(void *user, const gzc_webrtc_callbacks_t *callbacks,
                   gzc_rtc_peer_t **out) {
  (void)user;
  (void)callbacks;
  *out = NULL;
  return GZC_ERR_UNSUPPORTED;
}
static int no_offer(gzc_rtc_peer_t *peer) {
  (void)peer;
  return GZC_ERR_UNSUPPORTED;
}
static int no_sdp(gzc_rtc_peer_t *peer, gzc_rtc_sdp_type_t type,
                  gzc_str_t sdp) {
  (void)peer;
  (void)type;
  (void)sdp;
  return GZC_ERR_UNSUPPORTED;
}
static int no_channel(gzc_rtc_peer_t *peer,
                      const gzc_rtc_channel_config_t *config,
                      gzc_rtc_channel_t **out) {
  (void)peer;
  (void)config;
  *out = NULL;
  return GZC_ERR_UNSUPPORTED;
}
static int no_poll(gzc_rtc_peer_t *peer, int timeout_ms) {
  (void)peer;
  (void)timeout_ms;
  return GZC_ERR_UNSUPPORTED;
}
static int no_send(gzc_rtc_channel_t *channel, const uint8_t *data, size_t size,
                   bool text) {
  (void)channel;
  (void)data;
  (void)size;
  (void)text;
  return GZC_ERR_UNSUPPORTED;
}
static void no_channel_close(gzc_rtc_channel_t *channel) { (void)channel; }
static void no_peer_close(gzc_rtc_peer_t *peer) { (void)peer; }
static const gzc_http_vtable_t no_http_api = {.request = no_http};
static const gzc_platform_crypto_t no_crypto;
static const gzc_webrtc_vtable_t no_webrtc = {.peer_create = no_peer,
                                              .peer_start_offer = no_offer,
                                              .peer_set_remote_sdp = no_sdp,
                                              .peer_create_data_channel =
                                                  no_channel,
                                              .peer_poll = no_poll,
                                              .channel_send = no_send,
                                              .channel_close = no_channel_close,
                                              .peer_close = no_peer_close};

static bool encode_bytes(pb_ostream_t *stream, const pb_field_t *field,
                         void *const *arg) {
  const gzc_rpc_provider_response_t *response = *arg;
  return response->payload_len == 0u ||
         (pb_encode_tag_for_field(stream, field) &&
          pb_encode_string(stream, response->payload, response->payload_len));
}
static int copy_response(void *user,
                         const gzc_rpc_provider_response_t *response) {
  h2_gizclaw_local_call_t *call = user;
  if (call->responded || response == NULL ||
      (!response->payload && response->payload_len) ||
      (!response->error_message.data && response->error_message.len))
    return GZC_ERR_INVALID_ARGUMENT;
  call->responded = true;
  call->view.has_error = response->has_error;
  call->view.error_code = response->error_code;
  size_t payload_size = response->has_error ? 0u : response->payload_len;
  const bool wrap = !response->has_error && call->inner_tool == 0 &&
                    call->method == H2_GIZCLAW_RPC_CLIENT_TOOL_V0_INVOKE;
  gizclaw_rpc_v1_ClientToolV0InvokeResponse envelope = {0};
  envelope.payload =
      (pb_callback_t){.funcs.encode = encode_bytes, .arg = (void *)response};
  if (wrap && !pb_get_encoded_size(
                  &payload_size,
                  gizclaw_rpc_v1_ClientToolV0InvokeResponse_fields, &envelope))
    return GZC_ERR_RPC;
  size_t error_size = response->has_error ? response->error_message.len : 0u;
  if (payload_size > call->rpc->limit ||
      error_size > call->rpc->limit - payload_size) {
    call->view.result = H2_PAL_ERR_NO_SPACE;
    return GZC_ERR_NO_MEMORY;
  }
  if (payload_size + error_size != 0u) {
    call->storage = sdk_alloc(call->rpc, payload_size + error_size);
    if (!call->storage)
      return GZC_ERR_NO_MEMORY;
    if (wrap) {
      pb_ostream_t stream = pb_ostream_from_buffer(call->storage, payload_size);
      if (!pb_encode(&stream, gizclaw_rpc_v1_ClientToolV0InvokeResponse_fields,
                     &envelope))
        return GZC_ERR_RPC;
    } else if (payload_size)
      memcpy(call->storage, response->payload, payload_size);
    if (error_size)
      memcpy(call->storage + payload_size, response->error_message.data,
             error_size);
  }
  call->view.payload = (h2_gizclaw_rpc_bytes_t){call->storage, payload_size};
  call->view.error_message = (h2_gizclaw_rpc_bytes_t){
      error_size ? call->storage + payload_size : NULL, error_size};
  return GZC_OK;
}
static int invoke_tool(void *user, gzc_str_t bytes,
                       gzc_rpc_provider_respond_fn respond,
                       void *respond_user) {
  tool_binding_t *binding = user;
  h2_gizclaw_local_call_t *call = binding->rpc->executing;
  h2_gizclaw_rpc_provider_response_t response = {0};
  int rc = binding->handler->invoke(
      binding->handler->user, binding->handler->tool,
      (h2_gizclaw_rpc_bytes_t){(const uint8_t *)bytes.data, bytes.len},
      &response);
  call->view.result = rc;
  call->on_complete = response.on_complete;
  call->complete_user = response.complete_user;
  if (rc != H2_PAL_OK)
    return h2_gizclaw_provider_result_to_gzc(rc);
  gzc_rpc_provider_response_t result = {
      .payload = response.payload.data,
      .payload_len = response.payload.len,
      .has_error = response.has_error,
      .error_code = response.error_code,
      .error_message = {(const char *)response.error_message.data,
                        response.error_message.len}};
  return respond(respond_user, &result);
}
static int invoke_mhs(void *user, int method, gzc_str_t bytes,
                      gzc_rpc_provider_respond_fn respond, void *respond_user) {
  h2_gizclaw_local_t *rpc = user;
  uint8_t *storage = NULL;
  h2_gizclaw_rpc_provider_response_t response = {0};
  int rc = h2_gizclaw_mhs_request_internal(
      rpc->config.mhs_devices, rpc->config.mhs_device_count,
      method == H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE, rpc->config.allocator,
      (h2_gizclaw_rpc_bytes_t){(const uint8_t *)bytes.data, bytes.len},
      &response, &storage);
  rpc->executing->view.result = rc;
  gzc_rpc_provider_response_t result = {
      .payload = response.payload.data,
      .payload_len = response.payload.len,
      .has_error = rc != H2_PAL_OK,
      .error_code = rc == H2_PAL_OK ? 0 : h2_gizclaw_mhs_error_internal(rc)};
  int sent = respond(respond_user, &result);
  sdk_free(rpc, storage);
  return sent;
}
static void destroy_call(h2_gizclaw_local_call_t *call) {
  sdk_free(call->rpc, call->request);
  sdk_free(call->rpc, call->storage);
  sdk_free(call->rpc, call);
}
/* Mutex held; no callback or PAL I/O is performed by this helper. */
static void unlink_call(h2_gizclaw_local_t *rpc,
                        h2_gizclaw_local_call_t *call) {
  h2_gizclaw_local_call_t **position = &rpc->calls;
  h2_gizclaw_local_call_t *previous = NULL;
  while (*position != call) {
    previous = *position;
    position = &(*position)->next;
  }
  *position = call->next;
  if (rpc->tail == call)
    rpc->tail = previous;
  --rpc->count;
}
static void execute(h2_gizclaw_local_call_t *call) {
  h2_gizclaw_local_t *rpc = call->rpc;
  rpc->executing = call;
  gzc_str_t request = {(const char *)call->request, call->request_size};
  int rc = GZC_ERR_UNSUPPORTED;
  if (call->inner_tool) {
    for (size_t i = 0; i < rpc->config.tool_handler_count; ++i)
      if ((int)rpc->tools[i].tool == (int)call->inner_tool) {
        rc = invoke_tool(&rpc->bindings[i], request, copy_response, call);
        break;
      }
  } else {
    rc = gzc_client_dispatch_rpc_internal(rpc->dispatcher, call->method,
                                          request, copy_response, call);
  }
  if (rc != GZC_OK || !call->responded) {
    if (call->view.result == H2_PAL_OK)
      call->view.result =
          h2_gizclaw_result_from_gzc(rc == GZC_OK ? GZC_ERR_RPC : rc);
    call->view.has_error = true;
    call->view.error_code = rc == GZC_ERR_UNSUPPORTED
                                ? H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED
                                : H2_GIZCLAW_RPC_ERROR_INTERNAL;
  }
  rpc->executing = NULL;
}
static void owner(void *user) {
  h2_gizclaw_local_t *rpc = user;
  for (;;) {
    if (!owner_lock(rpc))
      return;
    h2_gizclaw_local_call_t *work = NULL, *completion = NULL, *garbage = NULL;
    uint64_t now = 0;
    int clock = h2_pal_time_get_monotonic_ms(rpc->config.time, &now);
    for (h2_gizclaw_local_call_t *call = rpc->calls; call; call = call->next) {
      if (call->terminal && !call->completion_running && !call->completed &&
          (call->completion_requested || call->canceled || rpc->stopping)) {
        completion = call;
        completion->completion_running = true;
        break;
      }
      if (call->terminal && call->completed && !call->caller_reference) {
        garbage = call;
        unlink_call(rpc, call);
        break;
      }
      if (!call->terminal && !call->executing) {
        if (call->canceled || rpc->stopping || clock != H2_PAL_OK ||
            now >= call->deadline_ms) {
          call->terminal = true;
          call->view.result = clock != H2_PAL_OK         ? clock
                              : now >= call->deadline_ms ? H2_PAL_ERR_TIMEOUT
                                                         : H2_PAL_ERR_CLOSED;
          call->completion_requested = true;
          call->delivery_result = call->view.result;
          rpc_wake(rpc);
          completion = call;
          completion->completion_running = true;
          break;
        }
        work = call;
        call->executing = true;
        break;
      }
    }
    if (!work && !completion && !garbage) {
      if (rpc->stopping) {
        rpc_unlock(rpc);
        return;
      }
      int waited = h2_pal_cond_wait(rpc->platform.sync, rpc->changed, rpc->mutex, 10u);
      if (waited != H2_PAL_OK && waited != H2_PAL_ERR_TIMEOUT)
        h2_atomic_store(&rpc->failure, waited);
      rpc_unlock(rpc);
      if (waited != H2_PAL_OK && waited != H2_PAL_ERR_TIMEOUT)
        return;
      continue;
    }
    int completion_result =
        completion && (completion->canceled || rpc->stopping)
            ? H2_PAL_ERR_CLOSED
        : completion ? completion->delivery_result
                     : H2_PAL_OK;
    rpc_unlock(rpc);
    if (garbage) {
      destroy_call(garbage);
      continue;
    }
    if (completion) {
      int result = completion_result;
      if (completion->view.result != H2_PAL_OK)
        result = completion->view.result;
      else if (completion->view.has_error)
        result = H2_PAL_ERR_INVALID_STATE;
      if (completion->on_complete)
        completion->on_complete(completion->complete_user, result);
      if (!owner_lock(rpc))
        return;
      completion->completed = true;
      completion->completion_running = false;
      rpc_wake(rpc);
      rpc_unlock(rpc);
      continue;
    }
    execute(work);
    if (!owner_lock(rpc))
      return;
    work->terminal = true;
    work->executing = false;
    int finished_clock = h2_pal_time_get_monotonic_ms(rpc->config.time, &now);
    if (finished_clock != H2_PAL_OK || now >= work->deadline_ms) {
      work->canceled = true;
      work->view.result = finished_clock != H2_PAL_OK ? finished_clock : H2_PAL_ERR_TIMEOUT;
    }
    if (work->canceled || rpc->stopping) {
      work->completion_requested = true;
      work->delivery_result = H2_PAL_ERR_CLOSED;
    }
    rpc_wake(rpc);
    rpc_unlock(rpc);
  }
}

h2_pal_result_t h2_gizclaw_local_create_internal(
    const h2_gizclaw_local_config_t *platform,
    const h2_gizclaw_config_t *config, h2_gizclaw_local_t **out) {
  *out = NULL;
  h2_gizclaw_local_t *rpc =
      h2_pal_mem_alloc(config->allocator, sizeof(*rpc));
  if (!rpc)
    return H2_PAL_ERR_NO_MEMORY;
  memset(rpc, 0, sizeof(*rpc));
  rpc->platform = *platform;
  rpc->config = *config;
  rpc->capacity = platform->request_capacity ? platform->request_capacity : 4u;
  rpc->limit =
      platform->request_limit_bytes ? platform->request_limit_bytes : 65536u;
  *out = rpc;
  if (h2_atomic_int_init(&rpc->failure, H2_PAL_OK) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  rpc->failure_ready = true;
  const h2_pal_mutex_config_t mutex = {.name = "gizclaw-local",
                                       .allocator = config->allocator};
  int rc = h2_pal_mutex_create(platform->sync, &mutex, &rpc->mutex);
  if (rc == H2_PAL_OK) {
    const h2_pal_cond_config_t cond = {.name = "gizclaw-local",
                                       .allocator = config->allocator};
    rc = h2_pal_cond_create(platform->sync, &cond, &rpc->changed);
  }
  rpc->sdk_platform = (gzc_platform_t){.userdata = rpc,
                                       .malloc = sdk_alloc,
                                       .realloc = sdk_resize,
                                       .free = sdk_free,
                                       .time_instant_ms = sdk_now};
  for (size_t i = 0; i < config->tool_handler_count; ++i) {
    rpc->bindings[i] = (tool_binding_t){rpc, &config->tool_handlers[i]};
    rpc->tools[i] = (gzc_tool_handler_t){
        (gizclaw_rpc_v1_ClientTool)config->tool_handlers[i].tool, invoke_tool,
        &rpc->bindings[i]};
  }
  if (rc == H2_PAL_OK) {
    const gzc_client_config_t registry = {
        .server_endpoint = {"local.invalid", sizeof("local.invalid") - 1u},
        .platform = &rpc->sdk_platform,
        .http = &no_http_api,
        .webrtc = &no_webrtc,
        .crypto = &no_crypto,
        .write_timeout_ms = 1,
        .tool_handlers = config->tool_handler_count ? rpc->tools : NULL,
        .tool_handler_count = config->tool_handler_count,
        .mhs_read = invoke_mhs,
        .mhs_write = invoke_mhs,
        .mhs_userdata = rpc};
    rc = h2_gizclaw_result_from_gzc(
        gzc_client_create(&registry, &rpc->dispatcher));
  }
  return rc;
}
h2_pal_result_t
h2_gizclaw_local_start_internal(h2_gizclaw_local_t *rpc) {
  if (!rpc)
    return H2_PAL_OK;
  int rc = h2_atomic_load(&rpc->failure);
  if (rc != H2_PAL_OK)
    return rc;
  rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  if (rpc->task || rpc->count) {
    rpc_unlock(rpc);
    return H2_PAL_ERR_BUSY;
  }
  rpc->stopping = false;
  rpc->accepting = false;
  rpc_unlock(rpc);
  const h2_pal_task_options_t task = {.name = "$gizclaw/local",
                                      .min_stack_size = 65536u};
  rc = h2_pal_task_start(rpc->platform.task, &task, owner, rpc, &rpc->task);
  int locked = rpc_lock(rpc);
  if (locked != H2_PAL_OK)
    return locked;
  rpc->accepting = rc == H2_PAL_OK;
  rpc_unlock(rpc);
  return rc;
}
h2_pal_result_t
h2_gizclaw_local_stop_internal(h2_gizclaw_local_t *rpc) {
  if (!rpc)
    return H2_PAL_OK;
  int rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  rpc->accepting = false;
  rpc->stopping = true;
  rpc_wake(rpc);
  rpc_unlock(rpc);
  if (rpc->task) {
    if (!rpc->task_joined) {
      rc = h2_pal_task_join(rpc->platform.task, rpc->task);
      if (rc != H2_PAL_OK)
        return rc;
      rpc->task_joined = true;
    }
    rc = rpc_lock(rpc);
    if (rc != H2_PAL_OK)
      return rc;
    rpc->task = NULL;
    rpc->task_joined = false;
    rpc_unlock(rpc);
  }
  rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  bool busy = rpc->count != 0;
  rpc_unlock(rpc);
  int failure = h2_atomic_load(&rpc->failure);
  return failure != H2_PAL_OK ? failure : busy ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_local_destroy_internal(h2_gizclaw_local_t **pointer) {
  if (!pointer || !*pointer)
    return H2_PAL_OK;
  h2_gizclaw_local_t *rpc = *pointer;
  if (rpc->task || rpc->count || rpc->borrowers)
    return H2_PAL_ERR_BUSY;
  if (rpc->changed) {
    int rc = h2_pal_cond_destroy(rpc->platform.sync, rpc->changed);
    if (rc != H2_PAL_OK)
      return rc;
    rpc->changed = NULL;
  }
  if (rpc->mutex) {
    int rc = h2_pal_mutex_destroy(rpc->platform.sync, rpc->mutex);
    if (rc != H2_PAL_OK)
      return rc;
    rpc->mutex = NULL;
  }
  gzc_client_destroy(rpc->dispatcher);
  if (rpc->failure_ready)
    h2_atomic_int_destroy(&rpc->failure);
  sdk_free(rpc, rpc);
  *pointer = NULL;
  return H2_PAL_OK;
}
static int submit(h2_gizclaw_local_t *rpc, int method,
                  h2_gizclaw_tool_t inner,
                  h2_gizclaw_rpc_bytes_t request, uint32_t timeout_ms,
                  h2_gizclaw_local_call_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!rpc || (request.len && !request.data) || !timeout_ms ||
      timeout_ms > 120000u)
    return H2_PAL_ERR_INVALID_ARG;
  if (request.len > rpc->limit)
    return H2_PAL_ERR_NO_SPACE;
  int failure = h2_atomic_load(&rpc->failure);
  if (failure != H2_PAL_OK)
    return failure;
  uint64_t now = 0;
  int rc = h2_pal_time_get_monotonic_ms(rpc->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  if (!rpc->accepting) {
    rpc_unlock(rpc);
    return H2_PAL_ERR_CLOSED;
  }
  if (rpc->count == rpc->capacity) {
    rpc_unlock(rpc);
    return H2_PAL_ERR_BUSY;
  }
  h2_gizclaw_local_call_t *call = sdk_alloc(rpc, sizeof(*call));
  if (!call) {
    rpc_unlock(rpc);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(call, 0, sizeof(*call));
  call->rpc = rpc;
  if (request.len) {
    call->request = sdk_alloc(rpc, request.len);
    if (!call->request) {
      sdk_free(rpc, call);
      rpc_unlock(rpc);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(call->request, request.data, request.len);
  }
  call->request_size = request.len;
  call->method = method;
  call->inner_tool = inner;
  call->deadline_ms =
      now > UINT64_MAX - timeout_ms ? UINT64_MAX : now + timeout_ms;
  call->caller_reference = true;
  if (rpc->tail)
    rpc->tail->next = call;
  else
    rpc->calls = call;
  rpc->tail = call;
  ++rpc->count;
  *out = call;
  rpc_wake(rpc);
  rpc_unlock(rpc);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_local_submit(h2_gizclaw_service_t *device,
                                         h2_gizclaw_rpc_method_t method,
                                         h2_gizclaw_rpc_bytes_t request,
                                         uint32_t timeout_ms,
                                         h2_gizclaw_local_call_t **out) {
  return submit(h2_gizclaw_local_internal(device), method, 0,
                request, timeout_ms, out);
}
h2_pal_result_t h2_gizclaw_local_call_wait(h2_gizclaw_local_call_t *call,
                                            uint32_t timeout_ms,
                                            h2_gizclaw_local_result_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!call)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_local_t *rpc = call->rpc;
  uint64_t began = 0;
  int rc = h2_pal_time_get_monotonic_ms(rpc->config.time, &began);
  if (rc != H2_PAL_OK)
    return rc;
  rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  while (!call->terminal) {
    if (call->canceled) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    rc = h2_atomic_load(&rpc->failure);
    if (rc != H2_PAL_OK)
      break;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(rpc->config.time, &now);
    if (rc != H2_PAL_OK)
      break;
    uint64_t elapsed = now >= began ? now - began : 0;
    if (elapsed >= timeout_ms) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    rc = h2_pal_cond_wait(rpc->platform.sync, rpc->changed, rpc->mutex,
                          (uint32_t)(timeout_ms - elapsed));
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      break;
  }
  if (call->terminal) {
    rc = call->consumed ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
    if (rc == H2_PAL_OK)
      *out = call->view;
  }
  rpc_unlock(rpc);
  return rc;
}
h2_pal_result_t h2_gizclaw_local_call_cancel(h2_gizclaw_local_call_t *call) {
  if (!call)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = rpc_lock(call->rpc);
  if (rc != H2_PAL_OK)
    return rc;
  if (call->completion_running || call->completed)
    rc = H2_PAL_ERR_CLOSED;
  else {
    call->canceled = true;
    rpc_wake(call->rpc);
  }
  rpc_unlock(call->rpc);
  return rc;
}
h2_pal_result_t h2_gizclaw_local_call_consume(h2_gizclaw_local_call_t *call) {
  if (!call)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = rpc_lock(call->rpc);
  if (rc != H2_PAL_OK)
    return rc;
  if (!call->terminal)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  else if (!call->consumed) {
    sdk_free(call->rpc, call->storage);
    call->storage = NULL;
    call->view.payload = (h2_gizclaw_rpc_bytes_t){0};
    call->view.error_message = (h2_gizclaw_rpc_bytes_t){0};
    call->consumed = true;
  }
  rpc_unlock(call->rpc);
  return rc;
}
h2_pal_result_t
h2_gizclaw_local_call_complete(h2_gizclaw_local_call_t *call,
                                h2_pal_result_t delivery_result) {
  if (!call)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = rpc_lock(call->rpc);
  if (rc != H2_PAL_OK)
    return rc;
  if (!call->terminal)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  else if (call->completion_requested || call->completed)
    rc = H2_PAL_ERR_CLOSED;
  else {
    call->completion_requested = true;
    call->delivery_result = delivery_result;
    rpc_wake(call->rpc);
  }
  rpc_unlock(call->rpc);
  return rc;
}
h2_pal_result_t
h2_gizclaw_local_call_release(h2_gizclaw_local_call_t **pointer) {
  if (!pointer)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_local_call_t *call = *pointer;
  if (!call)
    return H2_PAL_OK;
  h2_gizclaw_local_t *rpc = call->rpc;
  int rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK)
    return rc;
  call->caller_reference = false;
  if (!call->completion_requested && !call->completed)
    call->canceled = true;
  bool reclaim = !rpc->task && call->terminal && call->completed;
  if (reclaim) {
    unlink_call(rpc, call);
    /* Quiescence must not become observable before the last use of rpc. */
    destroy_call(call);
  }
  rpc_wake(rpc);
  *pointer = NULL;
  rpc_unlock(rpc);
  return H2_PAL_OK;
}
static void network_complete(void *user, int result) {
  h2_gizclaw_local_call_t *call = user;
  (void)h2_gizclaw_local_call_complete(call, result);
  (void)h2_gizclaw_local_call_release(&call);
}
static int network_call(h2_gizclaw_service_t *device, int method,
                        h2_gizclaw_tool_t inner, h2_gizclaw_rpc_bytes_t request,
                        h2_gizclaw_rpc_provider_response_t *out) {
  h2_gizclaw_local_t *rpc = h2_gizclaw_local_internal(device);
  h2_gizclaw_local_call_t *call = NULL;
  int rc = submit(rpc, method, inner, request, 110000u, &call);
  h2_gizclaw_local_result_t result = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_local_call_wait(call, 110000u, &result);
  }
  if (rc != H2_PAL_OK) {
    (void)h2_gizclaw_local_call_release(&call);
    return rc;
  }
  *out = (h2_gizclaw_rpc_provider_response_t){.payload = result.payload,
                                              .has_error = result.has_error,
                                              .error_code = result.error_code,
                                              .error_message =
                                                  result.error_message,
                                              .on_complete = network_complete,
                                              .complete_user = call};
  return H2_PAL_OK;
}
int h2_gizclaw_local_network_tool_internal(
    void *user, h2_gizclaw_tool_t tool, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *out) {
  return network_call(user, H2_GIZCLAW_RPC_CLIENT_TOOL_V0_INVOKE, tool, request,
                      out);
}
int h2_gizclaw_local_network_mhs_internal(
    h2_gizclaw_service_t *device, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t request, h2_gizclaw_rpc_provider_response_t *out) {
  return network_call(device, method, 0, request, out);
}

const h2_gizclaw_tool_handler_t *
h2_gizclaw_local_network_tools_internal(h2_gizclaw_local_t *rpc,
                                             h2_gizclaw_service_t *device) {
  for (size_t i = 0; i < rpc->config.tool_handler_count; ++i)
    rpc->network_tools[i] = (h2_gizclaw_tool_handler_t){
        rpc->config.tool_handlers[i].tool,
        h2_gizclaw_local_network_tool_internal, device};
  return rpc->config.tool_handler_count ? rpc->network_tools : NULL;
}
void h2_gizclaw_local_network_response_copied_internal(
    h2_gizclaw_rpc_provider_response_t *response) {
  if (response->on_complete == network_complete)
    (void)h2_gizclaw_local_call_consume(response->complete_user);
}

bool h2_gizclaw_local_quiescent_internal(h2_gizclaw_local_t *rpc) {
  if (!rpc)
    return true;
  if (!rpc->mutex)
    return !rpc->task && !rpc->count && !rpc->borrowers;
  if (rpc_lock(rpc) != H2_PAL_OK)
    return false;
  bool quiet = !rpc->task && !rpc->count && !rpc->borrowers;
  rpc_unlock(rpc);
  return quiet;
}

bool h2_gizclaw_local_network_response_needs_drain_internal(
    const h2_gizclaw_rpc_provider_response_t *response) {
  if (response->on_complete != network_complete)
    return response->on_complete != NULL;
  const h2_gizclaw_local_call_t *call = response->complete_user;
  return !response->has_error && call->on_complete != NULL;
}

h2_gizclaw_local_t *h2_gizclaw_local_internal(h2_gizclaw_service_t *service) {
  return service ? service->local : NULL;
}
h2_pal_result_t h2_gizclaw_local_start(h2_gizclaw_service_t *service) {
  if (!service) return H2_PAL_ERR_INVALID_ARG;
  if (service->started || service->stopping || service->stopped || service->local)
    return H2_PAL_ERR_INVALID_STATE;
  const h2_gizclaw_local_config_t platform = {.task = service->config.task,
      .sync = service->config.sync, .request_capacity = 4u,
      .request_limit_bytes = 65536u};
  int rc = h2_gizclaw_local_create_internal(&platform, &service->client_config,
                                           &service->local);
  if (rc != H2_PAL_OK) {
    int cleanup = h2_gizclaw_local_destroy_internal(&service->local);
    return cleanup == H2_PAL_OK ? rc : cleanup;
  }
  rc = h2_gizclaw_device_start_internal(service->device);
  if (rc == H2_PAL_OK) rc = h2_gizclaw_local_start_internal(service->local);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_device_cancel_internal(service->device);
    (void)h2_gizclaw_local_stop_internal(service->local);
    (void)h2_gizclaw_device_stop_internal(service->device);
    return rc;
  }
  service->client_config.tool_handlers =
      h2_gizclaw_local_network_tools_internal(service->local, service);
  return H2_PAL_OK;
}
void h2_gizclaw_local_cancel_internal(h2_gizclaw_local_t *rpc) {
  if (!rpc || !rpc->mutex || rpc_lock(rpc) != H2_PAL_OK) return;
  rpc->accepting = false;
  rpc->stopping = true;
  for (h2_gizclaw_local_call_t *call = rpc->calls; call; call = call->next)
    if (!call->completion_running && !call->completed) call->canceled = true;
  rpc_wake(rpc);
  rpc_unlock(rpc);
}
h2_pal_result_t h2_gizclaw_local_retain(h2_gizclaw_service_t *service) {
  h2_gizclaw_local_t *rpc = h2_gizclaw_local_internal(service);
  if (!rpc) return H2_PAL_ERR_INVALID_ARG;
  int rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK) return rc;
  if (!rpc->accepting) rc = H2_PAL_ERR_CLOSED;
  else if (rpc->borrowers == SIZE_MAX) rc = H2_PAL_ERR_NO_SPACE;
  else ++rpc->borrowers;
  rpc_unlock(rpc);
  return rc;
}
h2_pal_result_t h2_gizclaw_local_release(h2_gizclaw_service_t **pointer) {
  if (!pointer) return H2_PAL_ERR_INVALID_ARG;
  if (!*pointer) return H2_PAL_OK;
  h2_gizclaw_local_t *rpc = h2_gizclaw_local_internal(*pointer);
  if (!rpc) return H2_PAL_ERR_INVALID_ARG;
  int rc = rpc_lock(rpc);
  if (rc != H2_PAL_OK) return rc;
  if (!rpc->borrowers) rc = H2_PAL_ERR_INVALID_STATE;
  else { --rpc->borrowers; *pointer = NULL; }
  rpc_unlock(rpc);
  return rc;
}
