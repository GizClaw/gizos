#include "h2_web_gizclaw_apps.h"
#include "h2_atomic.h"
#include "h2_web_main_thread.h"

#include <emscripten.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHUNK_BYTES 32768u
enum { NONE, LIST, BEGIN, WRITE, FINISH, ABORT, UNINSTALL };

typedef struct command {
  uint32_t id;
  unsigned method;
  uint32_t size;
  uint32_t total;
} command_t;

struct h2_web_gizclaw_apps {
  h2_web_gizclaw_apps_config_t config;
  h2_pal_queue_t *commands;
  h2_pal_task_t *worker;
  h2_atomic_bool_t stopping;
  h2_atomic_bool_t inflight;
  bool atomics_ready;
  bool inflight_ready;
  bool bound;
  uint8_t input[CHUNK_BYTES + 1u];
  h2_gizclaw_lua_app_info_t items[H2_GIZCLAW_LUA_APPS_MAX];
};

/* Only the browser UI thread reads/writes this ingress pointer. Bind/unbind
 * run on that same thread, and close awaits unbind before freeing anything. */
static h2_web_gizclaw_apps_t *s_current;

EMSCRIPTEN_KEEPALIVE void h2_web_gizclaw_apps_set_current(uintptr_t address) {
  s_current = (h2_web_gizclaw_apps_t *)address;
}

EMSCRIPTEN_KEEPALIVE int h2_web_gizclaw_apps_request(uint32_t id,
                                                     unsigned command,
                                                     uint32_t size,
                                                     uint32_t total) {
  h2_web_gizclaw_apps_t *bridge = s_current;
  if (bridge == NULL)
    return H2_PAL_ERR_CLOSED;
  if (id == 0u || command < LIST || command > UNINSTALL || size > CHUNK_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_CLOSED;
  bool expected = false;
  if (!h2_atomic_bool_compare_exchange(&bridge->inflight, &expected, true,
                                       H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_BUSY;
  bridge->input[size] = 0u;
  const command_t request = {id, command, size, total};
  const int rc = h2_pal_queue_send(bridge->config.queue, bridge->commands,
                                   &request, H2_PAL_QUEUE_NO_WAIT);
  if (rc != H2_PAL_OK)
    h2_atomic_bool_store(&bridge->inflight, false, H2_ATOMIC_RELEASE);
  /* NO_WAIT queue admission can lose a short internal guard race on the UI.
   * No command was accepted in these cases; JS can retry on a later turn. */
  return rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK
             ? H2_PAL_ERR_WOULD_BLOCK
             : rc;
}

/* clang-format off */
EM_JS(void, bind_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion,
    ["pointer", "pointer", "u32", "i32", "i32", "i32", "i32", "i32"], "i32",
    (address, buffer, capacity, closedCode, busyCode, invalidCode, blockedCode, timeoutCode) => {
      if (Module.h2WebLuaAppsState?.attached) return busyCode;
      const state = {address, buffer, capacity, attached: true, closed: false,
                     busy: false, serial: 0, pending: new Map()};
      state.closedSignal = new Promise(resolve => { state.wakeClose = resolve; });
      const error = code => Object.assign(new Error('Lua app operation failed (' + code + ')'),
                                          {name: 'LuaAppError', code});
      const send = (command, bytes = new Uint8Array(), total = 0) => {
        if (state.closed || Module.h2WebLuaAppsState !== state)
          return Promise.reject(error(closedCode));
        const id = state.serial = state.serial % 0x7fffffff + 1;
        return new Promise((resolve, reject) => {
          state.pending.set(id, {command, resolve, reject});
          const deadline = performance.now() + 5000;
          const submit = () => {
            if (!state.pending.has(id)) return;
            if (state.closed) {
              state.pending.delete(id);
              reject(error(closedCode));
              return;
            }
            try {
              HEAPU8.set(bytes, state.buffer);
              const rc = Module._h2_web_gizclaw_apps_request(id, command, bytes.length, total);
              if (rc === blockedCode && performance.now() < deadline) {
                setTimeout(submit, 1);
                return;
              }
              if (rc !== 0) {
                state.pending.delete(id);
                reject(error(rc === blockedCode ? timeoutCode : rc));
              }
            } catch (cause) {
              state.pending.delete(id);
              reject(cause);
            }
          };
          submit();
        });
      };
      const exclusive = async operation => {
        if (state.closed || Module.h2WebLuaAppsState !== state) throw error(closedCode);
        if (state.busy) throw error(busyCode);
        state.busy = true;
        try { return await operation(); }
        finally { state.busy = false; }
      };
      const api = Object.freeze({
        version: 1,
        list: () => exclusive(() => send(1)),
        uninstall: appId => exclusive(() => {
          if (typeof appId !== 'string' || !/^[a-z0-9_-][a-z0-9_.-]{0,31}$/.test(appId))
            throw error(invalidCode);
          return send(6, new TextEncoder().encode(appId));
        }),
        install: (source, options = {}) => exclusive(async () => {
          const blob = typeof Blob !== 'undefined' && source instanceof Blob;
          const bytes = source instanceof Uint8Array ? source :
                        source instanceof ArrayBuffer ? new Uint8Array(source) : null;
          const size = blob ? source.size : bytes?.byteLength;
          const sha = options?.sha256;
          if (!Number.isInteger(size) || size <= 0 || size > 0xffffffff ||
              (sha !== undefined && (typeof sha !== 'string' || !/^[a-fA-F0-9]{64}$/.test(sha))))
            throw error(invalidCode);
          let begun = false;
          try {
            begun = true;
            await send(2, new TextEncoder().encode(sha || ""), size);
            for (let offset = 0; offset < size; offset += capacity) {
              const end = Math.min(offset + capacity, size);
              const chunk = blob ? new Uint8Array(await Promise.race([
                  source.slice(offset, end).arrayBuffer(),
                  state.closedSignal.then(() => { throw error(closedCode); }),
                ])) : bytes.subarray(offset, end);
              if (chunk.byteLength !== end - offset) throw error(invalidCode);
              await send(3, chunk);
            }
            const info = await send(4);
            begun = false;
            return info;
          } finally {
            if (begun) {
              try { await send(5); }
              catch (cleanup) {
                if (!state.closed) throw cleanup;
                // Close owns the canceled stream and joins its Worker.
              }
            }
          }
        }),
      });
      Module.h2WebLuaAppsState = state;
      globalThis.h2WebLuaApps = api;
      Module._h2_web_gizclaw_apps_set_current(address);
      return 0;
    });
});

EM_JS(void, detach_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "i32", "i32"], null,
    (address, closedCode, release) => {
      const state = Module.h2WebLuaAppsState;
      if (state?.address !== address) return;
      Module._h2_web_gizclaw_apps_set_current(0);
      state.closed = true;
      state.wakeClose();
      for (const pending of state.pending.values())
        pending.reject(Object.assign(new Error('Lua app directory closed'),
                                    {name: 'LuaAppError', code: closedCode}));
      state.pending.clear();
      if (release) state.attached = false;
    });
});

EM_JS(void, complete_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion,
    ["pointer", "u32", "i32", "pointer", "u32", "u32", "u32", "u32", "u32", "u32"], null,
    (address, id, rc, items, count, stride, idOffset, versionOffset, nameOffset, descriptionOffset) => {
      const state = Module.h2WebLuaAppsState;
      if (state?.address !== address || state.closed) return;
      const pending = state.pending.get(id);
      if (!pending) return;
      state.pending.delete(id);
      if (rc !== 0) {
        pending.reject(Object.assign(new Error('Lua app operation failed (' + rc + ')'),
                                    {name: 'LuaAppError', code: rc}));
        return;
      }
      try {
        const values = [];
        for (let i = 0; i < count; i++) {
          const base = items + i * stride;
          values.push({app_id: UTF8ToString(base + idOffset),
                       version: UTF8ToString(base + versionOffset),
                       display_name: UTF8ToString(base + nameOffset),
                       description: UTF8ToString(base + descriptionOffset)});
        }
        pending.resolve(pending.command === 1 ? values :
                        pending.command === 4 ? values[0] : undefined);
      } catch (cause) { pending.reject(cause); }
    });
});
/* clang-format on */

static int stopping(void *user) {
  h2_web_gizclaw_apps_t *bridge = user;
  return h2_atomic_bool_load(&bridge->stopping, H2_ATOMIC_ACQUIRE);
}

static void complete(h2_web_gizclaw_apps_t *bridge, uint32_t id, int rc,
                     size_t count) {
  const uint32_t n = (uint32_t)count;
  const uint32_t stride = sizeof(h2_gizclaw_lua_app_info_t);
  const uint32_t offsets[] = {offsetof(h2_gizclaw_lua_app_info_t, app_id),
                              offsetof(h2_gizclaw_lua_app_info_t, version),
                              offsetof(h2_gizclaw_lua_app_info_t, display_name),
                              offsetof(h2_gizclaw_lua_app_info_t, description)};
  (void)h2_web_main_call(complete_js,
                         (const void *[]){&bridge, &id, &rc,
                                          &(const void *){bridge->items}, &n,
                                          &stride, &offsets[0], &offsets[1],
                                          &offsets[2], &offsets[3]});
}

static void worker(void *user) {
  h2_web_gizclaw_apps_t *bridge = user;
  h2_gizclaw_lua_app_install_t *install = NULL;
  for (;;) {
    command_t request = {0};
    int rc = h2_pal_queue_recv(bridge->config.queue, bridge->commands, &request,
                               H2_PAL_QUEUE_WAIT_FOREVER);
    if (rc != H2_PAL_OK || stopping(bridge))
      break;
    const unsigned command = request.method;
    const uint32_t size = request.size;
    size_t count = 0u;
    if (install != NULL && command != WRITE && command != FINISH &&
        command != ABORT)
      rc = H2_PAL_ERR_BUSY;
    else if (command == LIST) {
      rc = h2_gizclaw_lua_app_catalog_list(bridge->config.apps, bridge->items,
                                           H2_GIZCLAW_LUA_APPS_MAX, &count);
    } else if (command == BEGIN) {
      if ((size != 0u && size != 64u) || request.total == 0u)
        rc = H2_PAL_ERR_INVALID_ARG;
      else
        rc = h2_gizclaw_lua_app_install_begin(
            bridge->config.apps, request.total,
            size ? (const char *)bridge->input : NULL, stopping, bridge,
            &install);
    } else if (command == WRITE) {
      rc = install != NULL && size != 0u
               ? h2_gizclaw_lua_app_install_write(install, bridge->input, size)
               : H2_PAL_ERR_INVALID_STATE;
      if (rc != H2_PAL_OK)
        h2_gizclaw_lua_app_install_abort(&install);
    } else if (command == FINISH) {
      rc = install != NULL
               ? h2_gizclaw_lua_app_install_finish(&install, &bridge->items[0])
               : H2_PAL_ERR_INVALID_STATE;
      count = rc == H2_PAL_OK ? 1u : 0u;
    } else if (command == ABORT) {
      h2_gizclaw_lua_app_install_abort(&install);
    } else if (command == UNINSTALL) {
      rc = size > 0u && size <= 32u &&
                   strlen((const char *)bridge->input) == size
               ? h2_gizclaw_lua_app_catalog_uninstall(
                     bridge->config.apps, (const char *)bridge->input)
               : H2_PAL_ERR_INVALID_ARG;
    } else {
      rc = H2_PAL_ERR_INVALID_ARG;
    }
    h2_atomic_bool_store(&bridge->inflight, false, H2_ATOMIC_RELEASE);
    complete(bridge, request.id, rc, count);
  }
  h2_atomic_bool_store(&bridge->stopping, true, H2_ATOMIC_RELEASE);
  h2_gizclaw_lua_app_install_abort(&install);
  const int closed = H2_PAL_ERR_CLOSED, release = 0;
  (void)h2_web_main_call(detach_js,
                         (const void *[]){&bridge, &closed, &release});
}

h2_pal_result_t
h2_web_gizclaw_apps_open(const h2_web_gizclaw_apps_config_t *config,
                         h2_web_gizclaw_apps_t **out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config == NULL || config->apps == NULL || config->allocator == NULL ||
      config->queue == NULL || config->task == NULL ||
      config->queue->vtable == NULL || config->queue->vtable->create == NULL ||
      config->queue->vtable->send == NULL ||
      config->queue->vtable->recv == NULL ||
      config->queue->vtable->close == NULL ||
      config->queue->vtable->destroy == NULL || config->task->vtable == NULL ||
      config->task->vtable->start == NULL ||
      config->task->vtable->join == NULL || config->allocator->vtable == NULL ||
      config->allocator->vtable->alloc == NULL ||
      config->allocator->vtable->free == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_gizclaw_apps_t *bridge =
      h2_pal_mem_alloc(config->allocator, sizeof(*bridge));
  if (bridge == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(bridge, 0, sizeof(*bridge));
  bridge->config = *config;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (h2_atomic_bool_init(&bridge->stopping, false) != H2_ATOMIC_OK)
    goto fail;
  bridge->atomics_ready = true;
  if (h2_atomic_bool_init(&bridge->inflight, false) != H2_ATOMIC_OK)
    goto fail;
  bridge->inflight_ready = true;
  const h2_pal_queue_config_t queue = {.name = "app_host/gizclaw_apps",
                                       .allocator = config->allocator,
                                       .item_size = sizeof(command_t),
                                       .item_count = 1u};
  rc = h2_pal_queue_create(config->queue, &queue, &bridge->commands);
  const h2_pal_task_options_t options = {.name = "$app_host/gizclaw_apps",
                                         .min_stack_size = 65536u};
  if (rc == H2_PAL_OK)
    rc = h2_pal_task_start(config->task, &options, worker, bridge,
                           &bridge->worker);
  if (rc == H2_PAL_OK) {
    const uint32_t capacity = CHUNK_BYTES;
    const int closed = H2_PAL_ERR_CLOSED, busy = H2_PAL_ERR_BUSY;
    const int invalid = H2_PAL_ERR_INVALID_ARG;
    const int blocked = H2_PAL_ERR_WOULD_BLOCK, timeout = H2_PAL_ERR_TIMEOUT;
    rc = h2_web_main_call(bind_js,
                          (const void *[]){&bridge, &(void *){bridge->input},
                                           &capacity, &closed, &busy, &invalid,
                                           &blocked, &timeout})
             .i32;
    bridge->bound = rc == H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) {
    *out = bridge;
    return H2_PAL_OK;
  }
fail:
  *out = bridge;
  const int cleanup = h2_web_gizclaw_apps_close(out);
  return cleanup == H2_PAL_OK ? rc : cleanup;
}

h2_pal_result_t h2_web_gizclaw_apps_close(h2_web_gizclaw_apps_t **out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_gizclaw_apps_t *bridge = *out;
  if (bridge == NULL)
    return H2_PAL_OK;
  if (bridge->atomics_ready)
    h2_atomic_bool_store(&bridge->stopping, true, H2_ATOMIC_RELEASE);
  if (bridge->bound) {
    const int closed = H2_PAL_ERR_CLOSED, release = 0;
    (void)h2_web_main_call(detach_js,
                           (const void *[]){&bridge, &closed, &release});
  }
  if (bridge->worker != NULL) {
    int rc = h2_pal_queue_close(bridge->config.queue, bridge->commands);
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_pal_task_join(bridge->config.task, bridge->worker);
    if (rc != H2_PAL_OK)
      return rc;
    bridge->worker = NULL;
  }
  if (bridge->commands != NULL) {
    h2_pal_queue_destroy(bridge->config.queue, bridge->commands);
    bridge->commands = NULL;
  }
  if (bridge->bound) {
    const int closed = H2_PAL_ERR_CLOSED, release = 1;
    (void)h2_web_main_call(detach_js,
                           (const void *[]){&bridge, &closed, &release});
  }
  if (bridge->atomics_ready)
    h2_atomic_bool_destroy(&bridge->stopping);
  if (bridge->inflight_ready)
    h2_atomic_bool_destroy(&bridge->inflight);
  h2_pal_mem_free(bridge->config.allocator, bridge);
  *out = NULL;
  return H2_PAL_OK;
}
