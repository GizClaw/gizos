#ifndef H2_BLOOMSPEAKER_LUA_H
#define H2_BLOOMSPEAKER_LUA_H

#include "h2_bloomspeaker_controller.h"
#include "h2_runtime.h"

#include <stdatomic.h>

typedef struct h2_bloomspeaker_lua_context {
  h2_runtime_t *runtime;
  h2_bloomspeaker_controller_t *controller;
  _Atomic bool *shutdown_requested;
  int touch_pairing_enabled;
} h2_bloomspeaker_lua_context_t;

int h2_bloomspeaker_lua_open(void *lua_state, void *user);

#endif
