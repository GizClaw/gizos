#include "h2_gizclaw_lua_stream_internal.h"
#include "h2_gizclaw_task_names.h"
#include "h2_atomic.h"
#include "payload/lua_app.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

/* One borrowed Binary frame is copied before returning to the SDK. The worker
 * keeps this slot occupied until inflation/filesystem writes consume it; a
 * second frame returns WOULD_BLOCK without consuming anything. */
#define LUA_STREAM_CHUNK_MAX 65535u

struct h2_gizclaw_lua_stream {
  h2_gizclaw_config_t config;
  const h2_pal_task_api_t *task_api;
  const h2_pal_sync_api_t *sync;
  h2_pal_task_t *task;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *cond;
  h2_atomic_bool_t stopping, canceled;
  bool atomics_ready, active, closed, eos, ready;
  uint64_t length;
  char sha256[65];
  size_t pending;
  uint8_t bytes[LUA_STREAM_CHUNK_MAX];
  int result;
  h2_lua_app_info_t info;
  gzc_rpc_stream_provider_t provider;
};

static void lock(h2_gizclaw_lua_stream_t *s) {
  (void)h2_pal_mutex_lock(s->sync, s->mutex);
}
static void unlock(h2_gizclaw_lua_stream_t *s) {
  (void)h2_pal_mutex_unlock(s->sync, s->mutex);
}
static void wake(h2_gizclaw_lua_stream_t *s) {
  (void)h2_pal_cond_broadcast(s->sync, s->cond);
}
static int canceled(void *user) {
  h2_gizclaw_lua_stream_t *s = user;
  return h2_atomic_load(&s->stopping) || h2_atomic_load(&s->canceled) ||
         (s->config.cancel_requested &&
          s->config.cancel_requested(s->config.cancel_user));
}
static int sdk_result(int result) {
  if (result == H2_PAL_OK)
    return GZC_OK;
  if (result == H2_PAL_ERR_FORMAT || result == H2_PAL_ERR_INVALID_ARG)
    return GZC_ERR_INVALID_ARGUMENT;
  if (result == H2_PAL_ERR_UNSUPPORTED || result == H2_PAL_ERR_NO_SPACE)
    return GZC_ERR_UNSUPPORTED;
  if (result == H2_PAL_ERR_CLOSED)
    return GZC_ERR_CLOSED;
  return GZC_ERR_RPC;
}

static int backpressure(h2_gizclaw_lua_stream_t *s) {
  /* The SDK may retry from event-stream reads as well as top-level poll.
   * Yield only after consuming nothing: a high-priority poll owner must let
   * the installation worker progress even when both share one CPU. */
  return h2_pal_time_sleep_ms(s->config.time, 1u) == H2_PAL_OK
             ? GZC_ERR_WOULD_BLOCK
             : GZC_ERR_RPC;
}

static void worker(void *user) {
  h2_gizclaw_lua_stream_t *s = user;
  lock(s);
  while (!h2_atomic_load(&s->stopping)) {
    while (!s->active && !h2_atomic_load(&s->stopping))
      (void)h2_pal_cond_wait(s->sync, s->cond, s->mutex, 100u);
    if (!s->active)
      break;
    h2_lua_app_install_t *install = NULL;
    unlock(s);
    int result = h2_lua_app_install_begin(s->config.lua_apps, s->length,
                                          s->sha256, canceled, s, &install);
    lock(s);
    if (result != H2_PAL_OK) {
      s->result = result;
      s->ready = true;
    }
    while (!s->closed && !h2_atomic_load(&s->stopping)) {
      if (!s->ready && s->pending) {
        size_t size = s->pending;
        unlock(s);
        result = h2_lua_app_install_write(install, s->bytes, size);
        if (result != H2_PAL_OK)
          h2_lua_app_install_abort(&install);
        lock(s);
        s->pending = 0;
        if (result != H2_PAL_OK) {
          s->result = result;
          s->ready = true;
        }
      } else if (!s->ready && s->eos) {
        h2_lua_app_info_t info;
        unlock(s);
        result = h2_lua_app_install_finish(&install, &info);
        lock(s);
        s->result = result;
        if (result == H2_PAL_OK)
          s->info = info;
        s->ready = true;
      } else {
        (void)h2_pal_cond_wait(s->sync, s->cond, s->mutex, 100u);
      }
    }
    unlock(s);
    h2_lua_app_install_abort(&install);
    lock(s);
    s->pending = 0;
    s->active = false;
  }
  unlock(s);
}

static int begin(void *user, int method, gzc_str_t metadata,
                 void **out_session) {
  (void)method;
  h2_gizclaw_lua_stream_t *s = user;
  *out_session = NULL;
  gizclaw_rpc_v1_ClientLuaAppInstallStreamRequest request = {0};
  pb_istream_t input =
      pb_istream_from_buffer((const pb_byte_t *)metadata.data, metadata.len);
  if (!pb_decode(&input, gizclaw_rpc_v1_ClientLuaAppInstallStreamRequest_fields,
                 &request) ||
      !request.content_length || request.content_length > 512u * 1024u ||
      strlen(request.sha256) != 64u ||
      strspn(request.sha256, "0123456789abcdefABCDEF") != 64u)
    return GZC_ERR_INVALID_ARGUMENT;
  lock(s);
  if (!s->task || s->active || h2_atomic_load(&s->stopping)) {
    unlock(s);
    return GZC_ERR_UNSUPPORTED;
  }
  s->length = request.content_length;
  strcpy(s->sha256, request.sha256);
  s->active = true;
  s->closed = false;
  s->eos = false;
  s->ready = false;
  s->pending = 0;
  s->result = H2_PAL_OK;
  h2_atomic_store(&s->canceled, false);
  *out_session = s;
  wake(s);
  unlock(s);
  return GZC_OK;
}
static int write_chunk(void *session, const uint8_t *bytes, size_t size) {
  h2_gizclaw_lua_stream_t *s = session;
  if (!bytes || !size || size > sizeof(s->bytes))
    return GZC_ERR_INVALID_ARGUMENT;
  bool interrupted = canceled(s);
  lock(s);
  int result = GZC_OK;
  if (!s->active || s->closed || s->eos || interrupted)
    result = GZC_ERR_CLOSED;
  else if (s->ready)
    result = sdk_result(s->result);
  else if (s->pending)
    result = GZC_ERR_WOULD_BLOCK;
  else {
    memcpy(s->bytes, bytes, size);
    s->pending = size;
    wake(s);
  }
  unlock(s);
  return result == GZC_ERR_WOULD_BLOCK ? backpressure(s) : result;
}
static int finish(void *session, gzc_rpc_provider_respond_fn respond,
                  void *user) {
  h2_gizclaw_lua_stream_t *s = session;
  if (!respond)
    return GZC_ERR_INVALID_ARGUMENT;
  bool interrupted = canceled(s);
  lock(s);
  if (!s->active || s->closed || interrupted) {
    unlock(s);
    return GZC_ERR_CLOSED;
  }
  s->eos = true;
  wake(s);
  if (!s->ready) {
    unlock(s);
    return backpressure(s);
  }
  int result = s->result;
  h2_lua_app_info_t info = s->info;
  unlock(s);
  gzc_rpc_provider_response_t response = {0};
  uint8_t bytes[gizclaw_rpc_v1_ClientLuaAppInstallResponse_size];
  if (result == H2_PAL_OK) {
    gizclaw_rpc_v1_ClientLuaAppInstallResponse value = {.has_app = true};
    strcpy(value.app.app_id, info.app_id);
    strcpy(value.app.version, info.version);
    value.app.has_display_name = info.display_name[0] != 0;
    strcpy(value.app.display_name, info.display_name);
    value.app.has_description = info.description[0] != 0;
    strcpy(value.app.description, info.description);
    pb_ostream_t output = pb_ostream_from_buffer(bytes, sizeof(bytes));
    if (!pb_encode(&output, gizclaw_rpc_v1_ClientLuaAppInstallResponse_fields,
                   &value))
      return GZC_ERR_RPC;
    response.payload = bytes;
    response.payload_len = output.bytes_written;
  } else {
    response.has_error = true;
    response.error_code =
        result == H2_PAL_ERR_NO_SPACE || result == H2_PAL_ERR_UNSUPPORTED
            ? H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED
        : result == H2_PAL_ERR_FORMAT || result == H2_PAL_ERR_INVALID_ARG
            ? H2_GIZCLAW_RPC_ERROR_INVALID_ARGUMENT
        : result == H2_PAL_ERR_BUSY || result == H2_PAL_ERR_NO_MEMORY
            ? H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED
            : H2_GIZCLAW_RPC_ERROR_INTERNAL;
  }
  return respond(user, &response);
}
static void close_stream(void *session, int status) {
  h2_gizclaw_lua_stream_t *s = session;
  lock(s);
  if (status != GZC_OK)
    h2_atomic_store(&s->canceled, true);
  s->closed = true;
  wake(s);
  unlock(s);
  /* The persistent worker retains its session storage until abort/finish has
   * consumed the installer. stop joins that worker before destroy frees it. */
}

int h2_gizclaw_lua_stream_create_internal(const h2_gizclaw_config_t *config,
                                          const h2_pal_task_api_t *task,
                                          const h2_pal_sync_api_t *sync,
                                          h2_gizclaw_lua_stream_t **out) {
  *out = NULL;
  if (!h2_lua_apps_can_install(config->lua_apps))
    return H2_PAL_OK;
  if (!config->time || !config->time->vtable || !config->time->vtable->sleep_ms)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_gizclaw_lua_stream_t *s = h2_pal_mem_alloc(config->allocator, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->config = *config;
  s->task_api = task;
  s->sync = sync;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (h2_atomic_bool_init(&s->stopping, false) != H2_ATOMIC_OK)
    goto fail;
  if (h2_atomic_bool_init(&s->canceled, false) != H2_ATOMIC_OK) {
    h2_atomic_bool_destroy(&s->stopping);
    goto fail;
  }
  s->atomics_ready = true;
  const h2_pal_mutex_config_t mutex = {.name = "lua-install",
                                       .allocator = config->allocator};
  const h2_pal_cond_config_t cond = {.name = "lua-install",
                                     .allocator = config->allocator};
  rc = h2_pal_mutex_create(sync, &mutex, &s->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_pal_cond_create(sync, &cond, &s->cond);
  if (rc != H2_PAL_OK)
    goto fail;
  s->provider =
      (gzc_rpc_stream_provider_t){s, begin, write_chunk, finish, close_stream};
  *out = s;
  return H2_PAL_OK;
fail:
  h2_gizclaw_lua_stream_destroy_internal(s);
  return rc;
}
int h2_gizclaw_lua_stream_start_internal(h2_gizclaw_lua_stream_t *s) {
  if (!s)
    return H2_PAL_OK;
  if (s->task || h2_atomic_load(&s->stopping))
    return H2_PAL_ERR_INVALID_STATE;
  const h2_pal_task_options_t options = {
      .name = H2_GIZCLAW_LUA_INSTALL_TASK_NAME_VALUE, .min_stack_size = 8192u};
  return h2_pal_task_start(s->task_api, &options, worker, s, &s->task);
}
void h2_gizclaw_lua_stream_cancel_internal(h2_gizclaw_lua_stream_t *s) {
  if (!s)
    return;
  h2_atomic_store(&s->stopping, true);
  h2_atomic_store(&s->canceled, true);
  lock(s);
  wake(s);
  unlock(s);
}
int h2_gizclaw_lua_stream_stop_internal(h2_gizclaw_lua_stream_t *s) {
  if (!s)
    return H2_PAL_OK;
  h2_gizclaw_lua_stream_cancel_internal(s);
  if (!s->task)
    return H2_PAL_OK;
  int rc = h2_pal_task_join(s->task_api, s->task);
  if (rc == H2_PAL_OK)
    s->task = NULL;
  return rc;
}
void h2_gizclaw_lua_stream_destroy_internal(h2_gizclaw_lua_stream_t *s) {
  if (!s || s->task)
    return;
  if (s->cond)
    (void)h2_pal_cond_destroy(s->sync, s->cond);
  if (s->mutex)
    (void)h2_pal_mutex_destroy(s->sync, s->mutex);
  if (s->atomics_ready) {
    h2_atomic_bool_destroy(&s->canceled);
    h2_atomic_bool_destroy(&s->stopping);
  }
  h2_pal_mem_free(s->config.allocator, s);
}
const gzc_rpc_stream_provider_t *
h2_gizclaw_lua_stream_provider_internal(h2_gizclaw_lua_stream_t *s) {
  return s ? &s->provider : NULL;
}
