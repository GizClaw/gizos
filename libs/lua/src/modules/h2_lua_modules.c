#include "h2_lua_display.h"
#include "h2_f32_math.h"
#include "../runtime/h2_lua_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

static char s_json_null;

static int lua_runtime_print(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  char message[H2_PAL_LOG_MESSAGE_MAX];
  size_t offset = 0u;
  int index;
  for (index = 1; index <= lua_gettop(state); ++index) {
    size_t length = 0u;
    const char *text = luaL_tolstring(state, index, &length);
    size_t separator = index == 1 ? 0u : 1u;
    if (offset + separator >= sizeof(message) ||
        length > sizeof(message) - 1u - offset - separator) {
      lua_pop(state, 1);
      return luaL_error(state, "print output limit reached");
    }
    if (separator != 0u) {
      message[offset++] = '\t';
    }
    memcpy(message + offset, text, length);
    offset += length;
    lua_pop(state, 1);
  }
  message[offset] = '\0';
  h2_pal_result_t result = h2_pal_log_write(job->host->config.runtime->log,
                                            H2_PAL_LOG_INFO, "lua", message);
  if (result != H2_PAL_OK) {
    return luaL_error(state, "Runtime Log write failed: %d", result);
  }
  return 0;
}

static void set_function(lua_State *state, const char *name,
                         lua_CFunction function, h2_lua_job_t *job) {
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

#define H2_LUA_COMPONENT_CACHE_KEY "h2.lua.runtime.components"

static int push_display_proxy(lua_State *state, h2_lua_job_t *job);
static int push_touch_proxy(lua_State *state, h2_lua_job_t *job);
static int push_button_proxy(lua_State *state, h2_lua_job_t *job,
                             h2_runtime_component_id_t component_id);
static int push_audio_proxy(lua_State *state, h2_lua_job_t *job);
static int lua_async_yield(lua_State *state);
static int lua_async_spawn(lua_State *state);
static int lua_async_status(lua_State *state);
static int lua_async_join(lua_State *state);
static int lua_async_cancel(lua_State *state);
static int lua_delay_ms(lua_State *state);

static int lua_runtime_component_get(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer requested_id = luaL_checkinteger(state, 1);
  h2_runtime_component_info_t info;
  int cache_index;
  int result;
  if (requested_id <= 0 || (lua_Unsigned)requested_id > UINT32_MAX) {
    lua_pushnil(state);
    lua_pushliteral(state, "invalid Runtime component id");
    return 2;
  }
  h2_runtime_component_id_t component_id =
      (h2_runtime_component_id_t)requested_id;
  h2_pal_result_t lookup =
      h2_runtime_component_get(job->host->config.runtime, component_id, &info);
  if (lookup != H2_PAL_OK) {
    lua_pushnil(state);
    lua_pushfstring(state, "unknown Runtime component id: %I", requested_id);
    return 2;
  }

  lua_getfield(state, LUA_REGISTRYINDEX, H2_LUA_COMPONENT_CACHE_KEY);
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushvalue(state, -1);
    lua_setfield(state, LUA_REGISTRYINDEX, H2_LUA_COMPONENT_CACHE_KEY);
  }
  cache_index = lua_absindex(state, -1);
  lua_geti(state, cache_index, (lua_Integer)component_id);
  if (!lua_isnil(state, -1)) {
    lua_remove(state, cache_index);
    return 1;
  }
  lua_pop(state, 1);

  switch (info.kind) {
  case H2_RUNTIME_COMPONENT_BUTTON:
    result = push_button_proxy(state, job, component_id);
    break;
  default:
    lua_pushnil(state);
    lua_pushfstring(state, "Runtime component kind %d is unavailable in Lua",
                    (int)info.kind);
    result = 2;
    break;
  }
  if (result == 1) {
    lua_pushvalue(state, -1);
    lua_seti(state, cache_index, (lua_Integer)component_id);
  }
  lua_remove(state, cache_index);
  return result;
}

static uint32_t allocate_callback_token(h2_lua_job_t *job) {
  for (size_t attempt = 0u; attempt <= job->callback_count; ++attempt) {
    uint32_t token = job->next_callback_token++;
    int collision = 0;
    if (job->next_callback_token == 0u) {
      job->next_callback_token = 1u;
    }
    if (token == 0u) {
      continue;
    }
    for (size_t i = 0u; i < job->callback_count; ++i) {
      if (job->callbacks[i].active && job->callbacks[i].token == token) {
        collision = 1;
        break;
      }
    }
    if (!collision) {
      return token;
    }
  }
  return 0u;
}

static int lua_runtime_on(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_runtime_component_id_t component_id =
      (h2_runtime_component_id_t)luaL_checkinteger(state, 1);
  h2_runtime_event_kind_t kind =
      (h2_runtime_event_kind_t)luaL_checkinteger(state, 2);
  h2_runtime_component_info_t component_info;
  luaL_checktype(state, 3, LUA_TFUNCTION);
  if (component_id == H2_RUNTIME_COMPONENT_ID_NONE ||
      (kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN &&
       kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP &&
       kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
       kind != H2_RUNTIME_COMPONENT_EVENT_NFC_STATE &&
       kind != H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE &&
       kind != H2_RUNTIME_COMPONENT_EVENT_ERROR)) {
    return luaL_error(state, "invalid component id or event type");
  }
  if (h2_runtime_component_get(job->host->config.runtime, component_id,
                               &component_info) != H2_PAL_OK) {
    return luaL_error(state, "unknown Runtime component id");
  }
  if (((kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN ||
        kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP ||
        kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION) &&
       component_info.kind != H2_RUNTIME_COMPONENT_BUTTON) ||
      (kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE &&
       component_info.kind != H2_RUNTIME_COMPONENT_NFC_READER) ||
      (kind == H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE &&
       component_info.kind != H2_RUNTIME_COMPONENT_IMU)) {
    return luaL_error(state, "Runtime component does not support event type");
  }
  return h2_lua_push_callback_register(state, job, component_id, (uint32_t)kind,
                                       3);
}

int h2_lua_push_callback_register(lua_State *state, h2_lua_job_t *job,
                                  h2_runtime_component_id_t component_id,
                                  uint32_t kind, int function_index) {
  h2_lua_callback_t *callback;
  size_t callback_index;
  function_index = lua_absindex(state, function_index);
  for (callback_index = 0u; callback_index < job->callback_count;
       ++callback_index) {
    if (!job->callbacks[callback_index].active) {
      break;
    }
  }
  if (callback_index == job->host->config.callback_capacity_per_job) {
    return luaL_error(state, "runtime callback limit reached");
  }
  if (callback_index == job->callback_count) {
    job->callback_count++;
  }
  callback = &job->callbacks[callback_index];
  callback->token = allocate_callback_token(job);
  if (callback->token == 0u) {
    return luaL_error(state, "runtime callback token space exhausted");
  }
  callback->component_id = component_id;
  callback->kind = kind;
  callback->active = 1;
  lua_pushvalue(state, function_index);
  callback->lua_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  lua_pushinteger(state, (lua_Integer)callback->token);
  return 1;
}

static int lua_runtime_off(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer handle = luaL_checkinteger(state, 1);
  h2_lua_callback_t *callback = NULL;
  if (handle <= 0 || (lua_Unsigned)handle > UINT32_MAX) {
    lua_pushboolean(state, 0);
    return 1;
  }
  for (size_t i = 0u; i < job->callback_count; ++i) {
    if (job->callbacks[i].active &&
        job->callbacks[i].token == (uint32_t)handle) {
      callback = &job->callbacks[i];
      break;
    }
  }
  if (callback == NULL) {
    lua_pushboolean(state, 0);
    return 1;
  }
  luaL_unref(state, LUA_REGISTRYINDEX, callback->lua_ref);
  callback->active = 0;
  lua_pushboolean(state, 1);
  return 1;
}

static int open_runtime(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 9);
  set_function(state, "spawn", lua_async_spawn, job);
  set_function(state, "yield", lua_async_yield, job);
  set_function(state, "sleep", lua_delay_ms, job);
  set_function(state, "status", lua_async_status, job);
  set_function(state, "join", lua_async_join, job);
  set_function(state, "cancel", lua_async_cancel, job);
  lua_pushliteral(state, "cooperative");
  lua_setfield(state, -2, "scheduler");

  lua_createtable(state, 0, 3);
  set_function(state, "get", lua_runtime_component_get, job);
  set_function(state, "on", lua_runtime_on, job);
  set_function(state, "off", lua_runtime_off, job);
  lua_setfield(state, -2, "components");

  lua_createtable(state, 0, 10);
#define H2_SET_EVENT(name, kind)                                               \
  lua_pushinteger(state, H2_RUNTIME_COMPONENT_EVENT_##kind);                   \
  lua_setfield(state, -2, name)
  H2_SET_EVENT("BUTTON_ACTION", BUTTON_ACTION);
  H2_SET_EVENT("BUTTON_DOWN", BUTTON_DOWN);
  H2_SET_EVENT("BUTTON_UP", BUTTON_UP);
  H2_SET_EVENT("NFC_STATE", NFC_STATE);
  H2_SET_EVENT("IMU_GESTURE", IMU_GESTURE);
  H2_SET_EVENT("ERROR", ERROR);
#undef H2_SET_EVENT
#define H2_SET_LINK_EVENT(name, kind)                                          \
  lua_pushinteger(state, H2_LUA_LINK_EVENT_##kind);                            \
  lua_setfield(state, -2, name)
  H2_SET_LINK_EVENT("LINK_CONNECTED", CONNECTED);
  H2_SET_LINK_EVENT("LINK_MESSAGE", MESSAGE);
  H2_SET_LINK_EVENT("LINK_DISCONNECTED", DISCONNECTED);
  H2_SET_LINK_EVENT("LINK_ERROR", ERROR);
#undef H2_SET_LINK_EVENT
  lua_setfield(state, -2, "event");
  return 1;
}

static void h2_lua_sleep_timer_callback(void *user, h2_pal_timer_t *timer) {
  h2_lua_task_t *task = user;
  (void)timer;
  if (task != NULL) {
    atomic_store(&task->timer_fired, 1);
    h2_lua_host_wake_job(task->job);
  }
}

static int lua_delay_ms(lua_State *state) {
  h2_lua_task_t *task = h2_lua_current_task(state);
  lua_Integer delay_ms = luaL_checkinteger(state, 1);
  if (task == NULL) {
    return luaL_error(state, "delay must run in a scheduler task");
  }
  if (delay_ms < 0 || delay_ms > UINT32_MAX) {
    return luaL_error(state, "delay_ms is out of range");
  }
  h2_lua_task_timer_destroy(task);
  task->wake_ms = h2_lua_now_ms(task->job->host) + (uint64_t)delay_ms;
  task->state = H2_LUA_TASK_SLEEPING;
  atomic_store(&task->timer_fired, 0);
  if (delay_ms > 0) {
    h2_pal_result_t timer_result;
    timer_result =
        h2_pal_timer_create(task->job->host->config.runtime->timer,
                            &(h2_pal_timer_config_t){
                                .name = "h2-lua-sleep",
                                .period_ms = (uint32_t)delay_ms,
                                .flags = H2_PAL_TIMER_FLAG_AUTO_START,
                                .cb = h2_lua_sleep_timer_callback,
                                .cb_user = task,
                            },
                            &task->timer);
    if (timer_result != H2_PAL_OK && timer_result != H2_PAL_ERR_UNSUPPORTED) {
      task->state = H2_LUA_TASK_READY;
      return luaL_error(state, "sleep timer failed: %d", timer_result);
    }
  }
  return lua_yield(state, 0);
}

static int lua_delay_us(lua_State *state) {
  h2_lua_task_t *task = h2_lua_current_task(state);
  lua_Integer delay_us = luaL_checkinteger(state, 1);
  uint64_t started_us;
  uint64_t now_us;
  h2_pal_result_t result;
  if (task == NULL) {
    return luaL_error(state, "delay must run in a scheduler task");
  }
  if (delay_us < 0 || delay_us > 1000000) {
    return luaL_error(state, "delay_us is out of range");
  }
  result = h2_pal_time_get_monotonic_us(task->job->host->config.runtime->time,
                                        &started_us);
  if (result != H2_PAL_OK) {
    return luaL_error(state, "delay_us requires Runtime microsecond time: %d",
                      result);
  }
  do {
    result = h2_pal_time_get_monotonic_us(task->job->host->config.runtime->time,
                                          &now_us);
    if (result != H2_PAL_OK) {
      return luaL_error(state, "delay_us clock failed: %d", result);
    }
  } while (now_us - started_us < (uint64_t)delay_us);
  return 0;
}

static int lua_async_yield(lua_State *state) {
  h2_lua_task_t *task = h2_lua_current_task(state);
  if (task == NULL) {
    return luaL_error(state, "yield must run in a scheduler task");
  }
  task->state = H2_LUA_TASK_READY;
  return lua_yield(state, 0);
}

static const char *task_state_name(h2_lua_task_state_t state) {
  switch (state) {
  case H2_LUA_TASK_READY:
    return "ready";
  case H2_LUA_TASK_SLEEPING:
    return "waiting";
  case H2_LUA_TASK_JOINING:
    return "waiting";
  case H2_LUA_TASK_CAPABILITY:
    return "waiting";
  case H2_LUA_TASK_DONE:
    return "done";
  case H2_LUA_TASK_FAILED:
    return "failed";
  case H2_LUA_TASK_CANCELLED:
    return "cancelled";
  case H2_LUA_TASK_UNUSED:
    return "unknown";
  }
  return "unknown";
}

static int lua_async_spawn(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t task_id = 0u;
  int argument_count = lua_gettop(state) - 1;
  luaL_checktype(state, 1, LUA_TFUNCTION);
  h2_pal_result_t result =
      h2_lua_spawn_task(job, state, 1, argument_count, &task_id);
  if (result != H2_PAL_OK) {
    return luaL_error(state, "spawn failed: %d", result);
  }
  lua_pushinteger(state, (lua_Integer)task_id);
  return 1;
}

static int lua_async_status(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t task_id = (uint32_t)luaL_checkinteger(state, 1);
  h2_lua_task_t *task = h2_lua_find_task(job, task_id);
  if (task == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "unknown task");
    return 2;
  }
  lua_pushstring(state, task_state_name(task->state));
  return 1;
}

static int push_join_result(lua_State *state, h2_lua_job_t *job,
                            uint32_t task_id) {
  h2_lua_task_t *target = h2_lua_find_task(job, task_id);
  if (target == NULL) {
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "unknown task");
    return 2;
  }
  if (target->state == H2_LUA_TASK_DONE) {
    lua_pushboolean(state, 1);
    lua_pushstring(state, target->message);
    return 2;
  }
  lua_pushboolean(state, 0);
  lua_pushstring(state, target->message[0] == '\0'
                            ? task_state_name(target->state)
                            : target->message);
  return 2;
}

static int lua_async_join_continue(lua_State *state, int status,
                                   lua_KContext context) {
  h2_lua_task_t *current = h2_lua_current_task(state);
  (void)status;
  if (current == NULL) {
    return luaL_error(state, "join lost scheduler task");
  }
  return push_join_result(state, current->job, (uint32_t)context);
}

static int lua_async_join(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_lua_task_t *current = h2_lua_current_task(state);
  uint32_t task_id = (uint32_t)luaL_checkinteger(state, 1);
  h2_lua_task_t *target = h2_lua_find_task(job, task_id);
  if (current == NULL || target == NULL || target == current) {
    return luaL_error(state, "invalid join target");
  }
  if (target->state == H2_LUA_TASK_DONE ||
      target->state == H2_LUA_TASK_FAILED ||
      target->state == H2_LUA_TASK_CANCELLED) {
    return push_join_result(state, job, task_id);
  }
  current->join_task_id = task_id;
  current->state = H2_LUA_TASK_JOINING;
  return lua_yieldk(state, 0, (lua_KContext)task_id, lua_async_join_continue);
}

static int lua_async_cancel(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t task_id = (uint32_t)luaL_checkinteger(state, 1);
  h2_lua_task_t *target = h2_lua_find_task(job, task_id);
  if (target == NULL || target->state == H2_LUA_TASK_DONE ||
      target->state == H2_LUA_TASK_FAILED ||
      target->state == H2_LUA_TASK_CANCELLED) {
    lua_pushboolean(state, 0);
    return 1;
  }
  target->cancel_requested = 1;
  target->state = H2_LUA_TASK_READY;
  lua_pushboolean(state, 1);
  return 1;
}

static int open_delay(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 2);
  set_function(state, "delay_ms", lua_delay_ms, job);
  set_function(state, "delay_us", lua_delay_us, job);
  return 1;
}

static int lua_system_millis(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_pushinteger(state, (lua_Integer)h2_lua_now_ms(job->host));
  return 1;
}

static int lua_system_time(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint64_t wall_ms = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_wall_ms(job->host->config.runtime->time, &wall_ms);
  if (result != H2_PAL_OK) {
    lua_pushnil(state);
    lua_pushinteger(state, result);
    return 2;
  }
  lua_pushinteger(state, (lua_Integer)(wall_ms / 1000u));
  return 1;
}

typedef struct h2_lua_calendar_time {
  int year;
  unsigned month;
  unsigned day;
  unsigned hour;
  unsigned minute;
  unsigned second;
} h2_lua_calendar_time_t;

static h2_lua_calendar_time_t calendar_from_epoch(int64_t epoch_seconds) {
  int64_t days = epoch_seconds / 86400;
  int64_t seconds = epoch_seconds % 86400;
  int64_t shifted;
  int64_t era;
  unsigned day_of_era;
  unsigned year_of_era;
  int year;
  unsigned day_of_year;
  unsigned month_prime;
  h2_lua_calendar_time_t value;
  if (seconds < 0) {
    seconds += 86400;
    days--;
  }
  shifted = days + 719468;
  era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  day_of_era = (unsigned)(shifted - era * 146097);
  year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                 day_of_era / 146096) /
                365;
  year = (int)year_of_era + (int)era * 400;
  day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  month_prime = (5 * day_of_year + 2) / 153;
  value.day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  value.month = month_prime < 10 ? month_prime + 3u : month_prime - 9u;
  value.year = year + (value.month <= 2);
  value.hour = (unsigned)(seconds / 3600);
  value.minute = (unsigned)((seconds % 3600) / 60);
  value.second = (unsigned)(seconds % 60);
  return value;
}

static int append_date_part(char *buffer, size_t capacity, size_t *offset,
                            const char *format, int value) {
  int written;
  if (*offset >= capacity) {
    return 0;
  }
  written = snprintf(buffer + *offset, capacity - *offset, format, value);
  if (written < 0 || (size_t)written >= capacity - *offset) {
    return 0;
  }
  *offset += (size_t)written;
  return 1;
}

static int lua_system_date(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const char *format = luaL_optstring(state, 1, "%Y-%m-%d %H:%M:%S");
  uint64_t wall_ms = 0u;
  int64_t epoch_seconds;
  h2_lua_calendar_time_t value;
  char output[128];
  size_t offset = 0u;
  size_t i;
  h2_pal_result_t result =
      h2_pal_time_get_wall_ms(job->host->config.runtime->time, &wall_ms);
  if (result != H2_PAL_OK) {
    return luaL_error(state, "system clock unavailable: %d", result);
  }
  epoch_seconds = (int64_t)(wall_ms / 1000u) +
                  (int64_t)job->host->config.utc_offset_minutes * 60;
  value = calendar_from_epoch(epoch_seconds);
  for (i = 0u; format[i] != '\0'; ++i) {
    if (offset + 1u >= sizeof(output)) {
      return luaL_error(state, "system.date output limit reached");
    }
    if (format[i] != '%') {
      output[offset++] = format[i];
      continue;
    }
    i++;
    switch (format[i]) {
    case '%':
      output[offset++] = '%';
      break;
    case 'Y':
      if (!append_date_part(output, sizeof(output), &offset, "%04d",
                            value.year))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'm':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.month))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'd':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.day))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'H':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.hour))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'M':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.minute))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'S':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.second))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'F':
      if (!append_date_part(output, sizeof(output), &offset, "%04d",
                            value.year) ||
          offset + 1u >= sizeof(output))
        return luaL_error(state, "system.date output limit reached");
      output[offset++] = '-';
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.month) ||
          offset + 1u >= sizeof(output))
        return luaL_error(state, "system.date output limit reached");
      output[offset++] = '-';
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.day))
        return luaL_error(state, "system.date output limit reached");
      break;
    case 'T':
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.hour) ||
          offset + 1u >= sizeof(output))
        return luaL_error(state, "system.date output limit reached");
      output[offset++] = ':';
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.minute) ||
          offset + 1u >= sizeof(output))
        return luaL_error(state, "system.date output limit reached");
      output[offset++] = ':';
      if (!append_date_part(output, sizeof(output), &offset, "%02d",
                            (int)value.second))
        return luaL_error(state, "system.date output limit reached");
      break;
    case '\0':
      return luaL_error(state, "system.date incomplete format");
    default:
      return luaL_error(state, "system.date unsupported format");
    }
  }
  output[offset] = '\0';
  lua_pushlstring(state, output, offset);
  return 1;
}

static int lua_system_uptime(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_pushinteger(state, (lua_Integer)(h2_lua_now_ms(job->host) / 1000u));
  return 1;
}

static int lua_system_ip(lua_State *state) {
  lua_pushnil(state);
  return 1;
}

static int lua_system_info(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint64_t wall_ms = 0u;
  uint64_t uptime_s = h2_lua_now_ms(job->host) / 1000u;
  h2_pal_result_t result =
      h2_pal_time_get_wall_ms(job->host->config.runtime->time, &wall_ms);
  lua_createtable(state, 0, 3);
  lua_pushinteger(state, (lua_Integer)uptime_s);
  lua_setfield(state, -2, "uptime_s");
  if (result == H2_PAL_OK) {
    int64_t epoch_seconds = (int64_t)(wall_ms / 1000u) +
                            (int64_t)job->host->config.utc_offset_minutes * 60;
    h2_lua_calendar_time_t value = calendar_from_epoch(epoch_seconds);
    char date[32];
    (void)snprintf(date, sizeof(date), "%04d-%02u-%02u %02u:%02u:%02u",
                   value.year, value.month, value.day, value.hour, value.minute,
                   value.second);
    lua_pushinteger(state, (lua_Integer)(wall_ms / 1000u));
    lua_setfield(state, -2, "time");
    lua_pushstring(state, date);
    lua_setfield(state, -2, "date");
  }
  return 1;
}

static int lua_system_heap_unsupported(lua_State *state) {
  return luaL_error(state,
                    "system.heap ESP heap/task introspection is unsupported");
}

static int open_system(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 7);
  set_function(state, "millis", lua_system_millis, job);
  set_function(state, "uptime", lua_system_uptime, job);
  set_function(state, "time", lua_system_time, job);
  set_function(state, "date", lua_system_date, job);
  set_function(state, "ip", lua_system_ip, job);
  set_function(state, "info", lua_system_info, job);
  lua_createtable(state, 0, 4);
  lua_newtable(state);
  lua_setfield(state, -2, "caps");
  set_function(state, "get_info", lua_system_heap_unsupported, job);
  set_function(state, "get_task_watermarks", lua_system_heap_unsupported, job);
  set_function(state, "get_current_task", lua_system_heap_unsupported, job);
  lua_setfield(state, -2, "heap");
  return 1;
}

static void json_escape(luaL_Buffer *buffer, const char *value, size_t length) {
  size_t i;
  luaL_addchar(buffer, '"');
  for (i = 0u; i < length; ++i) {
    unsigned char c = (unsigned char)value[i];
    switch (c) {
    case '"':
      luaL_addstring(buffer, "\\\"");
      break;
    case '\\':
      luaL_addstring(buffer, "\\\\");
      break;
    case '\n':
      luaL_addstring(buffer, "\\n");
      break;
    case '\r':
      luaL_addstring(buffer, "\\r");
      break;
    case '\t':
      luaL_addstring(buffer, "\\t");
      break;
    default:
      if (c < 0x20u) {
        char escaped[7];
        (void)snprintf(escaped, sizeof(escaped), "\\u%04x", c);
        luaL_addstring(buffer, escaped);
      } else {
        luaL_addchar(buffer, (char)c);
      }
      break;
    }
  }
  luaL_addchar(buffer, '"');
}

static int compare_keys(const void *left, const void *right) {
  const char *const *left_key = left;
  const char *const *right_key = right;
  return strcmp(*left_key, *right_key);
}

static void json_encode_value(lua_State *state, int index, luaL_Buffer *buffer,
                              int depth) {
  int type = lua_type(state, index);
  int absolute_index = lua_absindex(state, index);
  if (depth > 16) {
    luaL_error(state, "json nesting limit reached");
  }
  switch (type) {
  case LUA_TNIL:
    luaL_addstring(buffer, "null");
    break;
  case LUA_TBOOLEAN:
    luaL_addstring(buffer, lua_toboolean(state, index) ? "true" : "false");
    break;
  case LUA_TNUMBER: {
    size_t length;
    const char *number;
    if (!isfinite((double)lua_tonumber(state, absolute_index))) {
      luaL_error(state, "json number must be finite");
    }
    lua_pushvalue(state, index);
    number = lua_tolstring(state, -1, &length);
    luaL_addlstring(buffer, number, length);
    lua_pop(state, 1);
    break;
  }
  case LUA_TSTRING: {
    size_t length;
    const char *value = lua_tolstring(state, index, &length);
    json_escape(buffer, value, length);
    break;
  }
  case LUA_TTABLE: {
    lua_Integer length = (lua_Integer)lua_rawlen(state, absolute_index);
    lua_Integer i;
    size_t key_count = 0u;
    int is_array = 1;
    lua_pushnil(state);
    while (lua_next(state, absolute_index) != 0) {
      if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
          lua_tointeger(state, -2) > length) {
        is_array = 0;
      }
      lua_pop(state, 1);
      key_count++;
    }
    if (is_array && key_count == (size_t)length) {
      luaL_addchar(buffer, '[');
      for (i = 1; i <= length; ++i) {
        if (i != 1) {
          luaL_addchar(buffer, ',');
        }
        lua_geti(state, absolute_index, i);
        json_encode_value(state, -1, buffer, depth + 1);
        lua_pop(state, 1);
      }
      luaL_addchar(buffer, ']');
    } else {
      const char **keys =
          lua_newuserdatauv(state, key_count * sizeof(*keys), 0);
      size_t key_index = 0u;
      lua_pushnil(state);
      while (lua_next(state, absolute_index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
          (void)luaL_error(state, "json object keys must be strings");
          return;
        }
        keys[key_index++] = lua_tostring(state, -2);
        lua_pop(state, 1);
      }
      qsort(keys, key_count, sizeof(*keys), compare_keys);
      luaL_addchar(buffer, '{');
      for (key_index = 0u; key_index < key_count; ++key_index) {
        size_t key_length = strlen(keys[key_index]);
        if (key_index != 0u) {
          luaL_addchar(buffer, ',');
        }
        json_escape(buffer, keys[key_index], key_length);
        luaL_addchar(buffer, ':');
        lua_getfield(state, absolute_index, keys[key_index]);
        json_encode_value(state, -1, buffer, depth + 1);
        lua_pop(state, 1);
      }
      luaL_addchar(buffer, '}');
      lua_pop(state, 1);
    }
    break;
  }
  case LUA_TLIGHTUSERDATA:
    if (lua_touserdata(state, absolute_index) != &s_json_null) {
      luaL_error(state, "unsupported json lightuserdata");
    }
    luaL_addstring(buffer, "null");
    break;
  default:
    luaL_error(state, "unsupported json value: %s", lua_typename(state, type));
  }
}

static const char *encode_json_at(lua_State *state, int index,
                                  h2_lua_job_t *job, size_t *out_size) {
  luaL_Buffer buffer;
  luaL_buffinit(state, &buffer);
  json_encode_value(state, index, &buffer, 0);
  luaL_pushresult(&buffer);
  const char *result = lua_tolstring(state, -1, out_size);
  if (*out_size > job->host->config.output_limit_bytes) {
    luaL_error(state, "json output limit reached");
  }
  return result;
}

static int lua_json_encode(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  size_t result_size = 0u;
  (void)encode_json_at(state, 1, job, &result_size);
  return 1;
}

static void *json_malloc(void *context, size_t size) {
  const h2_pal_mem_api_t *mem = context;
  return h2_pal_mem_alloc(mem, size);
}

static void *json_realloc(void *context, void *pointer, size_t old_size,
                          size_t size) {
  const h2_pal_mem_api_t *mem = context;
  (void)old_size;
  return h2_pal_mem_realloc(mem, pointer, size);
}

static void json_free(void *context, void *pointer) {
  const h2_pal_mem_api_t *mem = context;
  h2_pal_mem_free(mem, pointer);
}

static int json_depth_is_valid(yyjson_val *value, int depth) {
  if (depth > 16) {
    return 0;
  }
  if (yyjson_is_arr(value)) {
    size_t index;
    size_t count;
    yyjson_val *item;
    yyjson_arr_foreach(value, index, count, item) {
      if (!json_depth_is_valid(item, depth + 1)) {
        return 0;
      }
    }
  } else if (yyjson_is_obj(value)) {
    size_t index;
    size_t count;
    yyjson_val *key;
    yyjson_val *item;
    yyjson_obj_foreach(value, index, count, key, item) {
      (void)key;
      if (!json_depth_is_valid(item, depth + 1)) {
        return 0;
      }
    }
  }
  return 1;
}

static void push_json_value(lua_State *state, yyjson_val *value, int depth) {
  if (depth > 16) {
    luaL_error(state, "json nesting limit reached");
  }
  if (yyjson_is_null(value)) {
    lua_pushlightuserdata(state, &s_json_null);
  } else if (yyjson_is_bool(value)) {
    lua_pushboolean(state, yyjson_get_bool(value));
  } else if (yyjson_is_uint(value)) {
    uint64_t number = yyjson_get_uint(value);
    if (number <= (uint64_t)LUA_MAXINTEGER) {
      lua_pushinteger(state, (lua_Integer)number);
    } else {
      lua_pushnumber(state, (lua_Number)number);
    }
  } else if (yyjson_is_sint(value)) {
    lua_pushinteger(state, (lua_Integer)yyjson_get_sint(value));
  } else if (yyjson_is_real(value)) {
    lua_pushnumber(state, (lua_Number)yyjson_get_real(value));
  } else if (yyjson_is_str(value)) {
    lua_pushlstring(state, yyjson_get_str(value), yyjson_get_len(value));
  } else if (yyjson_is_arr(value)) {
    size_t index;
    size_t count;
    yyjson_val *item;
    lua_createtable(state, (int)yyjson_get_len(value), 0);
    yyjson_arr_foreach(value, index, count, item) {
      push_json_value(state, item, depth + 1);
      lua_seti(state, -2, (lua_Integer)index + 1);
    }
  } else if (yyjson_is_obj(value)) {
    size_t index;
    size_t count;
    yyjson_val *key;
    yyjson_val *item;
    lua_createtable(state, 0, (int)yyjson_get_len(value));
    yyjson_obj_foreach(value, index, count, key, item) {
      push_json_value(state, item, depth + 1);
      lua_setfield(state, -2, yyjson_get_str(key));
    }
  } else {
    luaL_error(state, "unsupported json token");
  }
}

static int lua_json_decode(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  size_t length;
  const char *value = luaL_checklstring(state, 1, &length);
  yyjson_alc allocator = {
      .malloc = json_malloc,
      .realloc = json_realloc,
      .free = json_free,
      .ctx = (void *)job->host->config.runtime->mem,
  };
  yyjson_read_err error;
  yyjson_doc *document;
  if (length > job->host->config.source_limit_bytes) {
    return luaL_error(state, "json input limit reached");
  }
  document = yyjson_read_opts((char *)(uintptr_t)value, length, 0u, &allocator,
                              &error);
  if (document == NULL) {
    return luaL_error(state, "malformed json at byte %d", (int)error.pos);
  }
  if (!json_depth_is_valid(yyjson_doc_get_root(document), 0)) {
    yyjson_doc_free(document);
    return luaL_error(state, "json nesting limit reached");
  }
  push_json_value(state, yyjson_doc_get_root(document), 0);
  yyjson_doc_free(document);
  return 1;
}

static int open_json(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 3);
  set_function(state, "encode", lua_json_encode, job);
  set_function(state, "decode", lua_json_decode, job);
  lua_pushlightuserdata(state, &s_json_null);
  lua_setfield(state, -2, "null");
  return 1;
}

static int push_capability_tuple(lua_State *state, h2_pal_result_t result,
                                 const char *output, const char *error) {
  lua_pushboolean(state, result == H2_PAL_OK);
  if (result == H2_PAL_OK) {
    lua_pushstring(state, output == NULL ? "" : output);
    lua_pushnil(state);
  } else {
    lua_pushnil(state);
    lua_pushstring(state, error == NULL ? "capability failed" : error);
  }
  return 3;
}

static int lua_capability_continue(lua_State *state, int status,
                                   lua_KContext context) {
  h2_lua_task_t *task = h2_lua_current_task(state);
  h2_lua_host_t *host;
  h2_lua_capability_request_t *request;
  h2_pal_result_t result;
  char output[H2_LUA_CAPABILITY_OUTPUT_MAX];
  char error[H2_LUA_MESSAGE_MAX];
  (void)status;
  if (task == NULL) {
    return luaL_error(state, "capability lost scheduler task");
  }
  host = task->job->host;
  if (host->capability_mutex == NULL ||
      h2_pal_mutex_lock(host->config.runtime->sync, host->capability_mutex) !=
          H2_PAL_OK) {
    return push_capability_tuple(state, H2_PAL_ERR_UNSUPPORTED, NULL,
                                 "capability sync unavailable");
  }
  request = h2_lua_find_capability_request(
      host, (h2_lua_capability_request_id_t)context);
  if (request == NULL ||
      (request->state != H2_LUA_CAPABILITY_REQUEST_COMPLETED &&
       request->state != H2_LUA_CAPABILITY_REQUEST_CANCELLED)) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync,
                              host->capability_mutex);
    return push_capability_tuple(state, H2_PAL_ERR_INVALID_STATE, NULL,
                                 "capability completion missing");
  }
  result = request->state == H2_LUA_CAPABILITY_REQUEST_CANCELLED
               ? H2_PAL_ERR_CLOSED
               : request->result;
  (void)snprintf(output, sizeof(output), "%s", request->output);
  (void)snprintf(error, sizeof(error), "%s",
                 request->state == H2_LUA_CAPABILITY_REQUEST_CANCELLED
                     ? "capability cancelled"
                     : request->error);
  memset(request, 0, sizeof(*request));
  task->capability_request_id = 0u;
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->capability_mutex);
  return push_capability_tuple(state, result, output, error);
}

static h2_lua_capability_request_t *
allocate_capability_request(h2_lua_host_t *host) {
  size_t i;
  for (i = 0u; i < host->config.pending_capability_capacity; ++i) {
    if (host->capability_requests[i].state ==
        H2_LUA_CAPABILITY_REQUEST_UNUSED) {
      return &host->capability_requests[i];
    }
  }
  return NULL;
}

static int lua_capability_call(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_lua_task_t *task = h2_lua_current_task(state);
  const char *name = luaL_checkstring(state, 1);
  const char *input;
  const char *options;
  size_t input_size = 0u;
  size_t options_size = 0u;
  char output[H2_LUA_CAPABILITY_OUTPUT_MAX];
  const char *error = NULL;
  size_t i;
  if (lua_isnoneornil(state, 2)) {
    input = "{}";
  } else if (lua_type(state, 2) == LUA_TSTRING) {
    input = lua_tolstring(state, 2, &input_size);
  } else if (lua_type(state, 2) == LUA_TTABLE) {
    input = encode_json_at(state, 2, job, &input_size);
  } else {
    return luaL_argerror(state, 2, "expected nil, table, or JSON string");
  }
  if (lua_isnoneornil(state, 3)) {
    options = "{}";
  } else if (lua_type(state, 3) == LUA_TTABLE) {
    options = encode_json_at(state, 3, job, &options_size);
  } else {
    return luaL_argerror(state, 3, "expected table");
  }
  (void)input_size;
  (void)options_size;
  for (i = 0u; i < job->host->capability_count; ++i) {
    h2_lua_capability_entry_t *entry = &job->host->capabilities[i];
    if (strcmp(entry->name, name) == 0) {
      h2_lua_capability_request_t *request;
      h2_pal_result_t result;
      int locked = 0;
      if (job->host->capability_mutex != NULL) {
        if (h2_pal_mutex_lock(job->host->config.runtime->sync,
                              job->host->capability_mutex) != H2_PAL_OK) {
          return push_capability_tuple(state, H2_PAL_ERR_BUSY, NULL,
                                       "capability registry busy");
        }
        locked = 1;
      }
      request = allocate_capability_request(job->host);
      if (request == NULL || task == NULL) {
        if (locked) {
          (void)h2_pal_mutex_unlock(job->host->config.runtime->sync,
                                    job->host->capability_mutex);
        }
        return push_capability_tuple(state, H2_PAL_ERR_FULL, NULL,
                                     "capability request limit reached");
      }
      memset(request, 0, sizeof(*request));
      request->id = job->host->next_capability_request_id++;
      if (job->host->next_capability_request_id == 0u) {
        job->host->next_capability_request_id = 1u;
      }
      request->state = H2_LUA_CAPABILITY_REQUEST_PENDING;
      request->job_id = job->id;
      request->job_generation = job->generation;
      request->task_id = task->id;
      request->capability = entry;
      if (locked) {
        (void)h2_pal_mutex_unlock(job->host->config.runtime->sync,
                                  job->host->capability_mutex);
      }
      output[0] = '\0';
      result = entry->call(entry->user, request->id, input, options, output,
                           sizeof(output), &error);
      if (result != H2_PAL_ERR_WOULD_BLOCK) {
        if (locked) {
          (void)h2_pal_mutex_lock(job->host->config.runtime->sync,
                                  job->host->capability_mutex);
        }
        memset(request, 0, sizeof(*request));
        if (locked) {
          (void)h2_pal_mutex_unlock(job->host->config.runtime->sync,
                                    job->host->capability_mutex);
        }
        return push_capability_tuple(state, result, output, error);
      }
      if (job->host->capability_mutex == NULL) {
        if (entry->cancel != NULL) {
          entry->cancel(entry->user, request->id);
        }
        memset(request, 0, sizeof(*request));
        return push_capability_tuple(
            state, H2_PAL_ERR_UNSUPPORTED, NULL,
            "pending capability requires Runtime Sync");
      }
      task->capability_request_id = request->id;
      task->state = H2_LUA_TASK_CAPABILITY;
      return lua_yieldk(state, 0, (lua_KContext)request->id,
                        lua_capability_continue);
    }
  }
  lua_pushboolean(state, 0);
  lua_pushnil(state);
  lua_pushfstring(state, "unknown capability: %s", name);
  return 3;
}

static int open_capability(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 1);
  set_function(state, "call", lua_capability_call, job);
  return 1;
}

static uint16_t rgb_to_rgb565(unsigned r, unsigned g, unsigned b) {
  return (uint16_t)(((r & 0xf8u) << 8u) | ((g & 0xfcu) << 3u) |
                    ((b & 0xf8u) >> 3u));
}

static unsigned check_color_component(lua_State *state, int table_index,
                                      const char *name) {
  lua_getfield(state, table_index, name);
  lua_Integer value = luaL_checkinteger(state, -1);
  lua_pop(state, 1);
  if (value < 0 || value > 255) {
    luaL_error(state, "display color component '%s' must be in [0, 255]", name);
  }
  return (unsigned)value;
}

static uint16_t check_color(lua_State *state, int index) {
  index = lua_absindex(state, index);
  if (lua_istable(state, index)) {
    unsigned r = check_color_component(state, index, "r");
    unsigned g = check_color_component(state, index, "g");
    unsigned b = check_color_component(state, index, "b");
    return rgb_to_rgb565(r, g, b);
  }
  if (lua_type(state, index) == LUA_TSTRING) {
    const char *name = lua_tostring(state, index);
    if (strcmp(name, "white") == 0)
      return rgb_to_rgb565(255u, 255u, 255u);
    if (strcmp(name, "black") == 0)
      return 0u;
    if (strcmp(name, "red") == 0)
      return rgb_to_rgb565(255u, 0u, 0u);
    if (strcmp(name, "green") == 0)
      return rgb_to_rgb565(0u, 128u, 0u);
    if (strcmp(name, "blue") == 0)
      return rgb_to_rgb565(0u, 0u, 255u);
    (void)luaL_error(state, "unknown display color '%s'", name);
    return 0u;
  }
  (void)luaL_argerror(state, index, "display color must be a string or table");
  return 0u;
}

static int check_pixel_number(lua_State *state, int argument) {
  lua_Number value = luaL_checknumber(state, argument);
  if (!isfinite((double)value) || value < (lua_Number)INT_MIN ||
      value > (lua_Number)INT_MAX) {
    luaL_argerror(state, argument, "pixel value is out of range");
  }
  return (int)value;
}

#define H2_LUA_DISPLAY_REGION_META "h2.display.region"

typedef struct display_region_row {
  size_t offset;
  size_t first_run;
  int left, right, run_count;
} display_region_row_t;

typedef struct display_region_run {
  uint16_t left, right;
} display_region_run_t;

typedef struct display_region {
  int width, height, masked;
  uint16_t key;
  size_t pixel_count, run_count;
  /* Rows, compiled runs, pixels, then background damage bytes. */
  display_region_row_t rows[];
} display_region_t;

typedef struct display_presented {
  size_t pixel_count;
  /* Full last-successful frame followed by one comparison byte per tile. */
  uint16_t pixels[];
} display_presented_t;

static size_t display_tile_count(int width, int height) {
  return (size_t)((width + 15) / 16) * (size_t)((height + 15) / 16);
}

static display_region_run_t *display_region_runs(display_region_t *region) {
  return (display_region_run_t *)(region->rows + region->height);
}

static uint16_t *display_region_pixels(display_region_t *region) {
  return (uint16_t *)(display_region_runs(region) + region->run_count);
}

static uint8_t *display_region_damage(display_region_t *region) {
  return (uint8_t *)(display_region_pixels(region) + region->pixel_count);
}

static void display_damage_rect(h2_lua_job_t *job, int left, int top,
                                int right, int bottom) {
  display_region_t *region = job->display_background;
  if (region == NULL)
    return;
  int columns = (region->width + 15) / 16;
  uint8_t *damage = display_region_damage(region);
  for (int y = top / 16; y <= bottom / 16; ++y)
    memset(damage + (size_t)y * columns + left / 16, 1,
           (size_t)(right / 16 - left / 16 + 1));
}

static void display_dirty_full(h2_lua_job_t *job) {
  job->dirty_valid = 1;
  job->dirty_min_x = job->dirty_min_y = 0;
  job->dirty_max_x = job->display_info.width - 1;
  job->dirty_max_y = job->display_info.height - 1;
  display_damage_rect(job, 0, 0, job->dirty_max_x, job->dirty_max_y);
}

static h2_pal_result_t display_open(h2_lua_job_t *job) {
  size_t pixel_count;
  h2_pal_result_t result;
  if (job->display_shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (job->display_open)
    return H2_PAL_OK;
  if (!job->host->config.borrow_display) {
    result =
        (h2_pal_result_t)h2_pal_display_open(job->host->config.runtime->display);
    if (result != H2_PAL_OK)
      return result;
  }
  result = (h2_pal_result_t)h2_pal_display_get_info(
      job->host->config.runtime->display, &job->display_info);
  if (result != H2_PAL_OK || job->display_info.width <= 0 ||
      job->display_info.height <= 0) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result;
  }
  if ((size_t)job->display_info.width >
      SIZE_MAX / (size_t)job->display_info.height) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_SPACE;
  }
  pixel_count =
      (size_t)job->display_info.width * (size_t)job->display_info.height;
  if (pixel_count > SIZE_MAX / sizeof(*job->framebuffer)) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_SPACE;
  }
  job->framebuffer = h2_pal_mem_alloc(job->host->config.runtime->mem,
                                      pixel_count * sizeof(*job->framebuffer));
  if (job->framebuffer == NULL) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(job->framebuffer, 0, pixel_count * sizeof(*job->framebuffer));
  job->display_open = 1;
  job->dirty_valid = 1;
  job->dirty_min_x = 0;
  job->dirty_min_y = 0;
  job->dirty_max_x = job->display_info.width - 1;
  job->dirty_max_y = job->display_info.height - 1;
  return H2_PAL_OK;
}

static void set_pixel(h2_lua_job_t *job, int x, int y, uint16_t color) {
  if (x >= 0 && y >= 0 && x < job->display_info.width &&
      y < job->display_info.height) {
    job->framebuffer[(size_t)y * (size_t)job->display_info.width + (size_t)x] =
        color;
    display_damage_rect(job, x, y, x, y);
    if (job->dirty_valid && job->dirty_min_x == 0 && job->dirty_min_y == 0 &&
        job->dirty_max_x == job->display_info.width - 1 &&
        job->dirty_max_y == job->display_info.height - 1) {
      return;
    }
    if (!job->dirty_valid) {
      job->dirty_valid = 1;
      job->dirty_min_x = x;
      job->dirty_min_y = y;
      job->dirty_max_x = x;
      job->dirty_max_y = y;
    } else {
      if (x < job->dirty_min_x)
        job->dirty_min_x = x;
      if (y < job->dirty_min_y)
        job->dirty_min_y = y;
      if (x > job->dirty_max_x)
        job->dirty_max_x = x;
      if (y > job->dirty_max_y)
        job->dirty_max_y = y;
    }
  }
}

static void mark_dirty_rect(h2_lua_job_t *job, int x, int y, int width,
                            int height) {
  int min_x = x < 0 ? 0 : x;
  int min_y = y < 0 ? 0 : y;
  int max_x = x + width - 1;
  int max_y = y + height - 1;
  if (width <= 0 || height <= 0) {
    return;
  }
  if (max_x >= job->display_info.width) {
    max_x = job->display_info.width - 1;
  }
  if (max_y >= job->display_info.height) {
    max_y = job->display_info.height - 1;
  }
  if (min_x > max_x || min_y > max_y) {
    return;
  }
  display_damage_rect(job, min_x, min_y, max_x, max_y);
  if (!job->dirty_valid) {
    job->dirty_valid = 1;
    job->dirty_min_x = min_x;
    job->dirty_min_y = min_y;
    job->dirty_max_x = max_x;
    job->dirty_max_y = max_y;
    return;
  }
  if (min_x < job->dirty_min_x)
    job->dirty_min_x = min_x;
  if (min_y < job->dirty_min_y)
    job->dirty_min_y = min_y;
  if (max_x > job->dirty_max_x)
    job->dirty_max_x = max_x;
  if (max_y > job->dirty_max_y)
    job->dirty_max_y = max_y;
}

static void fill_span(h2_lua_job_t *job, int y, int min_x, int max_x,
                      uint16_t color) {
  uint16_t *pixels;
  size_t count;
  if (y < 0 || y >= job->display_info.height || max_x < 0 ||
      min_x >= job->display_info.width || min_x > max_x) {
    return;
  }
  if (min_x < 0)
    min_x = 0;
  if (max_x >= job->display_info.width)
    max_x = job->display_info.width - 1;
  pixels = job->framebuffer + (size_t)y * (size_t)job->display_info.width +
           (size_t)min_x;
  count = (size_t)(max_x - min_x + 1);
  while (count >= 4u) {
    pixels[0] = color;
    pixels[1] = color;
    pixels[2] = color;
    pixels[3] = color;
    pixels += 4;
    count -= 4u;
  }
  while (count != 0u) {
    *pixels++ = color;
    --count;
  }
}

static void write_pixel(h2_lua_job_t *job, int x, int y, uint16_t color) {
  if (x >= 0 && y >= 0 && x < job->display_info.width &&
      y < job->display_info.height) {
    job->framebuffer[(size_t)y * (size_t)job->display_info.width + (size_t)x] =
        color;
  }
}

static void blend_pixel(h2_lua_job_t *job, int x, int y, uint16_t color,
                        unsigned alpha) {
  uint16_t *pixel;
  uint16_t background;
  unsigned inverse;
  unsigned red;
  unsigned green;
  unsigned blue;
  if (alpha == 0u || x < 0 || y < 0 || x >= job->display_info.width ||
      y >= job->display_info.height) {
    return;
  }
  if (alpha >= 255u) {
    write_pixel(job, x, y, color);
    return;
  }
  pixel = job->framebuffer + (size_t)y * (size_t)job->display_info.width +
          (size_t)x;
  background = *pixel;
  inverse = 255u - alpha;
  red = (((color >> 11u) & 0x1fu) * alpha +
         ((background >> 11u) & 0x1fu) * inverse + 127u) /
        255u;
  green = (((color >> 5u) & 0x3fu) * alpha +
           ((background >> 5u) & 0x3fu) * inverse + 127u) /
          255u;
  blue =
      ((color & 0x1fu) * alpha + (background & 0x1fu) * inverse + 127u) / 255u;
  *pixel = (uint16_t)((red << 11u) | (green << 5u) | blue);
}

static int rounded_rect_inset(int height, int radius, int row) {
  int dy;
  int extent = 0;
  int64_t radius_squared;
  if (radius == 0 || (row >= radius && row < height - radius))
    return 0;
  dy = row < radius ? radius - row : row - (height - radius - 1);
  radius_squared = (int64_t)radius * radius;
  while (extent < radius &&
         (int64_t)(extent + 1) * (extent + 1) + (int64_t)dy * dy <=
             radius_squared)
    ++extent;
  return radius - extent;
}

static int point_is_bounded(const h2_lua_job_t *job, int x, int y) {
  return (int64_t)x >= -(int64_t)job->display_info.width &&
         (int64_t)x <= (int64_t)job->display_info.width * 2 &&
         (int64_t)y >= -(int64_t)job->display_info.height &&
         (int64_t)y <= (int64_t)job->display_info.height * 2;
}

static int rect_is_bounded(const h2_lua_job_t *job, int x, int y, int width,
                           int height) {
  return width >= 0 && height >= 0 && width <= job->display_info.width &&
         height <= job->display_info.height && point_is_bounded(job, x, y);
}

static void display_clear_pixels(h2_lua_job_t *job, uint16_t color) {
  size_t count;
  uint16_t *pixels = job->framebuffer;
  count = (size_t)job->display_info.width * (size_t)job->display_info.height;
  while (count >= 4u) {
    pixels[0] = color;
    pixels[1] = color;
    pixels[2] = color;
    pixels[3] = color;
    pixels += 4;
    count -= 4u;
  }
  while (count != 0u) {
    *pixels++ = color;
    --count;
  }
  display_dirty_full(job);
}

static int display_clear(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint16_t color = check_color(state, 1);
  if (!job->display_open) {
    return luaL_error(state, "display is not open");
  }
  display_clear_pixels(job, color);
  return 0;
}

static double check_geometry_number(lua_State *state, int index) {
  double value = luaL_checknumber(state, index);
  if (!isfinite(value) || fabs(value) > 100000.0)
    luaL_argerror(state, index, "geometry value is out of range");
  return value;
}

static double optional_geometry_number(lua_State *state, int index,
                                         double fallback) {
  return lua_isnoneornil(state, index) ? fallback
                                      : check_geometry_number(state, index);
}

static void display_check_clip(lua_State *state, h2_lua_job_t *job,
                                 int top_index, int bottom_index,
                                 int *top, int *bottom) {
  lua_Integer first = luaL_optinteger(state, top_index, 0);
  lua_Integer end = luaL_optinteger(state, bottom_index, job->display_info.height);
  /* Validate in Lua's integer width before narrowing on 32-bit targets. */
  if (!job->display_open || first < 0 || first > end ||
      end > job->display_info.height)
    luaL_error(state, "invalid display clip or closed display");
  *top = (int)first;
  *bottom = (int)end;
}

typedef struct display_cached_span {
  int32_t left, right, y, end_y; /* Negative end_y: span; otherwise a line. */
  uint16_t color;
} display_cached_span_t;

typedef struct display_span_cache {
  size_t count, capacity;
  int valid;
  display_cached_span_t spans[];
} display_span_cache_t;

static void display_cache_record(display_span_cache_t *cache, int left,
                                  int right, int y, int end_y, uint16_t color) {
  if (cache == NULL || !cache->valid) return;
  if (cache->count == cache->capacity) cache->valid = 0;
  else cache->spans[cache->count++] =
      (display_cached_span_t){left, right, y, end_y, color};
}

static void display_raster_polygon_rect_capture(h2_lua_job_t *job, const double *x,
                                     const double *y, size_t count,
                                     uint16_t color, double offset,
                                     int top, int bottom, int clip_left,
                                     int clip_right, display_span_cache_t *cache) {
  double intersections[128];
  float edge_x[128], edge_y[128], slope[128], error[128];
  int edge_first[128], edge_end[128];
  for (size_t i = 0; i < count; ++i) {
    size_t j = (i + 1) % count;
    edge_first[i] = (int)ceil(fmin(y[i], y[j]));
    edge_end[i] = (int)ceil(fmax(y[i], y[j]));
    edge_x[i] = (float)x[i];
    edge_y[i] = (float)y[i];
    float dx = (float)x[j] - edge_x[i], dy = (float)y[j] - edge_y[i];
    slope[i] = fabsf(dy) < 1e-5f ? 0 : dx / dy;
    error[i] = fabsf(dy) < 1e-5f ? 1 :
        32 * FLT_EPSILON * (fabsf(edge_x[i]) + fabsf(dx) *
        (1 + (fabsf(edge_y[i]) + job->display_info.height) / fabsf(dy))) + 1e-7f;
  }
  double low = y[0], high = y[0];
  for (size_t i = 1u; i < count; ++i) {
    low = fmin(low, y[i]);
    high = fmax(high, y[i]);
  }
  int first = (int)fmax(top, ceil(low));
  int last = (int)fmin(bottom - 1, floor(high));
  for (int row = first; row <= last; ++row) {
    size_t used = 0u;
    for (size_t i = 0u; i < count; ++i) {
      size_t next = (i + 1u) % count;
      if (row >= edge_first[i] && row < edge_end[i]) {
        double cross;
        if (x[i] == x[next]) cross = x[i];
        else {
          float fast = edge_x[i] + ((float)row - edge_y[i]) * slope[i];
          float fraction = fast - floorf(fast);
          if (error[i] < .25f && fraction > error[i] && fraction < 1 - error[i])
            cross = fast;
          else cross = x[i] + (row - y[i]) * (x[next] - x[i]) / (y[next] - y[i]);
        }
        size_t at = used++;
        while (at > 0u && intersections[at - 1u] > cross) {
          intersections[at] = intersections[at - 1u];
          --at;
        }
        intersections[at] = cross;
      }
    }
    /* Even-odd, half-open edges; inclusive horizontal integer spans. */
    for (size_t i = 0u; i + 1u < used; i += 2u) {
      int left = (int)floor(ceil(intersections[i]) + offset + 0.5);
      int width = (int)(floor(intersections[i + 1u]) -
                        ceil(intersections[i]) + 1);
      if (width > 0) {
        int right = left + width - 1;
        if (left < clip_left) left = clip_left;
        if (right >= clip_right) right = clip_right - 1;
        if (left <= right) {
          fill_span(job, row, left, right, color);
          mark_dirty_rect(job, left, row, right - left + 1, 1);
          display_cache_record(cache, left, right, row, -1, color);
        }
      }
    }
  }
}

static void display_raster_polygon_rect(h2_lua_job_t *job, const double *x,
                                        const double *y, size_t count,
                                        uint16_t color, double offset,
                                        int top, int bottom, int left, int right) {
  display_raster_polygon_rect_capture(job, x, y, count, color, offset,
                                       top, bottom, left, right, NULL);
}

static void display_raster_polygon(h2_lua_job_t *job, const double *x,
                                  const double *y, size_t count, uint16_t color,
                                  double offset, int top, int bottom) {
  display_raster_polygon_rect(job, x, y, count, color, offset, top, bottom,
                             0, job->display_info.width);
}

static int display_fill_polygon(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double x[128], y[128];
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1);
  uint16_t color = check_color(state, 2);
  double offset = optional_geometry_number(state, 3, 0);
  double scale = optional_geometry_number(state, 6, 1);
  int top, bottom;
  display_check_clip(state, job, 4, 5, &top, &bottom);
  if (count < 3u || count > 128u || scale <= 0 || scale > 16)
    return luaL_error(state, "invalid polygon count or scale");
  for (size_t i = 0u; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    x[i] = check_geometry_number(state, -1) * scale;
    lua_pop(state, 1);
    lua_rawgeti(state, -1, 2);
    y[i] = check_geometry_number(state, -1) * scale;
    lua_pop(state, 2);
  }
  display_raster_polygon(job, x, y, count, color, offset, top, bottom);
  return 0;
}

static int display_fill_ellipse(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double x = check_geometry_number(state, 1);
  double y = check_geometry_number(state, 2);
  double rx = check_geometry_number(state, 3);
  double ry = check_geometry_number(state, 4);
  uint16_t color = check_color(state, 5);
  double offset = optional_geometry_number(state, 6, 0);
  int top, bottom;
  display_check_clip(state, job, 7, 8, &top, &bottom);
  if (rx < 0 || ry <= 0 || ry > 2048)
    return luaL_error(state, "invalid ellipse radii");
  for (double local_y = -ry; local_y <= ry; local_y += 1) {
    int row = (int)floor(y + local_y + 0.5);
    if (row < top || row >= bottom)
      continue;
    double extent = rx * sqrt(fmax(0, 1 - local_y * local_y / (ry * ry)));
    int left = (int)floor(x - extent + offset + 0.5);
    int width = (int)floor(2 * extent + 1.5);
    fill_span(job, row, left, left + width - 1, color);
    mark_dirty_rect(job, left, row, width, 1);
  }
  return 0;
}

/* Clip before Bresenham so even very distant command endpoints do bounded
 * work. This helper does not modify the existing draw_line API's bounds. */
static void display_clipped_line_rect_capture(h2_lua_job_t *job, double x, double y,
                                   double end_x, double end_y, uint16_t color,
                                   int top, int bottom, int left, int right,
                                   display_span_cache_t *cache) {
  if (top == bottom || left == right)
    return;
  double dx = end_x - x, dy = end_y - y, low = 0, high = 1;
  /* Entirely visible segments need no software double divisions. Keep the
   * endpoint rounding below identical to the clipping path. */
  if (x < left || end_x < left || x > right - 1 ||
      end_x > right - 1 || y < top || end_y < top ||
      y > bottom - 1 || end_y > bottom - 1) {
    if (dx == 0) {
      if (x < left || x > right - 1)
        return;
    } else {
      double a = (left - x) / dx, b = (right - 1 - x) / dx;
      low = fmax(low, fmin(a, b));
      high = fmin(high, fmax(a, b));
    }
    if (dy == 0) {
      if (y < top || y > bottom - 1)
        return;
    } else {
      double a = (top - y) / dy, b = (bottom - 1 - y) / dy;
      low = fmax(low, fmin(a, b));
      high = fmin(high, fmax(a, b));
    }
  }
  if (low > high)
    return;
  int x0 = (int)floor(x + dx * low + 0.5);
  int y0 = (int)floor(y + dy * low + 0.5);
  int x1 = (int)floor(x + dx * high + 0.5);
  int y1 = (int)floor(y + dy * high + 0.5);
  display_cache_record(cache, x0, x1, y0, y1, color);
  int width = abs(x1 - x0), height = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int64_t error = (int64_t)width - height;
  mark_dirty_rect(job, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                  width + 1, height + 1);
  for (;;) {
    write_pixel(job, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int64_t twice = error * 2;
    if (twice >= -height) { error -= height; x0 += sx; }
    if (twice <= width) { error += width; y0 += sy; }
  }
}

static void display_clipped_line_rect(h2_lua_job_t *job, double x, double y,
                                       double end_x, double end_y, uint16_t color,
                                       int top, int bottom, int left, int right) {
  display_clipped_line_rect_capture(job, x, y, end_x, end_y, color,
                                     top, bottom, left, right, NULL);
}

static void display_cache_replay(h2_lua_job_t *job, display_span_cache_t *cache) {
  for (size_t i = 0; i < cache->count; ++i) {
    const display_cached_span_t *span = &cache->spans[i];
    if (span->end_y < 0) {
      fill_span(job, span->y, span->left, span->right, span->color);
      mark_dirty_rect(job, span->left, span->y, span->right - span->left + 1, 1);
    } else {
      display_clipped_line_rect(job, span->left, span->y, span->right, span->end_y,
          span->color, 0, job->display_info.height, 0, job->display_info.width);
    }
  }
}

static void display_clipped_line(h2_lua_job_t *job, double x, double y,
                                 double end_x, double end_y, uint16_t color,
                                 int top, int bottom) {
  display_clipped_line_rect(job, x, y, end_x, end_y, color, top, bottom,
                           0, job->display_info.width);
}

typedef struct h2_lua_pixel_command {
  int kind, x, y, a, b;
  uint16_t color;
} h2_lua_pixel_command_t;

typedef struct h2_lua_pixel_commands {
  size_t count;
  h2_lua_pixel_command_t commands[];
} h2_lua_pixel_commands_t;

#define H2_LUA_COMMANDS_META "h2.display.pixel_commands"

static int display_compile_commands(lua_State *state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1);
  if (count > 16384u ||
      count > (SIZE_MAX - sizeof(h2_lua_pixel_commands_t)) /
                  sizeof(h2_lua_pixel_command_t))
    return luaL_error(state, "too many pixel commands");
  h2_lua_pixel_commands_t *list = lua_newuserdatauv(
      state, sizeof(*list) + count * sizeof(*list->commands), 0);
  list->count = count;
  for (size_t i = 0u; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    lua_Integer kind = luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (kind != 0 && kind != 1)
      return luaL_error(state, "invalid pixel command kind");
    int values[4];
    for (int j = 0; j < 4; ++j) {
      lua_rawgeti(state, -1, j + 2);
      double value = check_geometry_number(state, -1);
      if (kind == 0 && j >= 2 && value < 0)
        return luaL_error(state, "negative command rectangle size");
      values[j] = (int)value;
      lua_pop(state, 1);
    }
    lua_rawgeti(state, -1, 6);
    uint16_t color = check_color(state, -1);
    lua_pop(state, 2);
    list->commands[i] = (h2_lua_pixel_command_t){
        (int)kind, values[0], values[1], values[2], values[3], color};
  }
  luaL_newmetatable(state, H2_LUA_COMMANDS_META);
  lua_setmetatable(state, -2);
  return 1;
}

static int display_draw_commands(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const h2_lua_pixel_commands_t *list =
      luaL_checkudata(state, 1, H2_LUA_COMMANDS_META);
  int top, bottom;
  double ox = optional_geometry_number(state, 4, 0);
  double oy = optional_geometry_number(state, 5, 0);
  double sx = optional_geometry_number(state, 6, 1);
  double sy = optional_geometry_number(state, 7, sx);
  int recolor = !lua_isnoneornil(state, 8);
  uint16_t ink = recolor ? check_color(state, 8) : 0;
  /* RGB table getters may call Lua, including Display deinit. Validate the
   * acquisition after decoding every argument that can invoke user code. */
  display_check_clip(state, job, 2, 3, &top, &bottom);
  if (sx <= 0 || sy <= 0 || sx > 1000 || sy > 1000)
    return luaL_error(state, "invalid command scale");
  if (top == bottom)
    return 0;
  int transformed = ox != 0 || oy != 0 || sx != 1 || sy != 1;
  /* Compile bounds and scale limits keep every transformed endpoint below
   * 201 million, including rectangle end coordinates, before int narrowing. */
  for (size_t i = 0u; i < list->count; ++i) {
    const h2_lua_pixel_command_t *command = &list->commands[i];
    uint16_t color = recolor ? ink : command->color;
    /* Pixel-aligned retained art does not need repeated double transforms. */
    if (!transformed) {
      if (command->kind == 0) {
        int first = command->y > top ? command->y : top;
        int end = command->y + command->b;
        int last = end < bottom ? end : bottom;
        if (command->a == 0 || last <= first)
          continue;
        for (int row = first; row < last; ++row)
          fill_span(job, row, command->x, command->x + command->a - 1, color);
        mark_dirty_rect(job, command->x, first, command->a, last - first);
      } else {
        display_clipped_line(job, command->x, command->y, command->a,
                             command->b, color, top, bottom);
      }
      continue;
    }
    int x = (int)floor(ox + command->x * sx + 0.5);
    int y = (int)floor(oy + command->y * sy + 0.5);
    if (command->kind == 0) {
      int right = (int)floor(ox + (command->x + command->a) * sx + 0.5);
      int end = (int)floor(oy + (command->y + command->b) * sy + 0.5);
      int first = y > top ? y : top, last = end < bottom ? end : bottom;
      if (right <= x || last <= first)
        continue;
      for (int row = first; row < last; ++row)
        fill_span(job, row, x, right - 1, color);
      mark_dirty_rect(job, x, first, right - x, last - first);
    } else {
      double end_x = floor(ox + command->a * sx + 0.5);
      double end_y = floor(oy + command->b * sy + 0.5);
      display_clipped_line(job, x, y, end_x, end_y, color, top, bottom);
    }
  }
  return 0;
}

/* One representation for Lua tables and allocation-free native updates. */
typedef struct h2_lua_display_mesh {
  size_t vertex_capacity, primitive_capacity;
  size_t vertex_count, primitive_count;
  double matrix[6];
  int grid;
  int positions_valid;
  int spans_valid;
  int span_left, span_right, span_top, span_bottom, span_width, span_height;
  int span_recolor;
  uint16_t span_color;
  double span_offset;
} h2_lua_display_mesh_t;

static const char s_display_mesh_meta = 0;

_Static_assert(_Alignof(h2_lua_display_mesh_t) >=
                   _Alignof(h2_lua_display_vertex_t), "mesh vertex alignment");
_Static_assert(_Alignof(h2_lua_display_vertex_t) >=
                   _Alignof(h2_lua_display_primitive_t), "mesh primitive alignment");

static h2_lua_display_vertex_t *mesh_vertices(h2_lua_display_mesh_t *mesh) {
  return (h2_lua_display_vertex_t *)(mesh + 1);
}

static h2_lua_display_vertex_t *mesh_positions(h2_lua_display_mesh_t *mesh) {
  return mesh_vertices(mesh) + mesh->vertex_capacity;
}

static h2_lua_display_primitive_t *mesh_primitives(h2_lua_display_mesh_t *mesh) {
  return (h2_lua_display_primitive_t *)(mesh_positions(mesh) + mesh->vertex_capacity);
}

static int mesh_data_valid(const h2_lua_display_mesh_data_t *data,
                          size_t vertex_capacity, size_t primitive_capacity) {
  if (data == NULL || data->vertex_count > vertex_capacity ||
      data->primitive_count > primitive_capacity ||
      (data->vertex_count != 0u && data->vertices == NULL) ||
      (data->primitive_count != 0u && data->primitives == NULL))
    return 0;
  for (size_t i = 0u; i < data->vertex_count; ++i) {
    if (!isfinite(data->vertices[i].x) || !isfinite(data->vertices[i].y) ||
        fabs(data->vertices[i].x) > 1000000 || fabs(data->vertices[i].y) > 1000000)
      return 0;
  }
  for (size_t i = 0u; i < data->primitive_count; ++i) {
    const h2_lua_display_primitive_t *p = &data->primitives[i];
    if (p->first > data->vertex_count || p->count > data->vertex_count - p->first)
      return 0;
    if (p->kind == H2_LUA_DISPLAY_POLYGON) {
      if (p->count < 3u || p->count > 128u)
        return 0;
    } else if (p->kind != H2_LUA_DISPLAY_LINE || p->count != 2u) {
      return 0;
    }
  }
  return 1;
}

static void mesh_replace(h2_lua_display_mesh_t *mesh,
                         const h2_lua_display_mesh_data_t *data) {
  if (data->vertex_count != 0u)
    memcpy(mesh_vertices(mesh), data->vertices,
           data->vertex_count * sizeof(*data->vertices));
  if (data->primitive_count != 0u)
    memcpy(mesh_primitives(mesh), data->primitives,
           data->primitive_count * sizeof(*data->primitives));
  mesh->vertex_count = data->vertex_count;
  mesh->primitive_count = data->primitive_count;
  mesh->positions_valid = 0;
  mesh->spans_valid = 0;
}

/* Two spare slots, no allocator or metamethod calls, even for wrong types. */
static h2_lua_display_mesh_t *mesh_test(lua_State *state, int index) {
  int top = lua_gettop(state);
  if (index == 0 || index > top || index < -top ||
      lua_type(state, index) != LUA_TUSERDATA)
    return NULL;
  index = lua_absindex(state, index);
  if (!lua_getmetatable(state, index))
    return NULL;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  int matches = lua_rawequal(state, -1, -2);
  lua_pop(state, 2);
  return matches ? lua_touserdata(state, index) : NULL;
}

static h2_lua_display_mesh_t *mesh_allocate(lua_State *state, size_t vertices,
                                           size_t primitives) {
  if (vertices > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      primitives > H2_LUA_DISPLAY_PRIMITIVE_LIMIT ||
      vertices > (SIZE_MAX - sizeof(h2_lua_display_mesh_t)) /
                     (2u * sizeof(h2_lua_display_vertex_t)))
    luaL_error(state, "invalid mesh capacity");
  size_t size = sizeof(h2_lua_display_mesh_t) +
                vertices * 2u * sizeof(h2_lua_display_vertex_t);
  if (primitives > (SIZE_MAX - size) / sizeof(h2_lua_display_primitive_t))
    luaL_error(state, "mesh capacity overflow");
  size += primitives * sizeof(h2_lua_display_primitive_t);
  h2_lua_display_mesh_t *mesh = lua_newuserdatauv(state, size, 1);
  memset(mesh, 0, sizeof(*mesh));
  mesh->vertex_capacity = vertices;
  mesh->primitive_capacity = primitives;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushliteral(state, "display mesh");
    lua_setfield(state, -2, "__metatable");
    lua_pushvalue(state, -1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  }
  lua_setmetatable(state, -2);
  return mesh;
}

static int mesh_push_protected(lua_State *state) {
  const h2_lua_display_mesh_config_t *config = lua_touserdata(state, 1);
  h2_lua_display_mesh_t *mesh = mesh_allocate(
      state, config->vertex_capacity, config->primitive_capacity);
  mesh_replace(mesh, &config->initial);
  return 1;
}

h2_pal_result_t h2_lua_display_mesh_push(
    void *lua_state, const h2_lua_display_mesh_config_t *config) {
  lua_State *state = lua_state;
  if (state == NULL || config == NULL ||
      config->vertex_capacity > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      config->primitive_capacity > H2_LUA_DISPLAY_PRIMITIVE_LIMIT ||
      !mesh_data_valid(&config->initial, config->vertex_capacity,
                       config->primitive_capacity))
    return H2_PAL_ERR_INVALID_ARG;
  int top = lua_gettop(state);
  if (!lua_checkstack(state, 3))
    return H2_PAL_ERR_NO_MEMORY;
  lua_pushcfunction(state, mesh_push_protected);
  lua_pushlightuserdata(state, (void *)config);
  int result = lua_pcall(state, 1, 1, 0);
  if (result == LUA_OK)
    return H2_PAL_OK;
  lua_settop(state, top);
  return result == LUA_ERRMEM ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_lua_display_mesh_update(
    void *lua_state, int stack_index, const h2_lua_display_mesh_data_t *data) {
  lua_State *state = lua_state;
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_lua_display_mesh_t *mesh = mesh_test(state, stack_index);
  if (mesh == NULL || !mesh_data_valid(data, mesh->vertex_capacity,
                                      mesh->primitive_capacity))
    return H2_PAL_ERR_INVALID_ARG;
  mesh_replace(mesh, data);
  return H2_PAL_OK;
}

static h2_lua_display_mesh_t *mesh_check(lua_State *state, int index) {
  h2_lua_display_mesh_t *mesh = mesh_test(state, index);
  if (mesh == NULL)
    luaL_argerror(state, index, "expected display mesh");
  return mesh;
}

static size_t mesh_table_count(lua_State *state, int index, size_t limit) {
  luaL_checktype(state, index, LUA_TTABLE);
  size_t count = lua_rawlen(state, index);
  if (count > limit)
    luaL_argerror(state, index, "mesh count exceeds capacity");
  return count;
}

static double mesh_number(lua_State *state, int index) {
  double value = luaL_checknumber(state, index);
  if (!isfinite(value) || fabs(value) > 1000000)
    luaL_argerror(state, index, "mesh coordinate out of range");
  return value;
}

static void mesh_decode(lua_State *state, h2_lua_display_mesh_t *mesh,
                         int vertices, int primitives, size_t nv, size_t np) {
  h2_lua_display_vertex_t *v = mesh_vertices(mesh);
  h2_lua_display_primitive_t *p = mesh_primitives(mesh);
  for (size_t i = 0; i < nv; ++i) {
    lua_rawgeti(state, vertices, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    v[i].x = mesh_number(state, -1);
    lua_pop(state, 1);
    lua_rawgeti(state, -1, 2);
    v[i].y = mesh_number(state, -1);
    lua_pop(state, 2);
  }
  for (size_t i = 0; i < np; ++i) {
    lua_rawgeti(state, primitives, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_Integer fields[3];
    for (int j = 0; j < 3; ++j) {
      lua_rawgeti(state, -1, j + 1);
      fields[j] = luaL_checkinteger(state, -1);
      lua_pop(state, 1);
    }
    if ((fields[0] != 0 && fields[0] != 1) || fields[1] < 1 ||
        (lua_Unsigned)fields[1] > nv || fields[2] < 2 || fields[2] > 128)
      luaL_error(state, "invalid mesh primitive");
    lua_rawgeti(state, -1, 4);
    uint16_t color = check_color(state, -1);
    lua_pop(state, 2);
    p[i] = (h2_lua_display_primitive_t){
        (h2_lua_display_primitive_kind_t)fields[0], (size_t)fields[1] - 1u,
        (size_t)fields[2], color};
  }
  h2_lua_display_mesh_data_t data = {v, nv, p, np};
  if (!mesh_data_valid(&data, mesh->vertex_capacity, mesh->primitive_capacity))
    luaL_error(state, "invalid mesh vertex range");
  mesh->vertex_count = nv;
  mesh->primitive_count = np;
}

static int display_compile_mesh(lua_State *state) {
  size_t nv = mesh_table_count(state, 1, H2_LUA_DISPLAY_VERTEX_LIMIT);
  size_t np = mesh_table_count(state, 2, H2_LUA_DISPLAY_PRIMITIVE_LIMIT);
  lua_Integer vc = luaL_optinteger(state, 3, (lua_Integer)nv);
  lua_Integer pc = luaL_optinteger(state, 4, (lua_Integer)np);
  if (vc < (lua_Integer)nv || vc > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      pc < (lua_Integer)np || pc > H2_LUA_DISPLAY_PRIMITIVE_LIMIT)
    return luaL_error(state, "invalid mesh capacity");
  h2_lua_display_mesh_t *mesh = mesh_allocate(state, (size_t)vc, (size_t)pc);
  mesh_decode(state, mesh, 1, 2, nv, np);
  return 1;
}

static int display_update_mesh(lua_State *state) {
  h2_lua_display_mesh_t *mesh = mesh_check(state, 1);
  size_t nv = mesh_table_count(state, 2, mesh->vertex_capacity);
  size_t np = mesh_table_count(state, 3, mesh->primitive_capacity);
  h2_lua_display_mesh_t *staged = mesh_allocate(state, nv, np);
  mesh_decode(state, staged, 2, 3, nv, np);
  h2_lua_display_mesh_data_t data = {
      mesh_vertices(staged), nv, mesh_primitives(staged), np};
  mesh_replace(mesh, &data);
  return 0;
}

/* Raw keys avoid invoking option-table policy during a draw. RGB colors
 * retain their existing getter semantics and are decoded before any writes. */
static void mesh_option(lua_State *state, const char *key) {
  if (lua_isnoneornil(state, 2)) {
    lua_pushnil(state);
  } else {
    lua_pushstring(state, key);
    lua_rawget(state, 2);
  }
}

static lua_Integer mesh_integer_option(lua_State *state, const char *key,
                                       lua_Integer fallback) {
  mesh_option(state, key);
  lua_Integer value = luaL_optinteger(state, -1, fallback);
  lua_pop(state, 1);
  return value;
}

static h2_lua_display_vertex_t mesh_transform(
    h2_lua_display_vertex_t v, const double matrix[6], int grid) {
  h2_lua_display_vertex_t p = {
      (matrix[0] * v.x + matrix[2] * v.y) + matrix[4],
      (matrix[1] * v.x + matrix[3] * v.y) + matrix[5]};
  if (grid != 0) {
    p.x = floor(p.x / grid + 0.5) * grid;
    p.y = floor(p.y / grid + 0.5) * grid;
  }
  return p;
}

static int display_draw_mesh(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_lua_display_mesh_t *mesh = mesh_check(state, 1);
  if (!lua_isnoneornil(state, 2)) luaL_checktype(state, 2, LUA_TTABLE);
  double matrix[6] = {1, 0, 0, 1, 0, 0};
  mesh_option(state, "matrix");
  if (!lua_isnil(state, -1)) {
    luaL_checktype(state, -1, LUA_TTABLE);
    for (int i = 0; i < 6; ++i) {
      lua_rawgeti(state, -1, i + 1);
      matrix[i] = mesh_number(state, -1);
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  lua_Integer grid = mesh_integer_option(state, "grid", 0);
  lua_Integer left = mesh_integer_option(state, "left", 0);
  lua_Integer right = mesh_integer_option(state, "right", job->display_info.width);
  lua_Integer top = mesh_integer_option(state, "top", 0);
  lua_Integer bottom = mesh_integer_option(state, "bottom", job->display_info.height);
  mesh_option(state, "offset_x");
  double offset = optional_geometry_number(state, -1, 0);
  lua_pop(state, 1);
  mesh_option(state, "color");
  int recolor = !lua_isnil(state, -1);
  uint16_t ink = recolor ? check_color(state, -1) : 0;
  lua_pop(state, 1);
  mesh_option(state, "cache");
  if (!lua_isnil(state, -1)) luaL_checktype(state, -1, LUA_TBOOLEAN);
  int retain_spans = lua_toboolean(state, -1);
  lua_pop(state, 1);
  display_span_cache_t *cache = NULL;
  if (retain_spans) {
    lua_getiuservalue(state, 1, 1);
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      cache = lua_newuserdatauv(state, sizeof(*cache) +
          8192u * sizeof(display_cached_span_t), 0);
      cache->capacity = 8192;
      cache->count = 0;
      cache->valid = 0;
      lua_pushvalue(state, -1);
      lua_setiuservalue(state, 1, 1);
      mesh->spans_valid = 0;
    } else cache = lua_touserdata(state, -1);
  }
  if (!job->display_open || grid < 0 || grid > 16 || left < 0 ||
      left > right || right > job->display_info.width || top < 0 ||
      top > bottom || bottom > job->display_info.height)
    return luaL_error(state, "invalid mesh options or closed display");
  h2_lua_display_vertex_t *positions = mesh_positions(mesh);
  const h2_lua_display_vertex_t *vertices = mesh_vertices(mesh);
  int identity = grid == 0 && matrix[0] == 1 && matrix[1] == 0 &&
                 matrix[2] == 0 && matrix[3] == 1 && matrix[4] == 0 &&
                 matrix[5] == 0;
  if (!mesh->positions_valid || mesh->grid != grid ||
      memcmp(mesh->matrix, matrix, sizeof(matrix)) != 0) {
    if (!identity) {
      /* Validate the complete transform before changing cache or pixels. */
      for (size_t i = 0; i < mesh->vertex_count; ++i) {
        h2_lua_display_vertex_t p = mesh_transform(vertices[i], matrix, (int)grid);
        if (!isfinite(p.x) || !isfinite(p.y) || fabs(p.x) > 16000000 ||
            fabs(p.y) > 16000000)
          return luaL_error(state, "transformed mesh coordinate out of range");
      }
      for (size_t i = 0; i < mesh->vertex_count; ++i)
        positions[i] = mesh_transform(vertices[i], matrix, (int)grid);
    }
    memcpy(mesh->matrix, matrix, sizeof(matrix));
    mesh->grid = (int)grid;
    mesh->positions_valid = 1;
    mesh->spans_valid = 0;
  }
  if (top == bottom || left == right) return 0;
  if (cache != NULL) {
    if (mesh->spans_valid && cache->valid && mesh->span_left == left &&
        mesh->span_right == right && mesh->span_top == top &&
        mesh->span_bottom == bottom && mesh->span_width == job->display_info.width &&
        mesh->span_height == job->display_info.height && mesh->span_offset == offset &&
        mesh->span_recolor == recolor && (!recolor || mesh->span_color == ink)) {
      display_cache_replay(job, cache);
      return 0;
    }
    mesh->spans_valid = 0;
    cache->valid = 1;
    cache->count = 0;
    mesh->span_left = (int)left;
    mesh->span_right = (int)right;
    mesh->span_top = (int)top;
    mesh->span_bottom = (int)bottom;
    mesh->span_width = job->display_info.width;
    mesh->span_height = job->display_info.height;
    mesh->span_offset = offset;
    mesh->span_recolor = recolor;
    mesh->span_color = ink;
  }
  /* Exact identity reads the mesh's own validated, bounded vertices, without
   * copying to positions. Select on cache hits too: positions may still hold
   * an older general transform. Signed zeros are raster-equivalent. */
  const h2_lua_display_vertex_t *draw_positions = identity ? vertices : positions;
  const h2_lua_display_primitive_t *primitives = mesh_primitives(mesh);
  for (size_t i = 0; i < mesh->primitive_count; ++i) {
    const h2_lua_display_primitive_t *p = &primitives[i];
    const h2_lua_display_vertex_t *v = draw_positions + p->first;
    uint16_t color = recolor ? ink : p->color;
    if (p->kind == H2_LUA_DISPLAY_LINE) {
      display_clipped_line_rect_capture(job, v[0].x + offset, v[0].y,
                               v[1].x + offset, v[1].y, color,
                               (int)top, (int)bottom, (int)left, (int)right, cache);
    } else {
      double x[128], y[128];
      for (size_t j = 0; j < p->count; ++j) { x[j] = v[j].x; y[j] = v[j].y; }
      display_raster_polygon_rect_capture(job, x, y, p->count, color, offset,
                                 (int)top, (int)bottom, (int)left, (int)right, cache);
    }
  }
  if (cache != NULL) mesh->spans_valid = cache->valid;
  return 0;
}

typedef struct display_stroke_data {
  size_t count;
  double x[256], y[256], width[256];
  uint16_t color[256];
} display_stroke_data_t;

typedef struct display_stroke_cache {
  display_stroke_data_t data;
  double offset;
  int top, bottom, width, height, fast;
  /* Aligned display_span_cache_t follows this header. */
} display_stroke_cache_t;

typedef struct display_stroke_normals {
  size_t count;
  double data[]; /* x, y, lengths, unit-normal-x, unit-normal-y */
} display_stroke_normals_t;

typedef struct display_smooth_scratch {
  size_t bytes;
  uint16_t pixels[];
} display_smooth_scratch_t;

static const char s_stroke_cache_key = 0, s_stroke_normals_key = 0;
#define H2_LUA_STROKE_META "h2.display.stroke"
#define H2_LUA_NORMALS_META "h2.display.normals"

static int display_optional_boolean(lua_State *state, int index) {
  if (!lua_isnoneornil(state, index)) luaL_checktype(state, index, LUA_TBOOLEAN);
  return lua_toboolean(state, index);
}

/* Conservative legacy guard: retain spans in the validation pass; otherwise
 * use double geometry. It never snaps the input path to a pixel grid. */
static int display_stroke_fast_quad(h2_lua_job_t *job, double ax, double ay,
                                     double bx, double by, double width,
                                     int top, int bottom, uint16_t color,
                                     double offset, display_span_cache_t *cache) {
  enum { k_rows = 512 };
  if (bottom - top > k_rows) return 0;
  float left[k_rows], right[k_rows];
  double extent = fmax(fmax(fabs(ax), fabs(ay)), fmax(fabs(bx), fabs(by)));
  if (extent > 4096 || width > 64 || width <= 0) return 0;
  float dx = (float)(bx - ax), dy = (float)(by - ay);
  float length = sqrtf(dx * dx + dy * dy);
  if (length < .02f) return 0;
  float scale = h2_f32_div((float)width * .5f, length);
  float nx = -dy * scale, ny = dx * scale;
  float x[] = {(float)ax + nx, (float)bx + nx, (float)bx - nx, (float)ax - nx};
  float y[] = {(float)ay + ny, (float)by + ny, (float)by - ny, (float)ay - ny};
  float bound = 64 * FLT_EPSILON * ((float)extent + (float)width + 1);
  for (int i = 0; i < 4; ++i) {
    float fraction = y[i] - floorf(y[i]);
    if (fraction <= bound || fraction >= 1 - bound) return 0;
  }
  int first = (int)ceilf(fminf(fminf(y[0], y[1]), fminf(y[2], y[3])));
  int end = (int)ceilf(fmaxf(fmaxf(y[0], y[1]), fmaxf(y[2], y[3])));
  if (first < top) first = top;
  if (end > bottom) end = bottom;
  if (first >= end) return 1;
  for (int row = first; row < end; ++row) {
    left[row - top] = FLT_MAX;
    right[row - top] = -FLT_MAX;
  }
  for (int i = 0; i < 4; ++i) {
    int j = (i + 1) % 4;
    int begin = (int)ceilf(fminf(y[i], y[j]));
    int stop = (int)ceilf(fmaxf(y[i], y[j]));
    if (begin < top) begin = top;
    if (stop > bottom) stop = bottom;
    if (begin >= stop) continue;
    float ex = x[j] - x[i], ey = y[j] - y[i];
    float denominator = fabsf(ey) - 2 * bound;
    if (denominator <= 0) return 0;
    float slope = h2_f32_div(ex, ey);
    float error = 4 * bound * (1 + h2_f32_div(fabsf(ex), denominator)) +
                  16 * FLT_EPSILON * (fabsf(x[i]) + fabsf(ex) + 1);
    if (error >= .25f) return 0;
    for (int row = begin; row < stop; ++row) {
      float hit = x[i] + ((float)row - y[i]) * slope;
      float fraction = hit - floorf(hit);
      if (fraction <= error || fraction >= 1 - error) return 0;
      if (hit < left[row - top]) left[row - top] = hit;
      if (hit > right[row - top]) right[row - top] = hit;
    }
  }
  for (int row = first; row < end; ++row) {
    if (left[row - top] == FLT_MAX) continue;
    int edge = (int)ceilf(left[row - top]);
    int a = (int)floor((double)edge + offset + .5);
    int b = a + (int)floorf(right[row - top]) - edge;
    if (a < 0) a = 0;
    if (b >= job->display_info.width) b = job->display_info.width - 1;
    if (a > b) continue;
    fill_span(job, row, a, b, color);
    mark_dirty_rect(job, a, row, b - a + 1, 1);
    display_cache_record(cache, a, b, row, -1, color);
  }
  return 1;
}

static void display_stroke_simplify(display_stroke_data_t *path, double tolerance) {
  size_t write = 0;
  for (size_t first = 0; first + 1 < path->count;) {
    size_t end = first + 1;
    for (size_t candidate = end + 1; candidate < path->count; ++candidate) {
      if (path->width[candidate - 1] != path->width[first] ||
          path->color[candidate - 1] != path->color[first]) break;
      double dx = path->x[candidate] - path->x[first];
      double dy = path->y[candidate] - path->y[first];
      double length2 = dx * dx + dy * dy;
      if (length2 < 1e-12) break;
      int valid = 1;
      for (size_t j = first + 1; j < candidate; ++j) {
        double px = path->x[j] - path->x[first], py = path->y[j] - path->y[first];
        double dot = px * dx + py * dy, cross = px * dy - py * dx;
        if (dot < 0 || dot > length2 || cross * cross > tolerance * tolerance * length2) {
          valid = 0;
          break;
        }
      }
      if (!valid) break;
      end = candidate;
    }
    path->width[write] = path->width[first];
    path->color[write] = path->color[first];
    path->x[write + 1] = path->x[end];
    path->y[write + 1] = path->y[end];
    ++write;
    first = end;
  }
  path->count = write + 1;
}

static void display_stroke_smooth(lua_State *state, h2_lua_job_t *job,
                                   const display_stroke_data_t *path,
                                   double offset, int top, int bottom) {
  double min_x = job->display_info.width, max_x = 0, min_y = bottom, max_y = top;
  for (size_t i = 0; i + 1 < path->count; ++i) {
    double radius = path->width[i] * .5 + 1;
    min_x = fmin(min_x, fmin(path->x[i], path->x[i+1]) + offset - radius);
    max_x = fmax(max_x, fmax(path->x[i], path->x[i+1]) + offset + radius);
    min_y = fmin(min_y, fmin(path->y[i], path->y[i+1]) - radius);
    max_y = fmax(max_y, fmax(path->y[i], path->y[i+1]) + radius);
  }
  int width = job->display_info.width, height = job->display_info.height;
  int left = (int)fmax(0, fmin(width, floor(min_x)));
  int right = (int)fmax(0, fmin(width, ceil(max_x)));
  int first = (int)fmax(top, fmin(bottom, floor(min_y)));
  int end = (int)fmax(top, fmin(bottom, ceil(max_y)));
  if (right <= left || end <= first) return;
  size_t stride = (size_t)(right - left);
  if ((size_t)(end - first) > SIZE_MAX / stride)
    luaL_error(state, "smooth stroke size overflow");
  size_t count = stride * (size_t)(end - first);
  if (count > (SIZE_MAX - sizeof(display_smooth_scratch_t)) / 3u)
    luaL_error(state, "smooth stroke size overflow");
  size_t bytes = count * 3u;
  display_smooth_scratch_t *scratch = job->display_smooth;
  if (scratch == NULL || scratch->bytes < bytes) {
    scratch = lua_newuserdatauv(state, sizeof(*scratch) + bytes, 0);
    scratch->bytes = bytes;
    int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    if (!job->display_open || job->display_info.width != width ||
        job->display_info.height != height) {
      luaL_unref(state, LUA_REGISTRYINDEX, ref);
      luaL_error(state, "display changed during smooth stroke allocation");
    }
    int old = job->display_smooth_ref;
    job->display_smooth = scratch;
    job->display_smooth_ref = ref;
    if (old > 0) luaL_unref(state, LUA_REGISTRYINDEX, old);
  }
  uint16_t *ink = scratch->pixels;
  uint8_t *coverage = (uint8_t *)(ink + count);
  memset(coverage, 0, count);
  for (size_t i = 0; i + 1 < path->count; ++i) {
    if (path->width[i] <= 0) continue;
    double ax = path->x[i] + offset, ay = path->y[i];
    double bx = path->x[i+1] + offset, by = path->y[i+1];
    double dx = bx - ax, dy = by - ay, length2 = dx*dx + dy*dy;
    double radius = path->width[i] * .5;
    int l = (int)fmax(left, fmin(right, floor(fmin(ax,bx) - radius - 1)));
    int r = (int)fmax(left, fmin(right, ceil(fmax(ax,bx) + radius + 1)));
    int t = (int)fmax(first, fmin(end, floor(fmin(ay,by) - radius - 1)));
    int b = (int)fmax(first, fmin(end, ceil(fmax(ay,by) + radius + 1)));
    int local_float = fmax(fmax(fabs(ax),fabs(ay)),fmax(fabs(bx),fabs(by))) <= 4096;
    float fax = (float)ax, fay = (float)ay, fdx = (float)dx, fdy = (float)dy;
    float length2f = fdx*fdx + fdy*fdy;
    float inverse = length2f > 1e-12f ? h2_f32_div(1, length2f) : 0;
    float edge = (float)radius + .5f, inner = (float)radius - .5f;
    for (int yy = t; yy < b; ++yy) for (int xx = l; xx < r; ++xx) {
      unsigned alpha;
      if (local_float) {
        float px = (float)xx + .5f - fax, py = (float)yy + .5f - fay;
        float u = (px*fdx + py*fdy)*inverse;
        if (u < 0) u = 0; else if (u > 1) u = 1;
        float ex = px - u*fdx, ey = py - u*fdy, squared = ex*ex + ey*ey;
        if (squared >= edge*edge) continue;
        if (inner >= 0 && squared <= inner*inner) alpha = 255;
        else {
          float a = (edge - sqrtf(squared))*255 + .5f;
          alpha = a <= 0 ? 0 : (a >= 255 ? 255 : (unsigned)a);
        }
      } else {
        double u = length2 > 1e-12 ? ((xx+.5-ax)*dx + (yy+.5-ay)*dy)/length2 : 0;
        u = fmax(0, fmin(1, u));
        double ex = xx+.5-ax-u*dx, ey = yy+.5-ay-u*dy;
        alpha = (unsigned)(fmax(0, fmin(1, radius+.5-sqrt(ex*ex+ey*ey)))*255+.5);
      }
      size_t at = (size_t)(yy-first)*stride + (size_t)(xx-left);
      if (alpha > coverage[at]) { coverage[at] = (uint8_t)alpha; ink[at] = path->color[i]; }
    }
  }
  for (int y = first; y < end; ++y) for (int x = left; x < right; ++x) {
    size_t at = (size_t)(y-first)*stride + (size_t)(x-left);
    if (coverage[at]) blend_pixel(job, x, y, ink[at], coverage[at]);
  }
  mark_dirty_rect(job, left, first, right-left, end-first);
}

static int display_stroke_path(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_stroke_data_t path;
  luaL_checktype(state, 1, LUA_TTABLE);
  luaL_checktype(state, 2, LUA_TTABLE);
  path.count = lua_rawlen(state, 1);
  if (path.count < 2 || path.count > 256 || lua_rawlen(state, 2) != path.count-1)
    return luaL_error(state, "invalid stroke point/width count");
  double offset = optional_geometry_number(state, 4, 0);
  double scale = optional_geometry_number(state, 10, 1);
  double tolerance = optional_geometry_number(state, 11, 0);
  int retain = display_optional_boolean(state, 7);
  int fast = display_optional_boolean(state, 8);
  int smooth = display_optional_boolean(state, 9);
  if (scale <= 0 || scale > 16 || tolerance < 0 || tolerance > .25)
    return luaL_error(state, "invalid stroke scale/tolerance");
  int colors = lua_istable(state, 3) && lua_rawlen(state, 3) > 0;
  uint16_t single = colors ? 0 : check_color(state, 3);
  if (colors && lua_rawlen(state, 3) != path.count-1)
    return luaL_error(state, "invalid stroke color count");
  for (size_t i = 0; i < path.count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i+1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1); path.x[i] = check_geometry_number(state, -1); lua_pop(state, 1);
    lua_rawgeti(state, -1, 2); path.y[i] = check_geometry_number(state, -1); lua_pop(state, 2);
    if (i + 1 < path.count) {
      lua_rawgeti(state, 2, (lua_Integer)i+1);
      path.width[i] = check_geometry_number(state, -1);
      lua_pop(state, 1);
      if (path.width[i] < 0 || path.width[i] > 1000)
        return luaL_error(state, "invalid stroke width");
      if (colors) {
        lua_rawgeti(state, 3, (lua_Integer)i+1);
        path.color[i] = check_color(state, -1);
        lua_pop(state, 1);
      } else path.color[i] = single;
      path.width[i] *= scale;
    }
    path.x[i] *= scale;
    path.y[i] *= scale;
  }
  int top, bottom;
  display_check_clip(state, job, 5, 6, &top, &bottom);
  if (smooth) {
    if (tolerance > 0) display_stroke_simplify(&path, tolerance);
    display_stroke_smooth(state, job, &path, offset, top, bottom);
    lua_pushboolean(state, 0); lua_pushinteger(state, 0);
    return 2;
  }
  display_stroke_cache_t *retained = NULL;
  display_span_cache_t *cache = NULL;
  if (retain) {
    lua_rawgetp(state, 2, &s_stroke_cache_key);
    retained = luaL_testudata(state, -1, H2_LUA_STROKE_META);
    if (retained == NULL) {
      lua_pop(state, 1);
      retained = lua_newuserdatauv(state, sizeof(*retained) + sizeof(*cache) +
          2048u*sizeof(display_cached_span_t), 0);
      memset(retained, 0, sizeof(*retained) + sizeof(*cache));
      if (luaL_newmetatable(state, H2_LUA_STROKE_META)) {
        lua_pushliteral(state, "display stroke cache");
        lua_setfield(state, -2, "__metatable");
      }
      lua_setmetatable(state, -2);
      lua_pushvalue(state, -1); lua_rawsetp(state, 2, &s_stroke_cache_key);
    }
    cache = (display_span_cache_t *)(retained + 1);
  }
  display_stroke_normals_t *normals = NULL;
  if (fast) {
    lua_rawgetp(state, 1, &s_stroke_normals_key);
    normals = luaL_testudata(state, -1, H2_LUA_NORMALS_META);
    if (normals == NULL || normals->count != path.count) {
      lua_pop(state, 1);
      normals = lua_newuserdatauv(state, sizeof(*normals) + 5*path.count*sizeof(double), 0);
      normals->count = 0;
      if (luaL_newmetatable(state, H2_LUA_NORMALS_META)) {
        lua_pushliteral(state, "display stroke normals");
        lua_setfield(state, -2, "__metatable");
      }
      lua_setmetatable(state, -2);
      lua_pushvalue(state, -1); lua_rawsetp(state, 1, &s_stroke_normals_key);
    }
  }
  /* All allocation/getters finished; recheck acquisition before cache or draw. */
  display_check_clip(state, job, 5, 6, &top, &bottom);
  if (cache != NULL) {
    const display_stroke_data_t *old = &retained->data;
    if (cache->valid && old->count == path.count && retained->offset == offset &&
        retained->top == top && retained->bottom == bottom && retained->fast == fast &&
        retained->width == job->display_info.width && retained->height == job->display_info.height &&
        !memcmp(old->x, path.x, path.count*sizeof(double)) &&
        !memcmp(old->y, path.y, path.count*sizeof(double)) &&
        !memcmp(old->width, path.width, (path.count-1)*sizeof(double)) &&
        !memcmp(old->color, path.color, (path.count-1)*sizeof(uint16_t))) {
      display_cache_replay(job, cache);
      lua_pushboolean(state, 1); lua_pushinteger(state, 0);
      return 2;
    }
    retained->data.count = path.count;
    memcpy(retained->data.x, path.x, path.count*sizeof(double));
    memcpy(retained->data.y, path.y, path.count*sizeof(double));
    memcpy(retained->data.width, path.width, (path.count-1)*sizeof(double));
    memcpy(retained->data.color, path.color, (path.count-1)*sizeof(uint16_t));
    retained->offset = offset; retained->top = top; retained->bottom = bottom;
    retained->width = job->display_info.width; retained->height = job->display_info.height;
    retained->fast = fast;
    cache->capacity = 2048; cache->count = 0; cache->valid = 1;
  }
  double *lengths = NULL, *normal_x = NULL, *normal_y = NULL;
  if (normals != NULL) {
    lengths = normals->data + 2*path.count;
    normal_x = normals->data + 3*path.count;
    normal_y = normals->data + 4*path.count;
    if (normals->count != path.count || memcmp(normals->data,path.x,path.count*sizeof(double)) ||
        memcmp(normals->data+path.count,path.y,path.count*sizeof(double))) {
      normals->count = path.count;
      memcpy(normals->data,path.x,path.count*sizeof(double));
      memcpy(normals->data+path.count,path.y,path.count*sizeof(double));
      for (size_t i = 0; i + 1 < path.count; ++i) lengths[i] = -1;
    }
  }
  int fast_count = 0;
  for (size_t i = 0; i + 1 < path.count; ++i) {
    double ax = path.x[i], ay = path.y[i], bx = path.x[i+1], by = path.y[i+1];
    double w = path.width[i];
    if (fast && display_stroke_fast_quad(job,ax,ay,bx,by,w,top,bottom,path.color[i],offset,cache)) {
      ++fast_count;
    } else {
      double dx = bx-ax, dy = by-ay, length, ux = 0, uy = 0;
      if (lengths != NULL && lengths[i] >= 0) {
        length = lengths[i]; ux = normal_x[i]; uy = normal_y[i];
      } else {
        length = sqrt(dx*dx + dy*dy);
        if (length >= .01) { ux = -dy/length; uy = dx/length; }
        if (lengths != NULL) { lengths[i] = length; normal_x[i] = ux; normal_y[i] = uy; }
      }
      if (length < .01) {
        int left = (int)floor(ax-w/2+offset+.5), row = (int)floor(ay-w/2+.5);
        int side = (int)floor(w+.5);
        int a = left < 0 ? 0 : left;
        int b = left+side < job->display_info.width ? left+side : job->display_info.width;
        int first = row < top ? top : row, end = row+side < bottom ? row+side : bottom;
        for (int y = first; y < end && a < b; ++y) {
          fill_span(job,y,a,b-1,path.color[i]); mark_dirty_rect(job,a,y,b-a,1);
          display_cache_record(cache,a,b-1,y,-1,path.color[i]);
        }
        continue;
      }
      double nx = ux*w/2, ny = uy*w/2;
      double x[] = {ax+nx,bx+nx,bx-nx,ax-nx}, y[] = {ay+ny,by+ny,by-ny,ay-ny};
      display_raster_polygon_rect_capture(job,x,y,4,path.color[i],offset,top,bottom,
                                           0,job->display_info.width,cache);
    }
    display_clipped_line_rect_capture(job,ax+offset,ay,bx+offset,by,path.color[i],
                                       top,bottom,0,job->display_info.width,cache);
  }
  lua_pushboolean(state, 0); lua_pushinteger(state, fast_count);
  return 2;
}

static int display_fill_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  uint16_t color = check_color(state, 5);
  int py;
  if (!job->display_open || !rect_is_bounded(job, x, y, width, height)) {
    return luaL_error(state, "invalid fill_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = y; py < y + height; ++py)
    fill_span(job, py, x, x + width - 1, color);
  return 0;
}

static int display_draw_line(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x0 = check_pixel_number(state, 1);
  int y0 = check_pixel_number(state, 2);
  int x1 = check_pixel_number(state, 3);
  int y1 = check_pixel_number(state, 4);
  uint16_t color = check_color(state, 5);
  int64_t dx;
  int64_t dy;
  int64_t error;
  int sx;
  int sy;
  if (!job->display_open || !point_is_bounded(job, x0, y0) ||
      !point_is_bounded(job, x1, y1)) {
    return luaL_error(state, "invalid draw_line");
  }
  dx = llabs((int64_t)x1 - x0);
  sx = x0 < x1 ? 1 : -1;
  dy = -llabs((int64_t)y1 - y0);
  sy = y0 < y1 ? 1 : -1;
  mark_dirty_rect(job, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (int)dx + 1,
                  (int)(-dy) + 1);
  error = dx + dy;
  for (;;) {
    int64_t twice;
    write_pixel(job, x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    twice = 2 * error;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
  }
  return 0;
}

static int display_fill_circle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  int extent = 0;
  int y;
  int64_t radius_squared;
  if (!job->display_open || radius < 0 || radius > job->display_info.width ||
      radius > job->display_info.height || !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid fill_circle");
  }
  mark_dirty_rect(job, cx - radius, cy - radius, radius * 2 + 1,
                  radius * 2 + 1);
  radius_squared = (int64_t)radius * radius;
  for (y = -radius; y <= radius; ++y) {
    while (extent < radius &&
           (int64_t)(extent + 1) * (extent + 1) + (int64_t)y * y <=
               radius_squared)
      ++extent;
    while (extent > 0 &&
           (int64_t)extent * extent + (int64_t)y * y > radius_squared)
      --extent;
    fill_span(job, cy + y, cx - extent, cx + extent, color);
  }
  return 0;
}

static int display_draw_circle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  int x;
  int y;
  int error;
  if (!job->display_open || radius < 0 || radius > job->display_info.width ||
      radius > job->display_info.height || !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid draw_circle");
  }
  mark_dirty_rect(job, cx - radius, cy - radius, radius * 2 + 1,
                  radius * 2 + 1);
  x = radius;
  y = 0;
  error = 1 - radius;
  while (x >= y) {
    write_pixel(job, cx + x, cy + y, color);
    write_pixel(job, cx + y, cy + x, color);
    write_pixel(job, cx - y, cy + x, color);
    write_pixel(job, cx - x, cy + y, color);
    write_pixel(job, cx - x, cy - y, color);
    write_pixel(job, cx - y, cy - x, color);
    write_pixel(job, cx + y, cy - x, color);
    write_pixel(job, cx + x, cy - y, color);
    ++y;
    if (error < 0) {
      error += 2 * y + 1;
    } else {
      --x;
      error += 2 * (y - x) + 1;
    }
  }
  return 0;
}

static void draw_circle_aa_pixels(h2_lua_job_t *job, int cx, int cy,
                                  int radius, uint16_t color) {
  int radius_q4 = radius * 4 + 2;
  int radius_squared_q8 = radius_q4 * radius_q4;
  int extent = radius + 1;
  int y;
  for (y = cy - extent; y <= cy + extent; ++y) {
    int dy0_q4 = (y - cy) * 4 - 1;
    int dy1_q4 = dy0_q4 + 2;
    int dy0_squared_q8 = dy0_q4 * dy0_q4;
    int dy1_squared_q8 = dy1_q4 * dy1_q4;
    int x;
    for (x = cx - extent; x <= cx + extent; ++x) {
      int dx0_q4 = (x - cx) * 4 - 1;
      int dx1_q4 = dx0_q4 + 2;
      int dx0_squared_q8 = dx0_q4 * dx0_q4;
      int dx1_squared_q8 = dx1_q4 * dx1_q4;
      unsigned coverage =
          (unsigned)(dx0_squared_q8 + dy0_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx1_squared_q8 + dy0_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx0_squared_q8 + dy1_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx1_squared_q8 + dy1_squared_q8 <= radius_squared_q8);
      if (coverage != 0u) {
        blend_pixel(job, x, y, color, coverage * 255u / 4u);
      }
    }
  }
}

static int display_fill_circle_aa(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  if (!job->display_open || radius < 0 || radius > 64 ||
      !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid fill_circle_aa");
  }
  mark_dirty_rect(job, cx - radius - 1, cy - radius - 1, radius * 2 + 3,
                  radius * 2 + 3);
  draw_circle_aa_pixels(job, cx, cy, radius, color);
  return 0;
}

static uint16_t fade_rgb565_to_black(uint16_t color,
                                     const uint16_t red_lut[32],
                                     const uint16_t green_lut[64],
                                     const uint16_t blue_lut[32]) {
  return (uint16_t)(red_lut[(color >> 11u) & 0x1fu] |
                    green_lut[(color >> 5u) & 0x3fu] |
                    blue_lut[color & 0x1fu]);
}

static void fade_region_to_black(h2_lua_job_t *job, int x, int y, int width,
                                 int height, unsigned amount) {
  static const unsigned k_fade_quantum = 38u;
  unsigned inverse;
  uint16_t red_lut[32];
  uint16_t green_lut[64];
  uint16_t blue_lut[32];
  if (amount == 0u) {
    return;
  }
  if (amount == 255u) {
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      memset(pixels, 0, (size_t)width * sizeof(*pixels));
    }
    mark_dirty_rect(job, x, y, width, height);
    return;
  }
  /* At very high Desktop frame rates a time-correct alpha can be smaller than
   * one RGB565 channel step. Applying that amount with integer rounding either
   * erases trails too quickly or leaves dim pixels stuck forever. Spatially
   * dither a 15% reference fade instead: every pixel receives the same average
   * decay over time while each individual update remains representable. */
  inverse = 255u - (amount < k_fade_quantum ? k_fade_quantum : amount);
  for (unsigned value = 0u; value < 32u; ++value) {
    unsigned faded = value * inverse / 255u;
    red_lut[value] = (uint16_t)(faded << 11u);
    blue_lut[value] = (uint16_t)faded;
  }
  for (unsigned value = 0u; value < 64u; ++value) {
    green_lut[value] = (uint16_t)((value * inverse / 255u) << 5u);
  }
  if (amount < k_fade_quantum) {
    unsigned selector = job->display_fade_phase;
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      for (int column = 0; column < width; ++column) {
        if (selector < amount) {
          pixels[column] = fade_rgb565_to_black(
              pixels[column], red_lut, green_lut, blue_lut);
        }
        selector += 17u;
        if (selector >= k_fade_quantum) {
          selector -= k_fade_quantum;
        }
      }
    }
    job->display_fade_phase =
        (uint8_t)((job->display_fade_phase + amount) % k_fade_quantum);
  } else {
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      size_t count = (size_t)width;
      while (count >= 4u) {
        pixels[0] =
            fade_rgb565_to_black(pixels[0], red_lut, green_lut, blue_lut);
        pixels[1] =
            fade_rgb565_to_black(pixels[1], red_lut, green_lut, blue_lut);
        pixels[2] =
            fade_rgb565_to_black(pixels[2], red_lut, green_lut, blue_lut);
        pixels[3] =
            fade_rgb565_to_black(pixels[3], red_lut, green_lut, blue_lut);
        pixels += 4;
        count -= 4u;
      }
      while (count != 0u) {
        *pixels =
            fade_rgb565_to_black(*pixels, red_lut, green_lut, blue_lut);
        ++pixels;
        --count;
      }
    }
  }
  mark_dirty_rect(job, x, y, width, height);
}

/* Apply a translucent black overlay to the retained RGB565 framebuffer.
 * This is the embedded equivalent of Canvas2D filling each animation frame
 * with rgba(0, 0, 0, alpha), which produces smooth particle afterimages
 * without allocating or redrawing explicit trail geometry in Lua. */
static int display_fade_to_black(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer requested_amount = luaL_checkinteger(state, 1);
  if (!job->display_open || requested_amount < 0 || requested_amount > 255) {
    return luaL_error(state, "invalid fade_to_black");
  }
  fade_region_to_black(job, 0, 0, job->display_info.width,
                       job->display_info.height, (unsigned)requested_amount);
  return 0;
}

static int display_fade_rect_to_black(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  lua_Integer requested_amount = luaL_checkinteger(state, 5);
  if (!job->display_open || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x > job->display_info.width - width ||
      y > job->display_info.height - height || requested_amount < 0 ||
      requested_amount > 255) {
    return luaL_error(state, "invalid fade_rect_to_black");
  }
  fade_region_to_black(job, x, y, width, height,
                       (unsigned)requested_amount);
  return 0;
}

static int display_fill_round_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  int radius = check_pixel_number(state, 5);
  uint16_t color = check_color(state, 6);
  int py;
  if (!job->display_open || !rect_is_bounded(job, x, y, width, height) ||
      radius < 0 || radius > width / 2 || radius > height / 2) {
    return luaL_error(state, "invalid fill_round_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = 0; py < height; ++py) {
    int inset = rounded_rect_inset(height, radius, py);
    fill_span(job, y + py, x + inset, x + width - inset - 1, color);
  }
  return 0;
}

static int display_draw_round_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  int radius = check_pixel_number(state, 5);
  uint16_t color = check_color(state, 6);
  int py;
  if (!job->display_open || width <= 0 || height <= 0 ||
      !rect_is_bounded(job, x, y, width, height) || radius < 0 ||
      radius > width / 2 || radius > height / 2) {
    return luaL_error(state, "invalid draw_round_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = 0; py < height; ++py) {
    int outer_inset = rounded_rect_inset(height, radius, py);
    int inner_width = width - 2;
    int inner_height = height - 2;
    int inner_radius = radius > 0 ? radius - 1 : 0;
    int inner_row = py - 1;
    if (inner_width <= 0 || inner_height <= 0 || inner_row < 0 ||
        inner_row >= inner_height) {
      fill_span(job, y + py, x + outer_inset, x + width - outer_inset - 1,
                color);
    } else {
      int inner_inset =
          rounded_rect_inset(inner_height, inner_radius, inner_row);
      int inner_min_x = x + 1 + inner_inset;
      int inner_max_x = x + width - inner_inset - 2;
      fill_span(job, y + py, x + outer_inset, inner_min_x - 1, color);
      fill_span(job, y + py, inner_max_x + 1, x + width - outer_inset - 1,
                color);
    }
  }
  return 0;
}

static int64_t triangle_sign(int px, int py, int ax, int ay, int bx, int by) {
  return ((int64_t)px - bx) * ((int64_t)ay - by) -
         ((int64_t)ax - bx) * ((int64_t)py - by);
}

static int display_fill_triangle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x0 = check_pixel_number(state, 1);
  int y0 = check_pixel_number(state, 2);
  int x1 = check_pixel_number(state, 3);
  int y1 = check_pixel_number(state, 4);
  int x2 = check_pixel_number(state, 5);
  int y2 = check_pixel_number(state, 6);
  uint16_t color = check_color(state, 7);
  int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
  int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
  int min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
  int max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
  int x;
  int y;
  if (!job->display_open || !point_is_bounded(job, x0, y0) ||
      !point_is_bounded(job, x1, y1) || !point_is_bounded(job, x2, y2)) {
    return luaL_error(state, "display is not open");
  }
  for (y = min_y; y <= max_y; ++y) {
    for (x = min_x; x <= max_x; ++x) {
      int64_t d0 = triangle_sign(x, y, x0, y0, x1, y1);
      int64_t d1 = triangle_sign(x, y, x1, y1, x2, y2);
      int64_t d2 = triangle_sign(x, y, x2, y2, x0, y0);
      if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) {
        set_pixel(job, x, y, color);
      }
    }
  }
  return 0;
}

static const uint8_t *glyph_rows(unsigned char character) {
  static const uint8_t unknown[7] = {14, 17, 1, 2, 4, 0, 4};
  static const uint8_t digits[10][7] = {
      {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
      {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
      {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
      {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
  };
  static const uint8_t letters[26][7] = {
      {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
      {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
      {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
      {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
      {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 18, 18, 12},
      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
      {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
      {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
      {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
      {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
      {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
      {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
      {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
  };
  if (character >= '0' && character <= '9')
    return digits[character - '0'];
  if (character >= 'a' && character <= 'z')
    character -= 'a' - 'A';
  if (character >= 'A' && character <= 'Z')
    return letters[character - 'A'];
  return unknown;
}

static void draw_glyph(h2_lua_job_t *job, int x, int y, unsigned char character,
                       int scale, uint16_t color) {
  const uint8_t *rows = glyph_rows(character);
  int row;
  int column;
  int sx;
  int sy;
  if (character == ' ')
    return;
  for (row = 0; row < 7; ++row) {
    for (column = 0; column < 5; ++column) {
      if ((rows[row] & (1u << (4 - column))) == 0u)
        continue;
      for (sy = 0; sy < scale; ++sy) {
        for (sx = 0; sx < scale; ++sx) {
          set_pixel(job, x + column * scale + sx, y + row * scale + sy, color);
        }
      }
    }
  }
}

static int draw_text_at(lua_State *state, h2_lua_job_t *job, int x, int y,
                        const char *text, size_t length, uint16_t color,
                        int scale) {
  size_t i;
  if (!job->display_open || scale < 1 || scale > 8 ||
      length > job->host->config.output_limit_bytes ||
      length > (size_t)INT_MAX / (6u * (size_t)scale) ||
      !point_is_bounded(job, x, y)) {
    return luaL_error(state, "invalid draw_text");
  }
  for (i = 0u; i < length; ++i) {
    draw_glyph(job, x + (int)i * 6 * scale, y, (unsigned char)text[i], scale,
               color);
  }
  return 0;
}

static int display_draw_text(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  size_t length;
  const char *text = luaL_checklstring(state, 3, &length);
  uint16_t color = rgb_to_rgb565(255u, 255u, 255u);
  int font_size = 24;
  int scale;
  if (!lua_isnoneornil(state, 4)) {
    luaL_checktype(state, 4, LUA_TTABLE);
    lua_getfield(state, 4, "color");
    if (!lua_isnil(state, -1))
      color = check_color(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 4, "font_size");
    if (!lua_isnil(state, -1))
      font_size = (int)luaL_checkinteger(state, -1);
    lua_pop(state, 1);
  }
  if (font_size < 1 || font_size > 64)
    return luaL_error(state, "display font_size must be between 1 and 64");
  scale = (font_size + 6) / 7;
  return draw_text_at(state, job, x, y, text, length, color, scale);
}

static int display_draw_text_aligned(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  size_t length;
  const char *text = luaL_checklstring(state, 5, &length);
  uint16_t color = rgb_to_rgb565(255u, 255u, 255u);
  int font_size = 24;
  int align = 0;
  int valign = 0;
  int scale;
  int text_width;
  int text_height;
  int64_t aligned_x;
  int64_t aligned_y;
  if (!lua_isnoneornil(state, 6)) {
    const char *value;
    luaL_checktype(state, 6, LUA_TTABLE);
    lua_getfield(state, 6, "color");
    if (!lua_isnil(state, -1))
      color = check_color(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 6, "font_size");
    if (!lua_isnil(state, -1))
      font_size = (int)luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 6, "align");
    value = lua_tostring(state, -1);
    if (value != NULL) {
      if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0)
        align = 1;
      else if (strcmp(value, "right") == 0)
        align = 2;
      else if (strcmp(value, "left") != 0)
        return luaL_error(state,
                          "display align must be left, center, or right");
    }
    lua_pop(state, 1);
    lua_getfield(state, 6, "valign");
    value = lua_tostring(state, -1);
    if (value != NULL) {
      if (strcmp(value, "middle") == 0 || strcmp(value, "center") == 0)
        valign = 1;
      else if (strcmp(value, "bottom") == 0)
        valign = 2;
      else if (strcmp(value, "top") != 0)
        return luaL_error(state,
                          "display valign must be top, middle, or bottom");
    }
    lua_pop(state, 1);
  }
  if (font_size < 1 || font_size > 64)
    return luaL_error(state, "display font_size must be between 1 and 64");
  scale = (font_size + 6) / 7;
  if (width < 0 || height < 0 || align < 0 || align > 2 || scale < 1 ||
      scale > 10 || length > (size_t)INT_MAX / (6u * (size_t)scale)) {
    return luaL_error(state, "invalid draw_text_aligned");
  }
  text_width = (int)(length * 6u * (size_t)scale);
  text_height = 7 * scale;
  aligned_x = x;
  aligned_y = y;
  if (align == 1)
    aligned_x += ((int64_t)width - text_width) / 2;
  if (align == 2)
    aligned_x += (int64_t)width - text_width;
  if (valign == 1)
    aligned_y += ((int64_t)height - text_height) / 2;
  if (valign == 2)
    aligned_y += (int64_t)height - text_height;
  if (aligned_x < INT_MIN || aligned_x > INT_MAX || aligned_y < INT_MIN ||
      aligned_y > INT_MAX) {
    return luaL_error(state, "invalid draw_text_aligned");
  }
  return draw_text_at(state, job, (int)aligned_x, (int)aligned_y, text, length,
                      color, scale);
}

static int display_integer(lua_State *state, int index, int fallback,
                            int minimum, int maximum) {
  lua_Integer value = luaL_optinteger(state, index, fallback);
  if (value < minimum || value > maximum)
    luaL_argerror(state, index, "display integer out of range");
  return (int)value;
}

static display_region_t *display_new_region(lua_State *state, int width,
                                            int height, size_t pixels,
                                            size_t runs, int masked,
                                            uint16_t key) {
  size_t bytes = sizeof(display_region_t) +
                 (size_t)height * sizeof(display_region_row_t);
  size_t tiles = display_tile_count(width, height);
  if (runs > (SIZE_MAX - bytes) / sizeof(display_region_run_t))
    luaL_error(state, "region size overflow");
  bytes += runs * sizeof(display_region_run_t);
  if (pixels > (SIZE_MAX - bytes) / sizeof(uint16_t))
    luaL_error(state, "region size overflow");
  bytes += pixels * sizeof(uint16_t);
  if (tiles > SIZE_MAX - bytes)
    luaL_error(state, "region size overflow");
  /* Create metatable first: no allocation follows returning the userdata. */
  if (luaL_newmetatable(state, H2_LUA_DISPLAY_REGION_META)) {
    lua_pushliteral(state, "display region");
    lua_setfield(state, -2, "__metatable");
  }
  display_region_t *region = lua_newuserdatauv(state, bytes + tiles, 0);
  region->width = width;
  region->height = height;
  region->masked = masked;
  region->key = key;
  region->pixel_count = pixels;
  region->run_count = runs;
  lua_pushvalue(state, -2);
  lua_setmetatable(state, -2);
  lua_remove(state, -2);
  memset(display_region_damage(region), 1, tiles);
  return region;
}

static void display_check_capture(lua_State *state, h2_lua_job_t *job,
                                   int x, int y, int width, int height) {
  if (!job->display_open || width > job->display_info.width ||
      height > job->display_info.height ||
      x > job->display_info.width - width ||
      y > job->display_info.height - height)
    luaL_error(state, "invalid capture region or closed display");
}

static int display_capture_region(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = display_integer(state, 1, -1, 0, 100000);
  int y = display_integer(state, 2, -1, 0, 100000);
  int width = display_integer(state, 3, 0, 1, 4096);
  int height = display_integer(state, 4, 0, 1, 4096);
  int masked = !lua_isnoneornil(state, 5);
  uint16_t key = masked ? check_color(state, 5) : 0;
  display_region_t *reuse = lua_isnoneornil(state, 6) ? NULL :
      luaL_checkudata(state, 6, H2_LUA_DISPLAY_REGION_META);
  if (reuse != NULL && (masked || reuse->masked || reuse->width != width ||
                        reuse->height != height))
    return luaL_error(state, "region reuse requires matching opaque storage");
  display_check_capture(state, job, x, y, width, height);
  size_t count = (size_t)width * height;
  display_region_t *region = reuse;
  uint16_t *captured;
  if (masked) {
    captured = lua_newuserdatauv(state, count * sizeof(uint16_t), 0);
  } else {
    if (region == NULL)
      region = display_new_region(state, width, height, count, 0, 0, 0);
    else
      lua_pushvalue(state, 6);
    captured = display_region_pixels(region);
  }
  /* Allocation may run arbitrary finalizers, including deinit/reacquire. */
  display_check_capture(state, job, x, y, width, height);
  for (int row = 0; row < height; ++row)
    memcpy(captured + (size_t)row * width,
           job->framebuffer + (size_t)(row + y) * job->display_info.width + x,
           (size_t)width * sizeof(uint16_t));
  if (masked) {
    size_t packed = 0, runs = 0;
    for (int row = 0; row < height; ++row) {
      const uint16_t *line = captured + (size_t)row * width;
      int left = 0, right = width;
      while (left < right && line[left] == key) ++left;
      while (right > left && line[right - 1] == key) --right;
      packed += (size_t)(right - left);
      for (int col = left; col < right; ++col)
        if (line[col] != key && (col == left || line[col - 1] == key)) ++runs;
    }
    region = display_new_region(state, width, height, packed, runs, 1, key);
    size_t offset = 0, run_index = 0;
    for (int row = 0; row < height; ++row) {
      const uint16_t *line = captured + (size_t)row * width;
      int left = 0, right = width;
      while (left < right && line[left] == key) ++left;
      while (right > left && line[right - 1] == key) --right;
      display_region_row_t *r = &region->rows[row];
      *r = (display_region_row_t){offset, run_index, left, right, 0};
      memcpy(display_region_pixels(region) + offset, line + left,
             (size_t)(right - left) * sizeof(uint16_t));
      offset += (size_t)(right - left);
      for (int col = left; col < right;) {
        if (line[col] == key) { ++col; continue; }
        int first = col++;
        while (col < right && line[col] != key) ++col;
        display_region_runs(region)[run_index++] =
            (display_region_run_t){(uint16_t)first, (uint16_t)col};
        ++r->run_count;
      }
    }
  } else {
    for (int row = 0; row < height; ++row)
      region->rows[row] = (display_region_row_t){(size_t)row * width, 0,
                                                0, width, 0};
    if (job->display_background == region)
      job->display_background_valid = 0;
  }
  return 1;
}

static void display_copy_region_span(h2_lua_job_t *job, int x, int y,
                                      const uint16_t *pixels, int count) {
  if (count <= 0) return;
  memcpy(job->framebuffer + (size_t)y * job->display_info.width + x, pixels,
         (size_t)count * sizeof(uint16_t));
  mark_dirty_rect(job, x, y, count, 1);
}

static int display_draw_region(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_region_t *region = luaL_checkudata(state, 1, H2_LUA_DISPLAY_REGION_META);
  int x = display_integer(state, 2, 0, -100000, 100000);
  int y = display_integer(state, 3, 0, -100000, 100000);
  int top = display_integer(state, 4, 0, 0, job->display_info.height);
  int bottom = display_integer(state, 5, job->display_info.height, top,
                               job->display_info.height);
  int keyed = !lua_isnoneornil(state, 6);
  uint16_t key = keyed ? check_color(state, 6) : 0;
  int left = display_integer(state, 7, 0, 0, job->display_info.width);
  int right = display_integer(state, 8, job->display_info.width, left,
                              job->display_info.width);
  if (!job->display_open || bottom > job->display_info.height)
    return luaL_error(state, "display is not open or clip changed");
  int first_x = left > x ? left - x : 0;
  int last_x = right - x < region->width ? right - x : region->width;
  int first_y = top > y ? top - y : 0;
  int last_y = bottom - y < region->height ? bottom - y : region->height;
  if (first_x >= last_x) return 0;
  uint16_t *pixels = display_region_pixels(region);
  for (int row = first_y; row < last_y; ++row) {
    display_region_row_t *r = &region->rows[row];
    if (!region->masked && !keyed) {
      display_copy_region_span(job, x + first_x, y + row,
          pixels + r->offset + first_x, last_x - first_x);
    } else if (region->masked && keyed && key == region->key) {
      for (int i = 0; i < r->run_count; ++i) {
        display_region_run_t run = display_region_runs(region)[r->first_run + i];
        int a = run.left > first_x ? run.left : first_x;
        int b = run.right < last_x ? run.right : last_x;
        if (a < b)
          display_copy_region_span(job, x + a, y + row,
              pixels + r->offset + a - r->left, b - a);
      }
    } else {
      for (int col = first_x; col < last_x; ++col) {
        uint16_t color = col < r->left || col >= r->right ? region->key :
            pixels[r->offset + (size_t)(col - r->left)];
        if (!keyed || color != key) set_pixel(job, x + col, y + row, color);
      }
    }
  }
  return 0;
}

static void display_release_background(lua_State *state, h2_lua_job_t *job) {
  int ref = job->display_background_ref;
  job->display_background = NULL;
  job->display_background_ref = 0;
  job->display_background_valid = 0;
  if (ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, ref);
}

static int display_release_background_lua(lua_State *state) {
  display_release_background(state, lua_touserdata(state, lua_upvalueindex(1)));
  return 0;
}

static int display_restore_background(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_region_t *region = luaL_checkudata(state, 1, H2_LUA_DISPLAY_REGION_META);
  if (!job->display_open || region->masked ||
      region->width != job->display_info.width ||
      region->height != job->display_info.height)
    return luaL_error(state, "background must be an opaque full-screen region");
  if (job->display_background != region) {
    lua_pushvalue(state, 1);
    int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    if (!job->display_open || region->width != job->display_info.width ||
        region->height != job->display_info.height) {
      luaL_unref(state, LUA_REGISTRYINDEX, ref);
      return luaL_error(state, "display changed while binding background");
    }
    display_release_background(state, job);
    job->display_background = region;
    job->display_background_ref = ref;
  }
  uint16_t *pixels = display_region_pixels(region);
  uint8_t *damage = display_region_damage(region);
  if (!job->display_background_valid) {
    memcpy(job->framebuffer, pixels, region->pixel_count * sizeof(uint16_t));
    display_dirty_full(job);
  } else {
    int columns = (region->width + 15) / 16;
    int rows = (region->height + 15) / 16;
    for (int ty = 0; ty < rows; ++ty) {
      for (int tx = 0; tx < columns;) {
        if (!damage[(size_t)ty * columns + tx]) { ++tx; continue; }
        int first = tx++;
        while (tx < columns && damage[(size_t)ty * columns + tx]) ++tx;
        int right = tx * 16 < region->width ? tx * 16 : region->width;
        int bottom = (ty + 1) * 16 < region->height ? (ty + 1) * 16 : region->height;
        for (int y = ty * 16; y < bottom; ++y)
          display_copy_region_span(job, first * 16, y,
              pixels + (size_t)y * region->width + first * 16, right - first * 16);
      }
    }
  }
  memset(damage, 0, display_tile_count(region->width, region->height));
  job->display_background_valid = 1;
  return 0;
}

static int display_begin_frame(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int clear = 0;
  uint16_t color = 0u;
  if (!lua_isnoneornil(state, 1)) {
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "clear");
    clear = lua_toboolean(state, -1);
    lua_pop(state, 1);
    if (clear) {
      lua_getfield(state, 1, "color");
      if (!lua_isnil(state, -1))
        color = check_color(state, -1);
      lua_pop(state, 1);
    }
  }
  if (!job->display_open || job->frame_open)
    return luaL_error(state, "invalid begin_frame");
  job->frame_open = 1;
  if (clear) display_clear_pixels(job, color);
  return 0;
}

static void display_option(lua_State *state, const char *key) {
  if (lua_isnoneornil(state, 1)) lua_pushnil(state);
  else { lua_pushstring(state, key); lua_rawget(state, 1); }
}

static int display_boolean_option(lua_State *state, const char *key,
                                   int fallback) {
  display_option(state, key);
  int value = fallback;
  if (!lua_isnil(state, -1)) {
    luaL_checktype(state, -1, LUA_TBOOLEAN);
    value = lua_toboolean(state, -1);
  }
  lua_pop(state, 1);
  return value;
}

static void display_release_presented(lua_State *state, h2_lua_job_t *job) {
  int ref = job->display_presented_ref;
  job->display_presented = NULL;
  job->display_presented_ref = 0;
  job->display_presented_valid = 0;
  if (ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, ref);
}

static void display_enable_retained(lua_State *state, h2_lua_job_t *job) {
  int width = job->display_info.width, height = job->display_info.height;
  if (width > 4096 || height > 4096)
    luaL_error(state, "retained display dimensions exceed 4096");
  size_t count = (size_t)width * height;
  size_t tiles = display_tile_count(width, height);
  size_t bytes = sizeof(display_presented_t) + tiles;
  if (count > (SIZE_MAX - bytes) / sizeof(uint16_t))
    luaL_error(state, "retained display size overflow");
  display_presented_t *frame = lua_newuserdatauv(state,
      bytes + count * sizeof(uint16_t), 0);
  frame->pixel_count = count;
  int ref = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!job->display_open || width != job->display_info.width ||
      height != job->display_info.height) {
    luaL_unref(state, LUA_REGISTRYINDEX, ref);
    luaL_error(state, "display changed while enabling retained mode");
  }
  /* GC may have installed another baseline; detach it before publishing ours. */
  display_release_presented(state, job);
  job->display_presented = frame;
  job->display_presented_ref = ref;
}

static h2_pal_result_t display_submit_rect(h2_lua_job_t *job, int x, int y,
                                           int width, int height,
                                           size_t *pixels, size_t *rects) {
  h2_display_rect_t rect = {x, y, width, height};
  if (job->display_presented != NULL) job->display_presented_valid = 0;
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
      job->host->config.runtime->display, &rect,
      job->framebuffer + (size_t)y * job->display_info.width + x,
      (size_t)job->display_info.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
  if (result == H2_PAL_OK) {
    *pixels += (size_t)width * height;
    ++*rects;
    display_presented_t *frame = job->display_presented;
    if (frame != NULL) {
      /* Tentative until present succeeds. Failure forces a complete retry. */
      for (int row = y; row < y + height; ++row) {
        size_t at = (size_t)row * job->display_info.width + x;
        memcpy(frame->pixels + at, job->framebuffer + at,
               (size_t)width * sizeof(uint16_t));
      }
    }
  }
  return result;
}

static int display_tile_changed(h2_lua_job_t *job, display_presented_t *frame,
                                 int tx, int ty) {
  int width = job->display_info.width, height = job->display_info.height;
  int x = tx * 16, y = ty * 16;
  int right = x + 16 < width ? x + 16 : width;
  int bottom = y + 16 < height ? y + 16 : height;
  for (; y < bottom; ++y) {
    size_t at = (size_t)y * width + x;
    if (memcmp(job->framebuffer + at, frame->pixels + at,
               (size_t)(right - x) * sizeof(uint16_t)) != 0) return 1;
  }
  return 0;
}

static h2_pal_result_t display_submit_retained(h2_lua_job_t *job, int bounds,
                                               int gap, size_t *pixels,
                                               size_t *rects) {
  display_presented_t *frame = job->display_presented;
  int width = job->display_info.width, height = job->display_info.height;
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  uint8_t *changed = (uint8_t *)(frame->pixels + frame->pixel_count);
  memset(changed, 0, (size_t)columns * rows);
  int left = columns, top = rows, right = 0, bottom = 0;
  /* Finish every comparison before touching the backend or baseline. */
  if (job->dirty_valid) {
    for (int ty = job->dirty_min_y / 16; ty <= job->dirty_max_y / 16; ++ty) {
      for (int tx = job->dirty_min_x / 16; tx <= job->dirty_max_x / 16; ++tx) {
        if (!display_tile_changed(job, frame, tx, ty)) continue;
        changed[(size_t)ty * columns + tx] = 1;
        if (tx < left) left = tx;
        if (ty < top) top = ty;
        if (tx + 1 > right) right = tx + 1;
        if (ty + 1 > bottom) bottom = ty + 1;
      }
    }
  }
  if (right == 0) return H2_PAL_OK;
  if (bounds) {
    int r = right * 16 < width ? right * 16 : width;
    int b = bottom * 16 < height ? bottom * 16 : height;
    return display_submit_rect(job, left * 16, top * 16,
                               r - left * 16, b - top * 16, pixels, rects);
  }
  for (int ty = top; ty < bottom; ++ty) {
    for (int tx = left; tx < right; ++tx) {
      if (!changed[(size_t)ty * columns + tx]) continue;
      int end_x = tx + 1;
      for (int x = end_x; x < right && x - end_x <= gap; ++x)
        if (changed[(size_t)ty * columns + x]) end_x = x + 1;
      int end_y = ty + 1;
      for (int y = end_y; y < bottom && y - end_y <= gap; ++y) {
        int any = 0;
        for (int x = tx; x < end_x; ++x) any |= changed[(size_t)y * columns + x];
        if (any) end_y = y + 1;
      }
      for (int y = ty; y < end_y; ++y)
        memset(changed + (size_t)y * columns + tx, 0, (size_t)(end_x - tx));
      int r = end_x * 16 < width ? end_x * 16 : width;
      int b = end_y * 16 < height ? end_y * 16 : height;
      h2_pal_result_t result = display_submit_rect(job, tx * 16, ty * 16,
          r - tx * 16, b - ty * 16, pixels, rects);
      if (result != H2_PAL_OK) return result;
    }
  }
  return H2_PAL_OK;
}

static int display_present(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  if (!lua_isnoneornil(state, 1)) luaL_checktype(state, 1, LUA_TTABLE);
  int retained = display_boolean_option(state, "retained", job->display_presented != NULL);
  int bounds = display_boolean_option(state, "bounds", 0);
  display_option(state, "merge_gap");
  int gap = display_integer(state, -1, 0, 0, 8);
  lua_pop(state, 1);
  if (!job->display_open) return luaL_error(state, "display is not open");
  if (retained && job->display_presented == NULL) display_enable_retained(state, job);
  else if (!retained && job->display_presented != NULL) {
    display_release_presented(state, job);
    display_dirty_full(job);
  }
  size_t pixels = 0, rects = 0;
  h2_pal_result_t result = H2_PAL_OK;
  if (retained && !job->display_presented_valid)
    result = display_submit_rect(job, 0, 0, job->display_info.width,
                                 job->display_info.height, &pixels, &rects);
  else if (retained)
    result = display_submit_retained(job, bounds, gap, &pixels, &rects);
  else if (job->dirty_valid)
    result = display_submit_rect(job, job->dirty_min_x, job->dirty_min_y,
        job->dirty_max_x - job->dirty_min_x + 1,
        job->dirty_max_y - job->dirty_min_y + 1, &pixels, &rects);
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_display_present(
        job->host->config.runtime->display);
  }
  if (result != H2_PAL_OK) {
    job->display_presented_valid = 0;
    job->display_background_valid = 0;
    display_dirty_full(job);
    return luaL_error(state, "display present failed: %d", result);
  }
  if (retained) {
    /* Only the whole successful present commits the tentative baseline. */
    job->display_presented_valid = 1;
  }
  job->dirty_valid = 0;
  lua_pushinteger(state, (lua_Integer)pixels);
  lua_pushinteger(state, (lua_Integer)rects);
  return 2;
}

static int display_end_frame(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  if (!job->frame_open) {
    return luaL_error(state, "invalid end_frame");
  }
  job->frame_open = 0;
  return display_present(state);
}

static void display_release(lua_State *state, h2_lua_job_t *job) {
  uint16_t *pixels = job->framebuffer;
  int was_open = job->display_open;
  job->framebuffer = NULL;
  job->display_open = job->frame_open = job->dirty_valid = 0;
  if (state != NULL) {
    display_release_background(state, job);
    display_release_presented(state, job);
    int smooth_ref = job->display_smooth_ref;
    job->display_smooth = NULL;
    job->display_smooth_ref = 0;
    if (smooth_ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, smooth_ref);
  }
  if (was_open && !job->host->config.borrow_display)
    (void)h2_pal_display_close(job->host->config.runtime->display);
  h2_pal_mem_free(job->host->config.runtime->mem, pixels);
}

void h2_lua_job_close_display(h2_lua_job_t *job) {
  job->display_shutting_down = 1;
  display_release(job->vm != NULL ? job->vm->state : NULL, job);
}

static int display_close(lua_State *state) {
  display_release(state, lua_touserdata(state, lua_upvalueindex(1)));
  return 0;
}

static int push_display_proxy(lua_State *state, h2_lua_job_t *job) {
  h2_pal_result_t result;
  result = display_open(job);
  if (result != H2_PAL_OK) {
    lua_pushnil(state);
    lua_pushfstring(state, "display open failed: %d", result);
    return 2;
  }
  lua_createtable(state, 0, 31);
  set_function(state, "stroke_path", display_stroke_path, job);
  set_function(state, "capture_region", display_capture_region, job);
  set_function(state, "draw_region", display_draw_region, job);
  set_function(state, "restore_background", display_restore_background, job);
  set_function(state, "release_background", display_release_background_lua, job);
  set_function(state, "compile_mesh", display_compile_mesh, job);
  set_function(state, "update_mesh", display_update_mesh, job);
  set_function(state, "draw_mesh", display_draw_mesh, job);
  set_function(state, "fill_polygon", display_fill_polygon, job);
  set_function(state, "fill_ellipse", display_fill_ellipse, job);
  set_function(state, "compile_commands", display_compile_commands, job);
  set_function(state, "draw_commands", display_draw_commands, job);
  set_function(state, "clear", display_clear, job);
  set_function(state, "fill_rect", display_fill_rect, job);
  set_function(state, "draw_line", display_draw_line, job);
  set_function(state, "fill_circle", display_fill_circle, job);
  set_function(state, "draw_circle", display_draw_circle, job);
  set_function(state, "fill_circle_aa", display_fill_circle_aa, job);
  set_function(state, "fade_to_black", display_fade_to_black, job);
  set_function(state, "fade_rect_to_black", display_fade_rect_to_black, job);
  set_function(state, "fill_round_rect", display_fill_round_rect, job);
  set_function(state, "draw_round_rect", display_draw_round_rect, job);
  set_function(state, "fill_triangle", display_fill_triangle, job);
  set_function(state, "draw_text", display_draw_text, job);
  set_function(state, "draw_text_aligned", display_draw_text_aligned, job);
  set_function(state, "begin_frame", display_begin_frame, job);
  set_function(state, "end_frame", display_end_frame, job);
  set_function(state, "present", display_present, job);
  set_function(state, "deinit", display_close, job);
  lua_pushinteger(state, job->display_info.width);
  lua_setfield(state, -2, "width");
  lua_pushinteger(state, job->display_info.height);
  lua_setfield(state, -2, "height");
  return 1;
}

static void touch_push_result(lua_State *state, h2_lua_job_t *job,
                              int just_pressed, int just_released, int dx,
                              int dy) {
  uint64_t held_ms = 0u;
  if (job->touch_pressed && job->touch_press_started_ms != 0u) {
    held_ms = h2_lua_now_ms(job->host) - job->touch_press_started_ms;
  }
  lua_createtable(state, 0, 9);
  lua_pushboolean(state, job->touch_pressed);
  lua_setfield(state, -2, "pressed");
  lua_pushboolean(state, just_pressed);
  lua_setfield(state, -2, "just_pressed");
  lua_pushboolean(state, just_released);
  lua_setfield(state, -2, "just_released");
  lua_pushinteger(state, job->touch_x);
  lua_setfield(state, -2, "x");
  lua_pushinteger(state, job->touch_y);
  lua_setfield(state, -2, "y");
  lua_pushinteger(state, dx);
  lua_setfield(state, -2, "dx");
  lua_pushinteger(state, dy);
  lua_setfield(state, -2, "dy");
  lua_pushboolean(state, dx != 0 || dy != 0);
  lua_setfield(state, -2, "moved");
  lua_pushinteger(state, (lua_Integer)held_ms);
  lua_setfield(state, -2, "held_ms");
}

static int touch_poll(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_pal_touch_event_t event;
  h2_pal_result_t result;
  int old_x = job->touch_x;
  int old_y = job->touch_y;
  int just_pressed = 0;
  int just_released = 0;
  int dx = 0;
  int dy = 0;
  if (!job->touch_open) {
    return luaL_error(state, "touch is not open");
  }
  result = h2_pal_touch_poll_event(job->host->config.runtime->touch, &event);
  if (result == H2_PAL_ERR_WOULD_BLOCK) {
    touch_push_result(state, job, 0, 0, 0, 0);
    return 1;
  }
  if (result != H2_PAL_OK) {
    return luaL_error(state, "touch poll failed: %d", result);
  }
  if (event.kind == H2_PAL_TOUCH_EVENT_DOWN) {
    just_pressed = !job->touch_pressed;
    job->touch_pressed = 1;
    job->touch_press_started_ms = h2_lua_now_ms(job->host);
  } else if (event.kind == H2_PAL_TOUCH_EVENT_UP) {
    just_released = job->touch_pressed;
    job->touch_pressed = 0;
    job->touch_press_started_ms = 0u;
  } else if (event.kind != H2_PAL_TOUCH_EVENT_MOVE) {
    return luaL_error(state, "touch poll returned invalid event kind");
  }
  job->touch_x = event.x;
  job->touch_y = event.y;
  if (job->touch_initialized) {
    dx = job->touch_x - old_x;
    dy = job->touch_y - old_y;
  }
  job->touch_initialized = 1;
  touch_push_result(state, job, just_pressed, just_released, dx, dy);
  return 1;
}

static int touch_read(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  touch_push_result(state, job, 0, 0, 0, 0);
  return 1;
}

static int touch_sync(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  job->touch_initialized = 0;
  return touch_poll(state);
}

static int push_touch_proxy(lua_State *state, h2_lua_job_t *job) {
  h2_pal_result_t result;
  if (!job->touch_open) {
    result = h2_pal_touch_open(job->host->config.runtime->touch);
    if (result != H2_PAL_OK) {
      lua_pushnil(state);
      lua_pushfstring(state, "touch open failed: %d", result);
      return 2;
    }
    job->touch_open = 1;
  }
  lua_createtable(state, 0, 3);
  set_function(state, "read", touch_read, job);
  set_function(state, "poll", touch_poll, job);
  set_function(state, "sync", touch_sync, job);
  return 1;
}

static int button_get_key_level(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_runtime_button_state_t button_state;
  h2_pal_result_t result = h2_runtime_component_state_button(
      job->host->config.runtime, job->button_component_id, &button_state);
  if (result != H2_PAL_OK) {
    lua_pushnil(state);
    lua_pushfstring(state, "button state failed: %d", result);
    return 2;
  }
  lua_pushinteger(state, button_state.pressed ? 1 : 0);
  return 1;
}

static int push_button_proxy(lua_State *state, h2_lua_job_t *job,
                             h2_runtime_component_id_t component_id) {
  if (job->button_component_id != H2_RUNTIME_COMPONENT_ID_NONE &&
      job->button_component_id != component_id) {
    lua_pushnil(state);
    lua_pushliteral(state, "another Button component is already bound");
    return 2;
  }
  job->button_component_id = component_id;
  lua_createtable(state, 0, 1);
  set_function(state, "get_key_level", button_get_key_level, job);
  return 1;
}

static int audio_write_result(lua_State *state, int result, size_t written) {
  if (result == H2_PAL_OK) {
    lua_pushboolean(state, 1);
    return 1;
  }
  lua_pushnil(state);
  lua_pushstring(state,
                 result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_BUSY
                     ? "audio output: busy"
                     : "audio output: write failed");
  lua_pushinteger(state, (lua_Integer)written);
  return 3;
}

static size_t audio_slot_chunk_bytes(const h2_lua_audio_track_slot_t *slot) {
  return (size_t)slot->format.frame_samples_per_channel *
         h2_audio_pcm_frame_bytes(&slot->format);
}

static int audio_slot_write_frame(h2_lua_audio_track_slot_t *slot,
                                  const void *data, size_t chunk_bytes) {
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer((void *)data, chunk_bytes, slot->format);
  frame.bytes = chunk_bytes;
  frame.samples_per_channel = slot->format.frame_samples_per_channel;
  return h2_pal_audio_track_write(slot->track, &frame, 0u);
}

void h2_lua_audio_track_slot_release_carry(h2_lua_audio_track_slot_t *slot,
                                           const h2_pal_mem_api_t *mem) {
  if (slot == NULL || slot->carry == NULL) {
    return;
  }
  h2_pal_mem_free(mem, slot->carry);
  slot->carry = NULL;
  slot->carry_bytes = 0u;
}

/* Zero pads and writes whatever the Track still holds below one device frame.
 * Used only when the Track is closing: the stream ends there, so the tail must
 * be emitted rather than carried further. Best effort by design — a busy Track
 * must not block or fail close. */
void h2_lua_audio_track_slot_flush_carry(h2_lua_audio_track_slot_t *slot) {
  size_t chunk_bytes;
  if (slot == NULL || slot->track == NULL || slot->carry == NULL ||
      slot->carry_bytes == 0u) {
    return;
  }
  chunk_bytes = audio_slot_chunk_bytes(slot);
  if (chunk_bytes == 0u || slot->carry_bytes > chunk_bytes) {
    slot->carry_bytes = 0u;
    return;
  }
  memset(slot->carry + slot->carry_bytes, 0, chunk_bytes - slot->carry_bytes);
  (void)audio_slot_write_frame(slot, slot->carry, chunk_bytes);
  slot->carry_bytes = 0u;
}

/* Writes PCM to the Track. Consecutive writes to one Track are a single
 * stream, so when the Audio System reports a fixed frame size the bytes are
 * split into device frames and a sub-frame tail is held in the slot and
 * prepended to the next write instead of being padded with silence; close
 * flushes whatever is left. On busy/failure the third return value is how many
 * bytes of this call the Track accepted, so callers resume from that offset.
 * Bytes moved into the carry buffer count as accepted. */
static int audio_output_write(lua_State *state) {
  h2_lua_audio_track_slot_t *slot = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t generation = (uint32_t)lua_tointeger(state, lua_upvalueindex(2));
  size_t size = 0u;
  const char *bytes = luaL_checklstring(state, 2, &size);
  size_t frame_bytes =
      slot == NULL ? 0u : h2_audio_pcm_frame_bytes(&slot->format);
  size_t chunk_bytes;
  size_t consumed = 0u;
  h2_audio_frame_t frame;
  int result = H2_PAL_OK;
  if (slot == NULL || slot->generation != generation || slot->track == NULL ||
      frame_bytes == 0u || size == 0u || size % frame_bytes != 0u) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio output: invalid frame");
    return 2;
  }
  if (slot->format.frame_samples_per_channel == 0u) {
    if (size / frame_bytes > UINT16_MAX) {
      lua_pushnil(state);
      lua_pushliteral(state, "audio output: invalid frame");
      return 2;
    }
    frame = h2_audio_frame_for_buffer((void *)bytes, size, slot->format);
    frame.bytes = size;
    frame.samples_per_channel = (uint16_t)(size / frame_bytes);
    result = h2_pal_audio_track_write(slot->track, &frame, 0u);
    return audio_write_result(state, result, result == H2_PAL_OK ? size : 0u);
  }
  chunk_bytes = audio_slot_chunk_bytes(slot);
  if (slot->carry == NULL) {
    slot->carry =
        h2_pal_mem_alloc(slot->job->host->config.runtime->mem, chunk_bytes);
    if (slot->carry == NULL) {
      return audio_write_result(state, H2_PAL_ERR_NO_MEMORY, 0u);
    }
    slot->carry_bytes = 0u;
  }
  /* Top up a held tail from the head of this buffer and emit it first. The
   * copy is only committed once the frame is accepted, so a busy Track leaves
   * the carry untouched and the caller can retry the same buffer. */
  if (slot->carry_bytes != 0u) {
    size_t take = chunk_bytes - slot->carry_bytes;
    if (take > size) {
      take = size;
    }
    memcpy(slot->carry + slot->carry_bytes, bytes, take);
    if (slot->carry_bytes + take < chunk_bytes) {
      slot->carry_bytes += take;
      return audio_write_result(state, H2_PAL_OK, size);
    }
    result = audio_slot_write_frame(slot, slot->carry, chunk_bytes);
    if (result != H2_PAL_OK) {
      return audio_write_result(state, result, 0u);
    }
    slot->carry_bytes = 0u;
    consumed = take;
  }
  while (consumed + chunk_bytes <= size) {
    result = audio_slot_write_frame(slot, bytes + consumed, chunk_bytes);
    if (result != H2_PAL_OK) {
      return audio_write_result(state, result, consumed);
    }
    consumed += chunk_bytes;
  }
  if (consumed < size) {
    slot->carry_bytes = size - consumed;
    memcpy(slot->carry, bytes + consumed, slot->carry_bytes);
  }
  return audio_write_result(state, H2_PAL_OK, size);
}

static int audio_output_info(lua_State *state) {
  h2_lua_audio_track_slot_t *slot = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t generation = (uint32_t)lua_tointeger(state, lua_upvalueindex(2));
  int opened =
      slot != NULL && slot->generation == generation && slot->track != NULL;
  lua_createtable(state, 0, 7);
  lua_pushliteral(state, "output");
  lua_setfield(state, -2, "role");
  lua_pushinteger(
      state, opened ? (lua_Integer)slot->format.frame_samples_per_channel : 0);
  lua_setfield(state, -2, "frame_samples");
  lua_pushboolean(state, opened);
  lua_setfield(state, -2, "opened");
  lua_pushinteger(state, opened ? slot->format.sample_rate_hz : 0u);
  lua_setfield(state, -2, "sample_rate");
  lua_pushinteger(state, opened ? slot->format.channels : 0u);
  lua_setfield(state, -2, "channels");
  lua_pushinteger(state, opened ? 16 : 0);
  lua_setfield(state, -2, "bits_per_sample");
  lua_pushinteger(
      state, opened ? (lua_Integer)h2_audio_pcm_frame_bytes(&slot->format) : 0);
  lua_setfield(state, -2, "bytes_per_frame");
  return 1;
}

static int audio_output_close(lua_State *state) {
  h2_lua_audio_track_slot_t *slot = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t generation = (uint32_t)lua_tointeger(state, lua_upvalueindex(2));
  if (slot != NULL && slot->generation == generation && slot->track != NULL) {
    h2_lua_job_t *job = slot->job;
    int result;
    h2_lua_audio_track_slot_flush_carry(slot);
    h2_lua_audio_track_slot_release_carry(slot, job->host->config.runtime->mem);
    result = h2_pal_audio_track_close(slot->track);
    slot->track = NULL;
    if (job->active_audio_track_count != 0u) {
      job->active_audio_track_count--;
    }
    if (job->active_audio_track_count == 0u) {
      h2_lua_job_release_audio_speaker(job);
    }
    if (result != H2_PAL_OK) {
      lua_pushnil(state);
      lua_pushliteral(state, "audio output: close failed");
      return 2;
    }
  }
  lua_pushboolean(state, 1);
  return 1;
}

static void set_audio_track_function(lua_State *state, const char *name,
                                     lua_CFunction function,
                                     h2_lua_audio_track_slot_t *slot) {
  lua_pushlightuserdata(state, slot);
  lua_pushinteger(state, (lua_Integer)slot->generation);
  lua_pushcclosure(state, function, 2);
  lua_setfield(state, -2, name);
}

static lua_Integer table_integer_field(lua_State *state, int table_index,
                                       const char *name,
                                       lua_Integer default_value) {
  lua_Integer value;
  lua_getfield(state, table_index, name);
  value = lua_isnil(state, -1) ? default_value : luaL_checkinteger(state, -1);
  lua_pop(state, 1);
  return value;
}

static h2_lua_job_t *audio_input_job(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint32_t generation = (uint32_t)lua_tointeger(state, lua_upvalueindex(2));
  if (job == NULL || !job->audio_mic_acquired ||
      job->audio_mic_generation != generation ||
      job->audio_mic_buffer == NULL) {
    return NULL;
  }
  return job;
}

/* Caps each h2_pal_audio_mic_read call so a long/indefinite caller timeout
 * still lets the job mutex (held across this call by the Lua worker) be
 * re-checked for host shutdown or job cancellation between slices, instead
 * of blocking the worker thread until a frame arrives. */
#define H2_LUA_AUDIO_INPUT_POLL_MS 50u

static int audio_input_read_frame(lua_State *state, h2_lua_job_t *job,
                                  uint32_t timeout_ms,
                                  h2_audio_frame_t *out_frame) {
  int result;
  uint64_t deadline_ms = h2_lua_now_ms(job->host) + (uint64_t)timeout_ms;
  uint32_t remaining_ms = timeout_ms;
  for (;;) {
    uint32_t slice_ms = remaining_ms < H2_LUA_AUDIO_INPUT_POLL_MS
                             ? remaining_ms
                             : H2_LUA_AUDIO_INPUT_POLL_MS;
    *out_frame = h2_audio_frame_for_buffer(job->audio_mic_buffer,
                                           job->audio_mic_buffer_capacity,
                                           job->audio_mic_format);
    result = h2_pal_audio_mic_read(job->host->config.runtime->audio, out_frame,
                                   slice_ms);
    if (result != H2_PAL_ERR_WOULD_BLOCK && result != H2_PAL_ERR_TIMEOUT) {
      break;
    }
    if (job->cancel_requested || atomic_load(&job->host->stopping) != 0) {
      lua_pushnil(state);
      lua_pushliteral(state, "audio input: cancelled");
      return 2;
    }
    if (slice_ms >= remaining_ms) {
      lua_pushnil(state);
      lua_pushliteral(state, "audio input: busy");
      return 2;
    }
    uint64_t now_ms = h2_lua_now_ms(job->host);
    if (now_ms >= deadline_ms) {
      lua_pushnil(state);
      lua_pushliteral(state, "audio input: busy");
      return 2;
    }
    uint64_t remaining = deadline_ms - now_ms;
    remaining_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
  }
  if (result != H2_PAL_OK || out_frame->bytes == 0u ||
      out_frame->bytes > job->audio_mic_buffer_capacity ||
      out_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      out_frame->channels == 0u || (out_frame->bytes & 1u) != 0u) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: read failed");
    return 2;
  }
  return 0;
}

static uint32_t audio_input_timeout(lua_State *state) {
  lua_Integer timeout = luaL_optinteger(state, 2, 0);
  if (timeout < 0 || (lua_Unsigned)timeout > UINT32_MAX) {
    luaL_argerror(state, 2, "timeout must be between 0 and UINT32_MAX");
  }
  return (uint32_t)timeout;
}

static int audio_input_read(lua_State *state) {
  h2_lua_job_t *job = audio_input_job(state);
  h2_audio_frame_t frame;
  int pushed;
  if (job == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: closed");
    return 2;
  }
  pushed =
      audio_input_read_frame(state, job, audio_input_timeout(state), &frame);
  if (pushed != 0) {
    return pushed;
  }
  lua_pushlstring(state, (const char *)frame.data, frame.bytes);
  return 1;
}

static int audio_input_level(lua_State *state) {
  h2_lua_job_t *job = audio_input_job(state);
  h2_audio_frame_t frame;
  const uint8_t *bytes;
  size_t sample_count;
  size_t i;
  double square_sum = 0.0;
  double difference_sum = 0.0;
  uint32_t peak = 0u;
  int pushed;
  if (job == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: closed");
    return 2;
  }
  pushed =
      audio_input_read_frame(state, job, audio_input_timeout(state), &frame);
  if (pushed != 0) {
    return pushed;
  }
  bytes = frame.data;
  sample_count = frame.bytes / sizeof(int16_t);
  for (i = 0u; i < sample_count; ++i) {
    int32_t sample = (int16_t)((uint16_t)bytes[i * 2u] |
                               ((uint16_t)bytes[i * 2u + 1u] << 8u));
    uint32_t magnitude =
        sample < 0 ? (uint32_t)(-(int64_t)sample) : (uint32_t)sample;
    double normalized = (double)sample / 32768.0;
    square_sum += normalized * normalized;
    if (i >= frame.channels) {
      size_t previous_index = i - frame.channels;
      int32_t previous =
          (int16_t)((uint16_t)bytes[previous_index * 2u] |
                    ((uint16_t)bytes[previous_index * 2u + 1u] << 8u));
      double difference = normalized - (double)previous / 32768.0;
      difference_sum += difference * difference;
    }
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  lua_pushnumber(state,
                 sample_count == 0u ? 0.0 : sqrt(square_sum / sample_count));
  lua_pushnumber(state, (lua_Number)peak / 32768.0);
  lua_pushnumber(state, square_sum <= 0.0
                            ? 0.0
                            : sqrt(difference_sum / (4.0 * square_sum)));
  return 3;
}

static int audio_input_info(lua_State *state) {
  h2_lua_job_t *job = audio_input_job(state);
  int opened = job != NULL;
  lua_createtable(state, 0, 7);
  lua_pushliteral(state, "input");
  lua_setfield(state, -2, "role");
  lua_pushboolean(state, opened);
  lua_setfield(state, -2, "opened");
  lua_pushinteger(state, opened ? job->audio_mic_format.sample_rate_hz : 0u);
  lua_setfield(state, -2, "sample_rate");
  lua_pushinteger(state, opened ? job->audio_mic_format.channels : 0u);
  lua_setfield(state, -2, "channels");
  lua_pushinteger(state, opened ? 16 : 0);
  lua_setfield(state, -2, "bits_per_sample");
  lua_pushinteger(
      state, opened
                 ? (lua_Integer)job->audio_mic_format.frame_samples_per_channel
                 : 0);
  lua_setfield(state, -2, "frame_samples");
  lua_pushinteger(state, opened ? (lua_Integer)h2_audio_pcm_frame_bytes(
                                      &job->audio_mic_format)
                                : 0);
  lua_setfield(state, -2, "bytes_per_frame");
  return 1;
}

static int audio_input_close(lua_State *state) {
  h2_lua_job_t *job = audio_input_job(state);
  if (job != NULL) {
    h2_lua_job_release_audio_mic(job);
    h2_pal_mem_free(job->host->config.runtime->mem, job->audio_mic_buffer);
    job->audio_mic_buffer = NULL;
    job->audio_mic_buffer_capacity = 0u;
    memset(&job->audio_mic_format, 0, sizeof(job->audio_mic_format));
  }
  lua_pushboolean(state, 1);
  return 1;
}

static void set_audio_input_function(lua_State *state, const char *name,
                                     lua_CFunction function,
                                     h2_lua_job_t *job) {
  lua_pushlightuserdata(state, job);
  lua_pushinteger(state, (lua_Integer)job->audio_mic_generation);
  lua_pushcclosure(state, function, 2);
  lua_setfield(state, -2, name);
}

static int audio_new_input(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_audio_info_t info = {0};
  size_t frame_bytes;
  int result;
  luaL_argcheck(state, lua_isnoneornil(state, 1), 1,
               "new_input takes no arguments; input format is fixed by the "
               "Runtime microphone");
  if (job->audio_mic_acquired || job->audio_mic_buffer != NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: already open");
    return 2;
  }
  result = h2_pal_audio_get_info(job->host->config.runtime->audio, &info);
  frame_bytes = h2_audio_pcm_frame_bytes(&info.mic_format);
  if (result != H2_PAL_OK || !info.available || !info.mic_supported ||
      info.mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      info.mic_format.frame_samples_per_channel == 0u || frame_bytes == 0u ||
      info.mic_format.frame_samples_per_channel > SIZE_MAX / frame_bytes) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: unavailable");
    return 2;
  }
  job->audio_mic_buffer_capacity =
      (size_t)info.mic_format.frame_samples_per_channel * frame_bytes;
  job->audio_mic_buffer = h2_pal_mem_alloc(job->host->config.runtime->mem,
                                           job->audio_mic_buffer_capacity);
  if (job->audio_mic_buffer == NULL) {
    job->audio_mic_buffer_capacity = 0u;
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: no memory");
    return 2;
  }
  job->audio_mic_format = info.mic_format;
  result = h2_lua_job_acquire_audio_mic(job);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(job->host->config.runtime->mem, job->audio_mic_buffer);
    job->audio_mic_buffer = NULL;
    job->audio_mic_buffer_capacity = 0u;
    memset(&job->audio_mic_format, 0, sizeof(job->audio_mic_format));
    lua_pushnil(state);
    lua_pushliteral(state, "audio input: unavailable");
    return 2;
  }
  job->audio_mic_generation++;
  if (job->audio_mic_generation == 0u) {
    job->audio_mic_generation = 1u;
  }
  lua_createtable(state, 0, 4);
  set_audio_input_function(state, "read", audio_input_read, job);
  set_audio_input_function(state, "level", audio_input_level, job);
  set_audio_input_function(state, "info", audio_input_info, job);
  set_audio_input_function(state, "close", audio_input_close, job);
  return 1;
}

static int audio_new_output(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer sample_rate;
  lua_Integer channels;
  lua_Integer bits;
  lua_Integer volume;
  h2_audio_track_config_t config;
  h2_audio_info_t info = {0};
  h2_lua_audio_track_slot_t *slot = NULL;
  size_t i;
  int result;
  luaL_checktype(state, 1, LUA_TTABLE);
  for (i = 0u; i < job->host->config.audio_track_capacity_per_job; ++i) {
    if (job->audio_tracks[i].track == NULL) {
      slot = &job->audio_tracks[i];
      break;
    }
  }
  if (slot == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio output: track limit reached");
    return 2;
  }
  sample_rate = table_integer_field(state, 1, "sample_rate", 16000);
  channels = table_integer_field(state, 1, "channels", 1);
  bits = table_integer_field(state, 1, "bits_per_sample", 16);
  volume = table_integer_field(state, 1, "volume", 90);
  if (sample_rate <= 0 || sample_rate > UINT32_MAX || channels <= 0 ||
      channels > UINT8_MAX || bits != 16 || volume < 0 || volume > 100) {
    lua_pushnil(state);
    lua_pushliteral(state, "audio output: invalid format");
    return 2;
  }
  slot->job = job;
  h2_lua_audio_track_slot_release_carry(slot, job->host->config.runtime->mem);
  slot->format = (h2_audio_pcm_format_t){
      .sample_rate_hz = (uint32_t)sample_rate,
      .frame_samples_per_channel = 0u,
      .channels = (uint8_t)channels,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  /* Mixer-backed Audio Systems only accept Tracks whose frame size equals the
   * device playback frame size, so adopt it from the Audio System info. */
  if (h2_pal_audio_get_info(job->host->config.runtime->audio, &info) ==
          H2_PAL_OK &&
      info.playback_supported) {
    slot->format.frame_samples_per_channel =
        info.playback_format.frame_samples_per_channel;
  }
  config = (h2_audio_track_config_t){
      .name = "lua-output",
      .format = slot->format,
      .volume_factor_milli = (uint32_t)volume * 10u,
      .buffer_frames = 8u,
  };
  result = h2_lua_job_acquire_audio_speaker(job);
  if (result == H2_PAL_OK) {
    result = h2_pal_audio_create_track(job->host->config.runtime->audio,
                                       &config, &slot->track);
    if (result == H2_PAL_OK && slot->track == NULL) {
      result = H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (result != H2_PAL_OK) {
    if (job->active_audio_track_count == 0u) {
      h2_lua_job_release_audio_speaker(job);
    }
    slot->track = NULL;
    lua_pushnil(state);
    lua_pushliteral(state, "audio output: unavailable");
    return 2;
  }
  slot->generation = job->next_audio_track_generation++;
  if (job->next_audio_track_generation == 0u) {
    job->next_audio_track_generation = 1u;
  }
  job->active_audio_track_count++;
  lua_createtable(state, 0, 3);
  set_audio_track_function(state, "write", audio_output_write, slot);
  set_audio_track_function(state, "info", audio_output_info, slot);
  set_audio_track_function(state, "close", audio_output_close, slot);
  return 1;
}

static int push_audio_proxy(lua_State *state, h2_lua_job_t *job) {
  lua_createtable(state, 0, 2);
  set_function(state, "new_input", audio_new_input, job);
  set_function(state, "new_output", audio_new_output, job);
  return 1;
}

static int require_proxy_result(lua_State *state, int result) {
  const char *message;
  if (result == 1) {
    return 1;
  }
  message = lua_tostring(state, -1);
  return luaL_error(state, "%s",
                    message == NULL ? "Runtime capability unavailable"
                                    : message);
}

static int open_display(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  return require_proxy_result(state, push_display_proxy(state, job));
}

static int open_lcd_touch(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const h2_pal_touch_api_t *touch = job->host->config.runtime->touch;
  if (touch == NULL || touch->vtable == NULL || touch->vtable->open == NULL ||
      touch->vtable->get_info == NULL || touch->vtable->poll_event == NULL ||
      touch->vtable->close == NULL) {
    return luaL_error(state, "Runtime Touch capability unavailable");
  }
  return require_proxy_result(state, push_touch_proxy(state, job));
}

static int open_audio(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const h2_pal_audio_api_t *audio = job->host->config.runtime->audio;
  if (audio == NULL || audio->vtable == NULL ||
      audio->vtable->start_speaker == NULL ||
      audio->vtable->stop_speaker == NULL ||
      audio->vtable->create_track == NULL) {
    return luaL_error(state, "Runtime Audio capability unavailable");
  }
  return require_proxy_result(state, push_audio_proxy(state, job));
}

static int open_custom(lua_State *state) {
  h2_lua_module_entry_t *entry = lua_touserdata(state, lua_upvalueindex(1));
  return entry->open_fn(state, entry->user);
}

static int local_module_path(const h2_lua_job_t *job, const char *module_name,
                             char *out_path, size_t path_capacity) {
  size_t prefix_length = strlen(job->require_root);
  size_t module_length = strlen(module_name);
  size_t i;
  size_t offset = 0u;
  if (module_length == 0u ||
      prefix_length + (prefix_length == 0u ? 0u : 1u) + module_length + 4u >=
          path_capacity) {
    return 0;
  }
  if (prefix_length != 0u) {
    memcpy(out_path, job->require_root, prefix_length);
    offset = prefix_length;
    out_path[offset++] = '/';
  }
  for (i = 0u; i < module_length; ++i) {
    unsigned char c = (unsigned char)module_name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '.')) {
      return 0;
    }
    out_path[offset++] = c == '.' ? '/' : (char)c;
  }
  memcpy(out_path + offset, ".lua", 5u);
  return 1;
}

static int load_local_module(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const char *module_name = luaL_checkstring(state, 1);
  char path[H2_LUA_PATH_MAX];
  char chunk_name[H2_LUA_PATH_MAX + 2u];
  const uint8_t *source = NULL;
  size_t source_size = 0u;
  uint8_t *owned_source = NULL;
  size_t i;
  int load_result;
  if (!local_module_path(job, module_name, path, sizeof(path))) {
    lua_pushfstring(state, "\n\tinvalid local module '%s'", module_name);
    return 1;
  }
  for (i = 0u; i < job->host->config.resource_count; ++i) {
    const h2_lua_resource_t *resource = &job->host->config.resources[i];
    const char *name = resource->name == NULL     ? ""
                       : resource->name[0] == '@' ? resource->name + 1
                                                  : resource->name;
    if (strcmp(name, path) == 0) {
      if (resource->source_size > job->host->config.source_limit_bytes) {
        return luaL_error(state, "local module source limit reached");
      }
      source = resource->source;
      source_size = resource->source_size;
      break;
    }
  }
  if (source == NULL && job->host->config.runtime->fs != NULL) {
    h2_pal_fs_stat_t stat;
    h2_pal_fs_file_t *file = NULL;
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_fs_stat(
        job->host->config.runtime->fs, path, &stat);
    size_t offset = 0u;
    if (result == H2_PAL_OK && !stat.is_dir &&
        stat.size > (uint64_t)(SIZE_MAX - 1u)) {
      return luaL_error(state, "local module source size is invalid");
    }
    if (result == H2_PAL_OK && !stat.is_dir &&
        stat.size > job->host->config.source_limit_bytes) {
      return luaL_error(state, "local module source limit reached");
    }
    if (result == H2_PAL_OK && !stat.is_dir) {
      owned_source = h2_pal_mem_alloc(job->host->config.runtime->mem,
                                      (size_t)stat.size + 1u);
      if (owned_source == NULL) {
        return luaL_error(state, "local module allocation failed");
      }
      result = (h2_pal_result_t)h2_pal_fs_open(
          job->host->config.runtime->fs, path, H2_PAL_FS_OPEN_READ, &file);
      while (result == H2_PAL_OK && offset < (size_t)stat.size) {
        size_t read_size = 0u;
        result = (h2_pal_result_t)h2_pal_fs_read(
            job->host->config.runtime->fs, file, owned_source + offset,
            (size_t)stat.size - offset, &read_size);
        if (result == H2_PAL_OK && read_size == 0u) {
          result = H2_PAL_ERR_TRUNCATED;
        }
        offset += read_size;
      }
      if (file != NULL) {
        h2_pal_result_t close_result = (h2_pal_result_t)h2_pal_fs_close(
            job->host->config.runtime->fs, file);
        if (result == H2_PAL_OK) {
          result = close_result;
        }
      }
      if (result == H2_PAL_OK) {
        source = owned_source;
        source_size = offset;
      }
    }
  }
  if (source == NULL) {
    h2_pal_mem_free(job->host->config.runtime->mem, owned_source);
    lua_pushfstring(state, "\n\tno confined module '%s'", path);
    return 1;
  }
  if (memchr(source, '\0', source_size) != NULL) {
    h2_pal_mem_free(job->host->config.runtime->mem, owned_source);
    return luaL_error(state, "local module contains embedded NUL");
  }
  (void)snprintf(chunk_name, sizeof(chunk_name), "@%s", path);
  load_result = luaL_loadbufferx(state, (const char *)source, source_size,
                                 chunk_name, "t");
  h2_pal_mem_free(job->host->config.runtime->mem, owned_source);
  if (load_result != LUA_OK) {
    return lua_error(state);
  }
  lua_pushstring(state, path);
  return 2;
}

static void add_preload(lua_State *state, const char *name, lua_CFunction open,
                        void *context) {
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "preload");
  lua_pushlightuserdata(state, context);
  lua_pushcclosure(state, open, 1);
  lua_setfield(state, -2, name);
  lua_pop(state, 2);
}

static int lua_link_available(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_pushboolean(state, job->host->link_hooks != NULL);
  return 1;
}

static int lua_link_unavailable(lua_State *state) {
  lua_pushnil(state);
  lua_pushliteral(state, "link: unavailable");
  return 2;
}

static int lua_link_close_unavailable(lua_State *state) {
  lua_pushboolean(state, 1);
  return 1;
}

static int lua_link_state_unavailable(lua_State *state) {
  lua_pushliteral(state, "unavailable");
  return 1;
}

static int lua_link_on(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer kind = luaL_checkinteger(state, 1);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  if (kind != H2_LUA_LINK_EVENT_CONNECTED &&
      kind != H2_LUA_LINK_EVENT_MESSAGE &&
      kind != H2_LUA_LINK_EVENT_DISCONNECTED &&
      kind != H2_LUA_LINK_EVENT_ERROR) {
    return luaL_argerror(state, 1, "expected a runtime.event.LINK_* kind");
  }
  return h2_lua_push_callback_register(
      state, job, H2_RUNTIME_COMPONENT_ID_NONE, (uint32_t)kind, 2);
}

/* Builtin `link` surface. Without an installed provider every operation fails
 * cleanly; h2_lua_link_enable() replaces host, join, send, send_unreliable,
 * write, read, close and state. */
static int open_link(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 11);
  set_function(state, "available", lua_link_available, job);
  set_function(state, "on", lua_link_on, job);
  set_function(state, "off", lua_runtime_off, job);
  set_function(state, "host", lua_link_unavailable, job);
  set_function(state, "join", lua_link_unavailable, job);
  set_function(state, "send", lua_link_unavailable, job);
  set_function(state, "send_unreliable", lua_link_unavailable, job);
  set_function(state, "write", lua_link_unavailable, job);
  set_function(state, "read", lua_link_unavailable, job);
  set_function(state, "close", lua_link_close_unavailable, job);
  set_function(state, "state", lua_link_state_unavailable, job);
  if (job->host->link_hooks != NULL) {
    job->host->link_hooks->open_module(job->host->link_user, state, job);
  }
  return 1;
}

h2_pal_result_t h2_lua_register_builtin_modules(h2_lua_job_t *job) {
  size_t i;
  lua_State *state;
  if (job == NULL || job->vm == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  state = job->vm->state;
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, lua_runtime_print, 1);
  lua_setglobal(state, "print");
  add_preload(state, "runtime", open_runtime, job);
  add_preload(state, "delay", open_delay, job);
  add_preload(state, "system", open_system, job);
  add_preload(state, "display", open_display, job);
  add_preload(state, "lcd_touch", open_lcd_touch, job);
  add_preload(state, "audio", open_audio, job);
  add_preload(state, "json", open_json, job);
  add_preload(state, "capability", open_capability, job);
  add_preload(state, "link", open_link, job);
  add_preload(state, "storage", h2_lua_open_storage, job);
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "searchers");
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, load_local_module, 1);
  lua_seti(state, -2, 2);
  lua_pop(state, 2);
  for (i = 0u; i < job->host->module_count; ++i) {
    add_preload(state, job->host->modules[i].name, open_custom,
                &job->host->modules[i]);
  }
  return H2_PAL_OK;
}
