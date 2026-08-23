#ifndef H2_LUA_INTERNAL_H
#define H2_LUA_INTERNAL_H

#include "h2_lua_vm.h"

#include "lua.h"

struct h2_lua_vm {
  lua_State *state;
  h2_lua_vm_config_t config;
  size_t memory_used;
};

void h2_lua_copy_message(const char *message, char *buffer, size_t capacity,
                         size_t *out_size);

#endif
