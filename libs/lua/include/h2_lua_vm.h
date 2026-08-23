#ifndef H2_LUA_VM_H
#define H2_LUA_VM_H

/**
 * @file h2_lua_vm.h
 * @brief Runtime-independent Lua 5.5 virtual machine contract.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lua_vm h2_lua_vm_t;

typedef void *(*h2_lua_vm_realloc_fn)(void *user, void *ptr, size_t old_size,
                                      size_t new_size);

typedef enum h2_lua_vm_result {
  H2_LUA_VM_OK = 0,
  H2_LUA_VM_INVALID_ARGUMENT,
  H2_LUA_VM_OUT_OF_MEMORY,
  H2_LUA_VM_SOURCE_TOO_LARGE,
  H2_LUA_VM_BYTECODE_REJECTED,
  H2_LUA_VM_SYNTAX_ERROR,
  H2_LUA_VM_RUNTIME_ERROR,
  H2_LUA_VM_OUTPUT_TOO_LARGE,
} h2_lua_vm_result_t;

typedef struct h2_lua_vm_config {
  h2_lua_vm_realloc_fn realloc_fn;
  void *allocator_user;
  size_t memory_limit_bytes;
  size_t source_limit_bytes;
  size_t output_limit_bytes;
} h2_lua_vm_config_t;

typedef struct h2_lua_vm_execution {
  char *output;
  size_t output_capacity;
  size_t output_size;
  char *error;
  size_t error_capacity;
  size_t error_size;
} h2_lua_vm_execution_t;

/** Creates one isolated Lua state and opens the safe standard libraries. */
h2_lua_vm_result_t h2_lua_vm_create(const h2_lua_vm_config_t *config,
                                    h2_lua_vm_t **out_vm);

/** Closes the Lua state and releases all memory through the configured
 * allocator. */
void h2_lua_vm_close(h2_lua_vm_t *vm);

/**
 * Loads and executes one text chunk. Lua bytecode is always rejected.
 *
 * The first returned Lua value, when present, is converted with Lua's normal
 * string conversion and copied into execution->output.
 */
h2_lua_vm_result_t h2_lua_vm_execute_text(h2_lua_vm_t *vm,
                                          const char *chunk_name,
                                          const uint8_t *source,
                                          size_t source_size,
                                          h2_lua_vm_execution_t *execution);

/** Returns the allocator-accounted live byte count for this VM. */
size_t h2_lua_vm_memory_used(const h2_lua_vm_t *vm);

#ifdef __cplusplus
}
#endif

#endif
