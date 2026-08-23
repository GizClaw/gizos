#include "h2_lua_internal.h"

#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lualib.h"

static void *default_realloc(void *user, void *ptr, size_t old_size,
                             size_t new_size) {
  (void)user;
  (void)old_size;
  if (new_size == 0u) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, new_size);
}

static void *lua_allocator(void *user, void *ptr, size_t old_size,
                           size_t new_size) {
  h2_lua_vm_t *vm = (h2_lua_vm_t *)user;
  size_t accounted_old_size = ptr == NULL ? 0u : old_size;
  void *next;

  if (new_size > accounted_old_size &&
      new_size - accounted_old_size >
          vm->config.memory_limit_bytes - vm->memory_used) {
    return NULL;
  }

  next = vm->config.realloc_fn(vm->config.allocator_user, ptr,
                               accounted_old_size, new_size);
  if (new_size != 0u && next == NULL) {
    return NULL;
  }

  vm->memory_used -= accounted_old_size;
  vm->memory_used += new_size;
  return next;
}

static int safe_traceback(lua_State *state) {
  const char *message = luaL_optstring(state, 1, NULL);
  int level = (int)luaL_optinteger(state, 2, 1);
  luaL_traceback(state, state, message, level);
  return 1;
}

h2_lua_vm_result_t h2_lua_vm_create(const h2_lua_vm_config_t *config,
                                    h2_lua_vm_t **out_vm) {
  h2_lua_vm_t *vm;
  h2_lua_vm_config_t normalized;

  if (config == NULL || out_vm == NULL) {
    return H2_LUA_VM_INVALID_ARGUMENT;
  }
  *out_vm = NULL;
  normalized = *config;
  if (normalized.realloc_fn == NULL) {
    normalized.realloc_fn = default_realloc;
  }
  if (normalized.memory_limit_bytes == 0u ||
      normalized.source_limit_bytes == 0u ||
      normalized.output_limit_bytes == 0u) {
    return H2_LUA_VM_INVALID_ARGUMENT;
  }

  vm = (h2_lua_vm_t *)normalized.realloc_fn(normalized.allocator_user, NULL, 0u,
                                            sizeof(*vm));
  if (vm == NULL) {
    return H2_LUA_VM_OUT_OF_MEMORY;
  }
  memset(vm, 0, sizeof(*vm));
  vm->config = normalized;
  vm->state = lua_newstate(lua_allocator, vm, 0u);
  if (vm->state == NULL) {
    normalized.realloc_fn(normalized.allocator_user, vm, sizeof(*vm), 0u);
    return H2_LUA_VM_OUT_OF_MEMORY;
  }
  luaL_requiref(vm->state, LUA_GNAME, luaopen_base, 1);
  lua_pop(vm->state, 1);
  lua_pushnil(vm->state);
  lua_setglobal(vm->state, "dofile");
  lua_pushnil(vm->state);
  lua_setglobal(vm->state, "loadfile");
  lua_pushnil(vm->state);
  lua_setglobal(vm->state, "load");
  luaL_requiref(vm->state, LUA_COLIBNAME, luaopen_coroutine, 1);
  lua_pop(vm->state, 1);
  luaL_requiref(vm->state, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(vm->state, 1);
  luaL_requiref(vm->state, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(vm->state, 1);
  lua_getglobal(vm->state, LUA_STRLIBNAME);
  lua_pushnil(vm->state);
  lua_setfield(vm->state, -2, "dump");
  lua_pop(vm->state, 1);
  luaL_requiref(vm->state, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(vm->state, 1);
  luaL_requiref(vm->state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(vm->state, 1);
  lua_createtable(vm->state, 0, 1);
  lua_pushcfunction(vm->state, safe_traceback);
  lua_setfield(vm->state, -2, "traceback");
  lua_setglobal(vm->state, "debug");
  luaL_requiref(vm->state, LUA_LOADLIBNAME, luaopen_package, 1);
  lua_pop(vm->state, 1);
  lua_getglobal(vm->state, "package");
  lua_pushnil(vm->state);
  lua_setfield(vm->state, -2, "loadlib");
  lua_pushliteral(vm->state, "");
  lua_setfield(vm->state, -2, "cpath");
  lua_getfield(vm->state, -1, "searchers");
  lua_pushnil(vm->state);
  lua_seti(vm->state, -2, 2);
  lua_pushnil(vm->state);
  lua_seti(vm->state, -2, 3);
  lua_pushnil(vm->state);
  lua_seti(vm->state, -2, 4);
  lua_pop(vm->state, 1);
  lua_pop(vm->state, 1);
  *out_vm = vm;
  return H2_LUA_VM_OK;
}

void h2_lua_vm_close(h2_lua_vm_t *vm) {
  h2_lua_vm_config_t config;
  if (vm == NULL) {
    return;
  }
  config = vm->config;
  if (vm->state != NULL) {
    lua_close(vm->state);
  }
  config.realloc_fn(config.allocator_user, vm, sizeof(*vm), 0u);
}

size_t h2_lua_vm_memory_used(const h2_lua_vm_t *vm) {
  return vm == NULL ? 0u : vm->memory_used;
}

void h2_lua_copy_message(const char *message, char *buffer, size_t capacity,
                         size_t *out_size) {
  size_t length = message == NULL ? 0u : strlen(message);
  size_t copied = capacity == 0u           ? 0u
                  : length < capacity - 1u ? length
                                           : capacity - 1u;
  if (capacity != 0u && buffer != NULL) {
    if (copied != 0u) {
      memcpy(buffer, message, copied);
    }
    buffer[copied] = '\0';
  }
  if (out_size != NULL) {
    *out_size = copied;
  }
}
