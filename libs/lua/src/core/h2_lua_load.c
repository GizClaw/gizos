#include "h2_lua_internal.h"

#include <string.h>

#include "lauxlib.h"

static h2_lua_vm_result_t copy_error(h2_lua_vm_t *vm,
                                     h2_lua_vm_execution_t *execution,
                                     h2_lua_vm_result_t result) {
  const char *message = lua_tostring(vm->state, -1);
  h2_lua_copy_message(message, execution->error, execution->error_capacity,
                      &execution->error_size);
  lua_settop(vm->state, 0);
  return result;
}

h2_lua_vm_result_t h2_lua_vm_execute_text(h2_lua_vm_t *vm,
                                          const char *chunk_name,
                                          const uint8_t *source,
                                          size_t source_size,
                                          h2_lua_vm_execution_t *execution) {
  int status;
  const char *output;
  size_t output_size;

  if (vm == NULL || chunk_name == NULL || source == NULL || execution == NULL ||
      (execution->output_capacity != 0u && execution->output == NULL) ||
      (execution->error_capacity != 0u && execution->error == NULL)) {
    return H2_LUA_VM_INVALID_ARGUMENT;
  }
  execution->output_size = 0u;
  execution->error_size = 0u;
  if (execution->output_capacity != 0u) {
    execution->output[0] = '\0';
  }
  if (execution->error_capacity != 0u) {
    execution->error[0] = '\0';
  }
  if (source_size > vm->config.source_limit_bytes) {
    return H2_LUA_VM_SOURCE_TOO_LARGE;
  }
  if (memchr(source, '\0', source_size) != NULL) {
    h2_lua_copy_message("embedded NUL is not valid Lua text", execution->error,
                        execution->error_capacity, &execution->error_size);
    return H2_LUA_VM_SYNTAX_ERROR;
  }
  if (source_size != 0u && source[0] == 0x1bu) {
    return H2_LUA_VM_BYTECODE_REJECTED;
  }

  lua_settop(vm->state, 0);
  status = luaL_loadbufferx(vm->state, (const char *)source, source_size,
                            chunk_name, "t");
  if (status != LUA_OK) {
    return copy_error(vm, execution, H2_LUA_VM_SYNTAX_ERROR);
  }
  status = lua_pcall(vm->state, 0, LUA_MULTRET, 0);
  if (status != LUA_OK) {
    return copy_error(vm, execution, H2_LUA_VM_RUNTIME_ERROR);
  }
  if (lua_gettop(vm->state) == 0) {
    return H2_LUA_VM_OK;
  }
  output = lua_tolstring(vm->state, 1, &output_size);
  if (output == NULL) {
    output = luaL_tolstring(vm->state, 1, &output_size);
  }
  if (output_size > vm->config.output_limit_bytes ||
      output_size >= execution->output_capacity) {
    lua_settop(vm->state, 0);
    return H2_LUA_VM_OUTPUT_TOO_LARGE;
  }
  if (output_size != 0u) {
    memcpy(execution->output, output, output_size);
  }
  execution->output[output_size] = '\0';
  execution->output_size = output_size;
  lua_settop(vm->state, 0);
  return H2_LUA_VM_OK;
}
