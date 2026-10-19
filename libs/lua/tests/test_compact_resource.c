#include "h2_lua_vm.h"
#include "compact_fixture_generated.h"
#include "original_fixture_generated.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void execute(const uint8_t *source, size_t size, char *error,
                    size_t error_capacity) {
  h2_lua_vm_t *vm = NULL;
  const h2_lua_vm_config_t config = {
      .memory_limit_bytes = 1024u * 1024u,
      .source_limit_bytes = 4096u,
      .output_limit_bytes = 128u,
  };
  assert(h2_lua_vm_create(&config, &vm) == H2_LUA_VM_OK);
  char output[129] = {0};
  h2_lua_vm_execution_t execution = {
      .output = output,
      .output_capacity = sizeof(output),
      .error = error,
      .error_capacity = error_capacity,
  };
  assert(h2_lua_vm_execute_text(vm, "@fixture.lua", source, size,
                               &execution) == H2_LUA_VM_RUNTIME_ERROR);
  assert(strstr(error, "compact fixture complete") != NULL);
  h2_lua_vm_close(vm);
}

int main(void) {
  assert(compact_fixture_size < original_fixture_size);
  char original_error[256] = {0};
  char compact_error[256] = {0};
  execute(original_fixture, original_fixture_size, original_error,
          sizeof(original_error));
  execute(compact_fixture, compact_fixture_size, compact_error,
          sizeof(compact_error));
  assert(strcmp(original_error, compact_error) == 0);
  puts("compact_resource_test: PASS");
  return 0;
}
