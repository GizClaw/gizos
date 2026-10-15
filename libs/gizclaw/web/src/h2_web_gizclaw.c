#include "h2_web_gizclaw.h"
#include "h2_atomic.h"
#include "h2_web_gizclaw_internal.h"
#include "payload/tool.pb.h"
#include "pb_decode.h"
#include <emscripten.h>
#include <string.h>

typedef struct web_call {
  h2_atomic_bool_t claimed, canceled;
  uint32_t id, size, timeout_ms;
  int32_t method;
  uint8_t *input;
} web_call_t;
struct h2_web_gizclaw {
  h2_web_gizclaw_config_t config;
  h2_pal_queue_t *commands;
  h2_pal_task_t *worker;
  h2_atomic_bool_t stopping;
  bool stopping_ready, bound, retained;
  size_t slots_ready, cancels_ready;
  web_call_t *calls;
  uint8_t *input;
};
/* UI-owned ingress pointer. Synchronous UI detach runs after the last ingress
 * callback, before checked join/free on the entry Worker. */
static h2_web_gizclaw_t *current;
EMSCRIPTEN_KEEPALIVE void h2_web_gizclaw_set_current(uintptr_t pointer) {
  current = (h2_web_gizclaw_t *)pointer;
}
EMSCRIPTEN_KEEPALIVE void h2_web_gizclaw_cancel(uint32_t id) {
  h2_web_gizclaw_t *bridge = current;
  if (!bridge)
    return;
  for (size_t i = 0; i < bridge->config.request_capacity; ++i) {
    web_call_t *call = &bridge->calls[i];
    if (h2_atomic_bool_load(&call->claimed, H2_ATOMIC_ACQUIRE) &&
        call->id == id)
      h2_atomic_bool_store(&call->canceled, true, H2_ATOMIC_RELEASE);
  }
}
EMSCRIPTEN_KEEPALIVE int h2_web_gizclaw_request(uint32_t id, int32_t method,
                                                uint32_t size,
                                                uint32_t timeout_ms) {
  h2_web_gizclaw_t *bridge = current;
  if (!bridge || h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_CLOSED;
  if (!id || size > bridge->config.request_limit_bytes || !timeout_ms ||
      timeout_ms > 120000u)
    return H2_PAL_ERR_INVALID_ARG;
  web_call_t *call = NULL;
  for (size_t i = 0; i < bridge->config.request_capacity; ++i) {
    bool available = false;
    if (h2_atomic_bool_compare_exchange(&bridge->calls[i].claimed, &available,
                                        true, H2_ATOMIC_ACQ_REL,
                                        H2_ATOMIC_ACQUIRE)) {
      call = &bridge->calls[i];
      break;
    }
  }
  if (!call)
    return H2_PAL_ERR_BUSY;
  call->id = id;
  call->method = method;
  call->size = size;
  call->timeout_ms = timeout_ms;
  h2_atomic_bool_store(&call->canceled, false, H2_ATOMIC_RELEASE);
  memcpy(call->input, bridge->input, size);
  int rc = h2_pal_queue_send(bridge->config.queue, bridge->commands, &call,
                             H2_PAL_QUEUE_NO_WAIT);
  if (rc != H2_PAL_OK)
    h2_atomic_bool_store(&call->claimed, false, H2_ATOMIC_RELEASE);
  return rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK
             ? H2_PAL_ERR_WOULD_BLOCK
             : rc;
}
static int finish(h2_web_gizclaw_t *bridge, web_call_t *call,
                  const h2_gizclaw_local_result_t *view) {
  h2_gizclaw_local_result_t result = *view;
  gizclaw_rpc_v1_ClientToolV0ListResponse tools = {0};
  uint32_t ids[sizeof(tools.tools) / sizeof(tools.tools[0])];
  int native_count = -1;
  // Discovery is a packed enum array. Reuse the SDK's generated C descriptor;
  // its JS codec currently accepts only unpacked scalar arrays. Raw bytes are
  // still delivered unchanged; this is typed adapter output, not a wire parser.
  if (result.result == H2_PAL_OK && !result.has_error &&
      call->method == H2_GIZCLAW_RPC_CLIENT_TOOL_V0_LIST) {
    pb_istream_t input =
        pb_istream_from_buffer(result.payload.data, result.payload.len);
    if (!pb_decode(&input, gizclaw_rpc_v1_ClientToolV0ListResponse_fields,
                   &tools))
      result.result = H2_PAL_ERR_FORMAT;
    else {
      native_count = (int)tools.tools_count;
      for (size_t i = 0; i < tools.tools_count; ++i)
        ids[i] = (uint32_t)tools.tools[i];
    }
  }
  int has_error = result.has_error;
  uint32_t length = (uint32_t)result.payload.len,
           message_size = (uint32_t)result.error_message.len;
  const uint32_t *tool_ids = ids;
  return h2_web_main_call(
             h2_web_gizclaw_finish_js,
             (const void *[]){&bridge, &call->id, &result.result, &has_error,
                              &result.error_code, &result.payload.data, &length,
                              &result.error_message.data, &message_size,
                              &tool_ids, &native_count})
      .i32;
}
static void web_worker(void *user) {
  h2_web_gizclaw_t *bridge = user;
  for (;;) {
    web_call_t *request = NULL;
    int received = h2_pal_queue_recv(bridge->config.queue, bridge->commands,
                                     &request, 10u);
    if (received != H2_PAL_OK) {
      if (h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE))
        return;
      continue;
    }
    h2_gizclaw_local_call_t *call = NULL;
    h2_gizclaw_local_result_t view = {0};
    if (h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE) ||
        h2_atomic_bool_load(&request->canceled, H2_ATOMIC_ACQUIRE))
      view.result = H2_PAL_ERR_CLOSED;
    else
      view.result = h2_gizclaw_local_submit(
          bridge->config.service, request->method,
          (h2_gizclaw_rpc_bytes_t){request->input, request->size},
          request->timeout_ms, &call);
    if (view.result == H2_PAL_OK) {
      for (;;) {
        if (h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE) ||
            h2_atomic_bool_load(&request->canceled, H2_ATOMIC_ACQUIRE)) {
          (void)h2_gizclaw_local_call_cancel(call);
          view.result = H2_PAL_ERR_CLOSED;
          break;
        }
        int rc = h2_gizclaw_local_call_wait(call, 10u, &view);
        if (rc == H2_PAL_ERR_TIMEOUT)
          continue;
        if (rc != H2_PAL_OK)
          view.result = rc;
        break;
      }
    }
    int delivery = finish(bridge, request, &view);
    if (call) {
      (void)h2_gizclaw_local_call_complete(call, delivery);
      (void)h2_gizclaw_local_call_release(&call);
    }
    h2_atomic_bool_store(&request->claimed, false, H2_ATOMIC_RELEASE);
  }
}
h2_pal_result_t h2_web_gizclaw_open(const h2_web_gizclaw_config_t *config,
                                    h2_web_gizclaw_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!config || !config->service || !config->allocator ||
      !config->allocator->vtable || !config->allocator->vtable->alloc ||
      !config->allocator->vtable->free || !config->task ||
      !config->task->vtable || !config->task->vtable->start ||
      !config->task->vtable->join || !config->queue || !config->queue->vtable ||
      !config->queue->vtable->create || !config->queue->vtable->send ||
      !config->queue->vtable->recv || !config->queue->vtable->destroy ||
      config->request_capacity > 32u ||
      config->request_limit_bytes > 1024u * 1024u ||
      config->timeout_ms > 120000u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_gizclaw_t *bridge =
      h2_pal_mem_alloc(config->allocator, sizeof(*bridge));
  if (!bridge)
    return H2_PAL_ERR_NO_MEMORY;
  memset(bridge, 0, sizeof(*bridge));
  bridge->config = *config;
  if (!bridge->config.request_capacity)
    bridge->config.request_capacity = 4u;
  if (!bridge->config.request_limit_bytes)
    bridge->config.request_limit_bytes = 65536u;
  if (!bridge->config.timeout_ms)
    bridge->config.timeout_ms = 30000u;
  int rc = h2_gizclaw_local_retain(config->service);
  if (rc != H2_PAL_OK)
    goto fail;
  bridge->retained = true;
  rc = H2_PAL_ERR_NO_MEMORY;
  if (h2_atomic_bool_init(&bridge->stopping, false) != H2_ATOMIC_OK)
    goto fail;
  bridge->stopping_ready = true;
  bridge->calls =
      h2_pal_mem_alloc(config->allocator, bridge->config.request_capacity *
                                              sizeof(*bridge->calls));
  bridge->input =
      h2_pal_mem_alloc(config->allocator, bridge->config.request_limit_bytes);
  if (!bridge->calls || !bridge->input)
    goto fail;
  memset(bridge->calls, 0,
         bridge->config.request_capacity * sizeof(*bridge->calls));
  for (size_t i = 0; i < bridge->config.request_capacity; ++i) {
    web_call_t *call = &bridge->calls[i];
    if (h2_atomic_bool_init(&call->claimed, false) != H2_ATOMIC_OK)
      goto fail;
    ++bridge->slots_ready;
    if (h2_atomic_bool_init(&call->canceled, false) != H2_ATOMIC_OK)
      goto fail;
    ++bridge->cancels_ready;
    call->input =
        h2_pal_mem_alloc(config->allocator, bridge->config.request_limit_bytes);
    if (!call->input)
      goto fail;
  }
  const h2_pal_queue_config_t queue = {.name = "gizclaw/web/device",
                                       .allocator = config->allocator,
                                       .item_count =
                                           bridge->config.request_capacity,
                                       .item_size = sizeof(web_call_t *)};
  rc = h2_pal_queue_create(config->queue, &queue, &bridge->commands);
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t task = {.name = "$gizclaw/web/device",
                                        .min_stack_size = 32768u};
    rc = h2_pal_task_start(config->task, &task, web_worker, bridge,
                           &bridge->worker);
  }
  if (rc == H2_PAL_OK) {
    const uint32_t limit = (uint32_t)bridge->config.request_limit_bytes;
    const uint32_t capacity = (uint32_t)bridge->config.request_capacity;
    rc = h2_web_main_call(h2_web_gizclaw_bind_js,
                          (const void *[]){&bridge, &bridge->input, &limit,
                                           &bridge->config.timeout_ms,
                                           &capacity})
             .i32;
    bridge->bound = rc == H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) {
    *out = bridge;
    return rc;
  }
fail:
  *out = bridge;
  int cleanup = h2_web_gizclaw_close(out);
  return cleanup == H2_PAL_OK ? rc : cleanup;
}
h2_pal_result_t h2_web_gizclaw_close(h2_web_gizclaw_t **pointer) {
  if (!pointer)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_gizclaw_t *bridge = *pointer;
  if (!bridge)
    return H2_PAL_OK;
  if (bridge->stopping_ready)
    h2_atomic_bool_store(&bridge->stopping, true, H2_ATOMIC_RELEASE);
  if (bridge->bound) {
    int rc =
        h2_web_main_call(h2_web_gizclaw_detach_js, (const void *[]){&bridge})
            .i32;
    if (rc != H2_PAL_OK)
      return rc;
    bridge->bound = false;
  }
  if (bridge->worker) {
    int rc = h2_pal_task_join(bridge->config.task, bridge->worker);
    if (rc != H2_PAL_OK)
      return rc;
    bridge->worker = NULL;
  }
  if (bridge->commands) {
    h2_pal_queue_destroy(bridge->config.queue, bridge->commands);
    bridge->commands = NULL;
  }
  if (bridge->retained) {
    int rc = h2_gizclaw_local_release(&bridge->config.service);
    if (rc != H2_PAL_OK)
      return rc;
    bridge->retained = false;
  }
  for (size_t i = 0; i < bridge->slots_ready; ++i) {
    if (i < bridge->cancels_ready)
      h2_atomic_bool_destroy(&bridge->calls[i].canceled);
    h2_atomic_bool_destroy(&bridge->calls[i].claimed);
    h2_pal_mem_free(bridge->config.allocator, bridge->calls[i].input);
  }
  if (bridge->stopping_ready)
    h2_atomic_bool_destroy(&bridge->stopping);
  h2_pal_mem_free(bridge->config.allocator, bridge->calls);
  h2_pal_mem_free(bridge->config.allocator, bridge->input);
  h2_pal_mem_free(bridge->config.allocator, bridge);
  *pointer = NULL;
  return H2_PAL_OK;
}
