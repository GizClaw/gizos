#include "h2_lua_vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static h2_lua_vm_t *create_vm(size_t memory_limit) {
  h2_lua_vm_t *vm = NULL;
  h2_lua_vm_config_t config = {
      .memory_limit_bytes = memory_limit,
      .source_limit_bytes = 1024u,
      .output_limit_bytes = 128u,
  };
  assert(h2_lua_vm_create(&config, &vm) == H2_LUA_VM_OK);
  assert(vm != NULL);
  return vm;
}

static h2_lua_vm_result_t execute(h2_lua_vm_t *vm, const uint8_t *source,
                                  size_t source_size, char *output,
                                  size_t output_capacity, char *error,
                                  size_t error_capacity) {
  h2_lua_vm_execution_t execution = {
      .output = output,
      .output_capacity = output_capacity,
      .error = error,
      .error_capacity = error_capacity,
  };
  return h2_lua_vm_execute_text(vm, "@test.lua", source, source_size,
                                &execution);
}

int main(void) {
  static const uint8_t valid[] =
      "return coroutine.isyieldable() == false and 6 * 7";
  static const uint8_t malformed[] = "return )";
  static const uint8_t runtime_error[] = "error('boom')";
  static const uint8_t bytecode[] = {0x1bu, 'L', 'u', 'a'};
  static const uint8_t embedded_nul[] = {'r', 'e', 't', 'u', 'r', 'n', ' ',
                                         '1', '\0', ';', 'r', 'e', 't', 'u',
                                         'r', 'n', ' ', '2'};
  static const uint8_t long_output[] = "return string.rep('x', 129)";
  static const uint8_t safe_debug[] =
      "return type(debug.traceback)=='function' and debug.sethook==nil and "
      "load==nil and string.dump==nil";
  char output[129] = {0};
  char error[128] = {0};
  h2_lua_vm_t *vm = create_vm(1024u * 1024u);

  assert(execute(vm, valid, sizeof(valid) - 1u, output, sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_OK);
  assert(strcmp(output, "42") == 0);
  assert(h2_lua_vm_memory_used(vm) != 0u);
  assert(execute(vm, malformed, sizeof(malformed) - 1u, output, sizeof(output),
                 error, sizeof(error)) == H2_LUA_VM_SYNTAX_ERROR);
  assert(strstr(error, "unexpected") != NULL);
  assert(execute(vm, runtime_error, sizeof(runtime_error) - 1u, output,
                 sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_RUNTIME_ERROR);
  assert(strstr(error, "boom") != NULL);
  assert(execute(vm, bytecode, sizeof(bytecode), output, sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_BYTECODE_REJECTED);
  assert(execute(vm, embedded_nul, sizeof(embedded_nul), output,
                 sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_SYNTAX_ERROR);
  assert(strstr(error, "embedded NUL") != NULL);
  assert(execute(vm, long_output, sizeof(long_output) - 1u, output,
                 sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_OUTPUT_TOO_LARGE);
  assert(execute(vm, safe_debug, sizeof(safe_debug) - 1u, output,
                 sizeof(output), error, sizeof(error)) == H2_LUA_VM_OK);
  assert(strcmp(output, "true") == 0);
  h2_lua_vm_close(vm);

  vm = create_vm(32u * 1024u);
  assert(execute(vm, (const uint8_t *)"local t={} for i=1,10000 do t[i]=i end",
                 strlen("local t={} for i=1,10000 do t[i]=i end"), output,
                 sizeof(output), error,
                 sizeof(error)) == H2_LUA_VM_RUNTIME_ERROR);
  h2_lua_vm_close(vm);
  puts("lua_core_test: PASS");
  return 0;
}
