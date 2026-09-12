#include "../runtime/h2_lua_internal.h"
#include "h2_lua_canvas.h"
#include "h2_lua_fpu_math.h"

#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"
#include "zlib.h"

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
  h2_lua_callback_t *callback;
  size_t callback_index;
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
  lua_pushvalue(state, 3);
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

  lua_createtable(state, 0, 6);
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

static h2_pal_result_t display_open(h2_lua_job_t *job) {
  size_t pixel_count;
  h2_pal_result_t result;
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

static void mark_dirty_tiles(h2_lua_job_t *job,int x,int y,int right,int bottom) {
  if(!job->dirty_tiles && !job->background_dirty_tiles)return;
  int columns=(job->display_info.width+15)/16;
  for(int ty=y/16;ty<=bottom/16;++ty)for(int tx=x/16;tx<=right/16;++tx) {
    size_t at=(size_t)ty*columns+tx;
    if(job->dirty_tiles)job->dirty_tiles[at]=1;
    if(job->background_dirty_tiles)job->background_dirty_tiles[at]=1;
  }
}
static void set_pixel(h2_lua_job_t *job, int x, int y, uint16_t color) {
  if (x >= 0 && y >= 0 && x < job->display_info.width &&
      y < job->display_info.height) {
    job->framebuffer[(size_t)y * (size_t)job->display_info.width + (size_t)x] =
        color;
    mark_dirty_tiles(job,x,y,x,y);
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
  mark_dirty_tiles(job,min_x,min_y,max_x,max_y);
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
  mark_dirty_tiles(job,0,0,job->display_info.width-1,job->display_info.height-1);
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
  job->dirty_valid = 1;
  job->dirty_min_x = 0;
  job->dirty_min_y = 0;
  job->dirty_max_x = job->display_info.width - 1;
  job->dirty_max_y = job->display_info.height - 1;
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

/* Scanline rules match the Lua pixel-geometry reference exactly: integer
 * polygon scanlines, half-open edges, ceil(left), inclusive floor(right).
 * Geometry remains authored in Lua; no antialiasing or texture conversion. */
static double check_geometry_number(lua_State *state, int index) {
  double v = luaL_checknumber(state, index);
  if (!isfinite(v) || fabs(v) > 100000.0)
    luaL_error(state, "invalid geometry coordinate");
  return v;
}
typedef struct h2_lua_cached_span { int32_t left,right,y; uint16_t color; } h2_lua_cached_span_t;
typedef struct h2_lua_span_cache {
  const void *source;
  size_t count,capacity;
  double offset;
  int top,bottom,valid;
} h2_lua_span_cache_t;
static void display_raster_polygon_capture(h2_lua_job_t *job, const double *px,
    const double *py, size_t n, uint16_t color, double offset, int top, int bottom,
    h2_lua_span_cache_t *cache) {
  double xs[128], lo = job->display_info.height, hi = 0;
  float x0[128],y0[128],slope[128],error[128];
  int edge_first[128],edge_end[128];
  for(size_t i=0;i<n;++i) {
    size_t j=(i+1)%n;
    edge_first[i]=(int)ceil(fmin(py[i],py[j]));
    edge_end[i]=(int)ceil(fmax(py[i],py[j]));
    x0[i]=(float)px[i];y0[i]=(float)py[i];
    float dx=(float)px[j]-x0[i],dy=(float)py[j]-y0[i];
    slope[i]=fabsf(dy)<1e-5f?0:dx/dy;
    error[i]=fabsf(dy)<1e-5f?1:32*FLT_EPSILON*(fabsf(x0[i])+fabsf(dx)*(1+(fabsf(y0[i])+job->display_info.height)/fabsf(dy)))+1e-7f;
  }
  for (size_t i = 0; i < n; ++i) {
    if (py[i] < lo) lo = py[i];
    if (py[i] > hi) hi = py[i];
  }
  int first = (int)fmax(top, ceil(lo)), last = (int)fmin(bottom - 1, floor(hi));
  for (int y = first; y <= last; ++y) {
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
      size_t j = (i + 1) % n;
      if (y>=edge_first[i] && y<edge_end[i]) {
        double x;
        if(px[j]==px[i])x=px[i];
        else {
          float fast=x0[i]+((float)y-y0[i])*slope[i],fraction=fast-floorf(fast);
          /* Quantized output only depends on floor/ceil. Fall back to the
           * original double expression whenever float error could cross an
           * integer boundary; ill-conditioned/offscreen edges also fall back. */
          if(error[i]<.25f && fraction>error[i] && fraction<1-error[i])x=fast;
          else x=px[i]+(y-py[i])*(px[j]-px[i])/(py[j]-py[i]);
        }
        size_t k = count++;
        while (k > 0 && xs[k-1] > x) { xs[k] = xs[k-1]; --k; }
        xs[k] = x;
      }
    }
    for (size_t i = 0; i + 1 < count; i += 2) {
      int left = (int)floor(ceil(xs[i]) + offset + .5);
      int width = (int)(floor(xs[i+1]) - ceil(xs[i]) + 1);
      if (width > 0) {
        fill_span(job, y, left, left + width - 1, color);
        mark_dirty_rect(job, left, y, width, 1);
        if(cache && cache->valid) {
          if(cache->count==cache->capacity)cache->valid=0;
          else {
            h2_lua_cached_span_t *spans=(h2_lua_cached_span_t *)(cache+1);
            spans[cache->count++]=(h2_lua_cached_span_t){left,left+width-1,y,color};
          }
        }
      }
    }
  }
}
static void display_raster_polygon(h2_lua_job_t *job,const double *px,
    const double *py,size_t n,uint16_t color,double offset,int top,int bottom) {
  display_raster_polygon_capture(job,px,py,n,color,offset,top,bottom,NULL);
}
static int display_fill_polygon(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double px[128], py[128];
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t n = lua_rawlen(state, 1);
  uint16_t color = check_color(state, 2);
  double offset = lua_isnoneornil(state, 3) ? 0 : check_geometry_number(state, 3);
  int top = (int)luaL_optinteger(state, 4, 0);
  int bottom = (int)luaL_optinteger(state, 5, job->display_info.height);
  if (!job->display_open || n < 3 || n > 128 || top < 0 || bottom > job->display_info.height || top > bottom)
    return luaL_error(state, "invalid fill_polygon");
  for (size_t i = 0; i < n; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1); px[i] = check_geometry_number(state, -1); lua_pop(state, 1);
    lua_rawgeti(state, -1, 2); py[i] = check_geometry_number(state, -1); lua_pop(state, 2);
  }
  display_raster_polygon(job, px, py, n, color, offset, top, bottom);
  return 0;
}
static int display_fill_ellipse(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double x = check_geometry_number(state, 1), y = check_geometry_number(state, 2);
  double rx = check_geometry_number(state, 3), ry = check_geometry_number(state, 4);
  uint16_t color = check_color(state, 5);
  double offset = lua_isnoneornil(state, 6) ? 0 : check_geometry_number(state, 6);
  int top = (int)luaL_optinteger(state, 7, 0);
  int bottom = (int)luaL_optinteger(state, 8, job->display_info.height);
  if (!job->display_open || rx < 0 || ry <= 0 || ry > 2048 || top < 0 || bottom > job->display_info.height || top > bottom)
    return luaL_error(state, "invalid fill_ellipse");
  for (double yy = -ry; yy <= ry; yy += 1) {
    int row = (int)floor(y + yy + .5);
    if (row < top || row >= bottom) continue;
    double ex = rx * sqrt(fmax(0, 1 - yy * yy / (ry * ry)));
    int left = (int)floor(x - ex + offset + .5);
    int width = (int)floor(2 * ex + 1 + .5);
    fill_span(job, row, left, left + width - 1, color);
    mark_dirty_rect(job, left, row, width, 1);
  }
  return 0;
}

static void display_clipped_line(h2_lua_job_t *job, double x, double y,
    double xx, double yy, uint16_t color, double offset, int top, int bottom) {
  x+=offset;xx+=offset;double dx=xx-x,dy=yy-y,lo=0,hi=1;
  /* Trivial accept: most rod/rope segments are already wholly inside the
   * viewport. Avoid four software double divisions without changing the
   * original endpoint rounding expressions below. */
  if(x<0 || xx<0 || x>job->display_info.width-1 || xx>job->display_info.width-1 ||
     y<top || yy<top || y>bottom-1 || yy>bottom-1) {
  if(dx==0) {if(x<0 || x>job->display_info.width-1) return;}
  else {double a=-x/dx,b=(job->display_info.width-1-x)/dx;
    if(a>b){double t=a;a=b;b=t;}lo=fmax(lo,a);hi=fmin(hi,b);}
  if(dy==0) {if(y<top || y>bottom-1) return;}
  else {double a=(top-y)/dy,b=(bottom-1-y)/dy;
    if(a>b){double t=a;a=b;b=t;}lo=fmax(lo,a);hi=fmin(hi,b);}
  }
  if(lo>hi) return;
  int x0=(int)floor(x+dx*lo+.5),y0=(int)floor(y+dy*lo+.5);
  int x1=(int)floor(x+dx*hi+.5),y1=(int)floor(y+dy*hi+.5);
  int ix=abs(x1-x0),iy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,error=ix+iy;
  mark_dirty_rect(job,x0<x1?x0:x1,y0<y1?y0:y1,ix+1,-iy+1);
  for(;;){write_pixel(job,x0,y0,color);if(x0==x1 && y0==y1) break;
    int twice=2*error;if(twice>=iy){error+=iy;x0+=sx;}if(twice<=ix){error+=ix;y0+=sy;}}
}
/* Perspective segment projection and near-plane clipping without transient Lua
 * point/edge tables. Camera is {center_x,horizon,focal,eye_height,near_z}. */
static void projected_segment(h2_lua_job_t *job,const double *from,const double *to,uint16_t color,const double *camera) {
  double a[3]={from[0],from[1],from[2]},b[3]={to[0],to[1],to[2]},near=camera[4];
  if(a[2]<near && b[2]<near)return;
  if(a[2]<near || b[2]<near) {
    double t=(near-a[2])/(b[2]-a[2]);
    double x=a[0]+(b[0]-a[0])*t,y=a[1]+(b[1]-a[1])*t;
    double *cut=a[2]<near?a:b;cut[0]=x;cut[1]=y;cut[2]=near;
  }
  display_clipped_line(job,camera[0]+camera[2]*a[0]/a[2],camera[1]+camera[2]*(camera[3]-a[1])/a[2],
      camera[0]+camera[2]*b[0]/b[2],camera[1]+camera[2]*(camera[3]-b[1])/b[2],color,0,0,job->display_info.height);
}
static int display_projected_line(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  double a[3],b[3],camera[5];
  luaL_checktype(s,1,LUA_TTABLE);luaL_checktype(s,2,LUA_TTABLE);luaL_checktype(s,4,LUA_TTABLE);
  for(int j=0;j<3;++j) {
    lua_rawgeti(s,1,j+1);a[j]=check_geometry_number(s,-1);lua_pop(s,1);
    lua_rawgeti(s,2,j+1);b[j]=check_geometry_number(s,-1);lua_pop(s,1);
  }
  for(int j=0;j<5;++j) {lua_rawgeti(s,4,j+1);camera[j]=check_geometry_number(s,-1);lua_pop(s,1);}
  uint16_t color=check_color(s,3);
  if(!job->display_open || camera[2]<=0 || camera[4]<.001)return luaL_error(s,"invalid projection camera");
  projected_segment(job,a,b,color,camera);
  return 0;
}
typedef struct path_gradient { double from[3],to[3],origin,divisor,bias,low,high; int multiply; } path_gradient_t;
static path_gradient_t read_path_gradient(lua_State *s,int at) {
  path_gradient_t g={0};luaL_checktype(s,at,LUA_TTABLE);
  const char *channels[]={"r","g","b"};
  for(int i=0;i<2;++i) {
    lua_rawgeti(s,at,i+1);luaL_checktype(s,-1,LUA_TTABLE);
    for(int j=0;j<3;++j) {lua_getfield(s,-1,channels[j]);double value=check_geometry_number(s,-1);lua_pop(s,1);
      if(value<0 || value>255)luaL_error(s,"invalid path gradient color");
      (i?g.to:g.from)[j]=value;}
    lua_pop(s,1);
  }
  double values[5];for(int i=0;i<5;++i) {lua_rawgeti(s,at,i+3);values[i]=check_geometry_number(s,-1);lua_pop(s,1);}
  g.origin=values[0];g.divisor=values[1];g.bias=values[2];g.low=values[3];g.high=values[4];
  lua_rawgeti(s,at,8);g.multiply=lua_toboolean(s,-1);lua_pop(s,1);
  if(fabs(g.divisor)<1e-6 || g.low<0 || g.high>1 || g.low>g.high)luaL_error(s,"invalid path gradient range");
  return g;
}
static uint16_t path_color(const path_gradient_t *g,double value) {
  double v=value-g->origin;
  double t=fmax(g->low,fmin(g->high,(g->multiply?v*g->divisor:v/g->divisor)+g->bias));
  uint8_t rgb[3];for(int i=0;i<3;++i)rgb[i]=(uint8_t)floor(g->from[i]*(1-t)+g->to[i]*t);
  return rgb_to_rgb565(rgb[0],rgb[1],rgb[2]);
}
/* Reverse-order 3D polyline with depth/height fades and a y=0 material split.
 * Gradient: {fromRGB,toRGB,origin,scale,bias,min,max,multiply=false}. */
static int display_depth_path(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  luaL_checktype(s,1,LUA_TTABLE);luaL_checktype(s,2,LUA_TTABLE);
  size_t n=lua_rawlen(s,1);double points[256][3],projected[256][2],camera[5];
  if(!job->display_open || n<2 || n>256)return luaL_error(s,"invalid depth path");
  for(int i=0;i<5;++i){lua_rawgeti(s,2,i+1);camera[i]=check_geometry_number(s,-1);lua_pop(s,1);}
  if(camera[2]<=0 || camera[4]<.001)return luaL_error(s,"invalid projection camera");
  path_gradient_t air=read_path_gradient(s,3),water=read_path_gradient(s,4);
  for(size_t i=0;i<n;++i) {lua_rawgeti(s,1,i+1);luaL_checktype(s,-1,LUA_TTABLE);
    for(int j=0;j<3;++j){lua_rawgeti(s,-1,j+1);points[i][j]=check_geometry_number(s,-1);lua_pop(s,1);}lua_pop(s,1);
    if(points[i][2]>=camera[4]) {
      projected[i][0]=camera[0]+camera[2]*points[i][0]/points[i][2];
      projected[i][1]=camera[1]+camera[2]*(camera[3]-points[i][1])/points[i][2];
    }
  }
  for(size_t k=n-1;k>0;--k) {
    const double *a=points[k-1],*b=points[k];
    if(a[1]*b[1]<0) {
      uint16_t dry=path_color(&air,(a[2]+b[2])*.5),wet=path_color(&water,fmin(a[1],b[1]));
      double u=-a[1]/(b[1]-a[1]),cut[]={a[0]+u*(b[0]-a[0]),0,a[2]+u*(b[2]-a[2])};
      projected_segment(job,a,cut,a[1]<0?wet:dry,camera);
      projected_segment(job,cut,b,b[1]<0?wet:dry,camera);
    } else {
      uint16_t color=a[1]<0 && b[1]<0?path_color(&water,fmin(a[1],b[1])):path_color(&air,(a[2]+b[2])*.5);
      if(a[2]>=camera[4] && b[2]>=camera[4])
        display_clipped_line(job,projected[k-1][0],projected[k-1][1],projected[k][0],projected[k][1],color,0,0,job->display_info.height);
      else projected_segment(job,a,b,color,camera);
    }
  }
  return 0;
}
typedef struct h2_lua_stroke_cache {
  size_t n,ends[256];
  double x[256],y[256],width[256];
  uint16_t color[256];
  unsigned char center[256];
  h2_lua_span_cache_t spans; /* span storage immediately follows */
} h2_lua_stroke_cache_t;
#define H2_LUA_STROKE_META "h2.display.stroke_cache"
typedef struct h2_lua_stroke_normals { size_t n; double data[]; } h2_lua_stroke_normals_t;
#define H2_LUA_NORMALS_META "h2.display.stroke_normals"
/* Use the FPU only when conservative vertex/intersection error bounds cannot
 * change any integer scanline or covered pixel. Near boundaries, use the
 * original double geometry below. This does not quantize the authored path. */
static int stroke_fast_quad(h2_lua_job_t *job,double ax,double ay,double bx,double by,double width,
    int top,int bottom,uint16_t color,double offset,h2_lua_span_cache_t *cache) {
  /* Validate and retain the integer spans in the same pass. Running the
   * general double polygon rasterizer after this test repeated every edge
   * division and scanline intersection, cancelling the FPU savings. */
  enum { MAX_FAST_ROWS=512 };
  if(bottom-top>MAX_FAST_ROWS)return 0;
  float left[MAX_FAST_ROWS],right[MAX_FAST_ROWS];
  int first_row=bottom,end_row=top;
  double extent=fmax(fmax(fabs(ax),fabs(ay)),fmax(fabs(bx),fabs(by)));
  if(extent>4096 || width>64 || width<=0)return 0;
  float dx=(float)(bx-ax),dy=(float)(by-ay),length=sqrtf(dx*dx+dy*dy);
  if(length<.02f)return 0;
  float scale=h2_lua_fpu_div((float)width*.5f,length),nx=-dy*scale,ny=dx*scale;
  float x[]={(float)ax+nx,(float)bx+nx,(float)bx-nx,(float)ax-nx};
  float y[]={(float)ay+ny,(float)by+ny,(float)by-ny,(float)ay-ny};
  float bound=64*FLT_EPSILON*((float)extent+(float)width+1);
  for(int i=0;i<4;++i) {
    float fraction=y[i]-floorf(y[i]);
    if(fraction<=bound || fraction>=1-bound)return 0;
  }
  /* A rod segment usually covers only a handful of rows. Clearing the entire
   * viewport-sized scratch array here multiplied that work by every segment. */
  first_row=(int)ceilf(fminf(fminf(y[0],y[1]),fminf(y[2],y[3])));
  end_row=(int)ceilf(fmaxf(fmaxf(y[0],y[1]),fmaxf(y[2],y[3])));
  if(first_row<top)first_row=top;
  if(end_row>bottom)end_row=bottom;
  if(first_row>=end_row)return 1;
  for(int row=first_row;row<end_row;++row){left[row-top]=FLT_MAX;right[row-top]=-FLT_MAX;}
  for(int i=0;i<4;++i) {
    int j=(i+1)%4,first=(int)ceilf(fminf(y[i],y[j])),end=(int)ceilf(fmaxf(y[i],y[j]));
    if(first<top)first=top;
    if(end>bottom)end=bottom;
    if(first>=end)continue;
    float ex=x[j]-x[i],ey=y[j]-y[i],denominator=fabsf(ey)-2*bound;
    if(denominator<=0)return 0;
    float slope=h2_lua_fpu_div(ex,ey),error=4*bound*(1+h2_lua_fpu_div(fabsf(ex),denominator))+16*FLT_EPSILON*(fabsf(x[i])+fabsf(ex)+1);
    if(error>=.25f)return 0;
    for(int row=first;row<end;++row) {
      float hit=x[i]+((float)row-y[i])*slope,fraction=hit-floorf(hit);
      if(fraction<=error || fraction>=1-error)return 0;
      if(hit<left[row-top])left[row-top]=hit;
      if(hit>right[row-top])right[row-top]=hit;
    }
  }
  for(int row=first_row;row<end_row;++row) {
    if(left[row-top]==FLT_MAX)continue;
    int edge=(int)ceilf(left[row-top]);
    int lo=(int)floor((double)edge+offset+.5),hi=lo+(int)floorf(right[row-top])-edge;
    if(hi<lo)continue;
    fill_span(job,row,lo,hi,color);mark_dirty_rect(job,lo,row,hi-lo+1,1);
    if(cache && cache->valid) {
      if(cache->count==cache->capacity)cache->valid=0;
      else {
        h2_lua_cached_span_t *spans=(h2_lua_cached_span_t *)(cache+1);
        spans[cache->count++]=(h2_lua_cached_span_t){lo,hi,row,color};
      }
    }
  }
  return 1;
}
static int display_stroke_path(lua_State *state) {
  h2_lua_job_t *job=lua_touserdata(state,lua_upvalueindex(1));
  int use_fast=lua_toboolean(state,8),fast_count=0;
  double x[256],y[256],width[256];uint16_t color[256];
  luaL_checktype(state,1,LUA_TTABLE);luaL_checktype(state,2,LUA_TTABLE);
  size_t n=lua_rawlen(state,1);
  double offset=lua_isnoneornil(state,4)?0:check_geometry_number(state,4);
  int top=(int)luaL_optinteger(state,5,0),bottom=(int)luaL_optinteger(state,6,job->display_info.height);
  if(!job->display_open || n<2 || n>256 || lua_rawlen(state,2)!=n-1 || top<0 || bottom>job->display_info.height || top>bottom)
    return luaL_error(state,"invalid stroke_path");
  int colors=lua_istable(state,3) && lua_rawlen(state,3)>0;
  uint16_t single=colors?0:check_color(state,3);
  if(colors && lua_rawlen(state,3)!=n-1) return luaL_error(state,"invalid path colors");
  for(size_t i=0;i<n;++i) {
    lua_rawgeti(state,1,(lua_Integer)i+1);luaL_checktype(state,-1,LUA_TTABLE);
    lua_rawgeti(state,-1,1);x[i]=check_geometry_number(state,-1);lua_pop(state,1);
    lua_rawgeti(state,-1,2);y[i]=check_geometry_number(state,-1);lua_pop(state,2);
    if(i+1<n) {
      lua_rawgeti(state,2,(lua_Integer)i+1);width[i]=check_geometry_number(state,-1);lua_pop(state,1);
      if(width[i]<0 || width[i]>1000) return luaL_error(state,"invalid path width");
      if(colors){lua_rawgeti(state,3,(lua_Integer)i+1);color[i]=check_color(state,-1);lua_pop(state,1);}else color[i]=single;
    }
  }
  /* Optional exact-geometry cache owned by the width/style table. A changed
   * coordinate, width, color or clip always invalidates it; no quantization. */
  static const char cache_key=0;
  h2_lua_stroke_cache_t *cache=NULL;
  if(lua_toboolean(state,7)) {
    lua_rawgetp(state,2,&cache_key);
    cache=luaL_testudata(state,-1,H2_LUA_STROKE_META);
    if(!cache) {
      lua_pop(state,1);
      cache=lua_newuserdatauv(state,sizeof(*cache)+2048*sizeof(h2_lua_cached_span_t),0);
      memset(cache,0,sizeof(*cache));cache->spans.capacity=2048;
      luaL_newmetatable(state,H2_LUA_STROKE_META);lua_setmetatable(state,-2);
      lua_pushvalue(state,-1);lua_rawsetp(state,2,&cache_key);
    }
    h2_lua_span_cache_t *c=&cache->spans;
    if(c->valid && cache->n==n && c->offset==offset && c->top==top && c->bottom==bottom &&
       !memcmp(cache->x,x,n*sizeof(double)) && !memcmp(cache->y,y,n*sizeof(double)) &&
       !memcmp(cache->width,width,(n-1)*sizeof(double)) && !memcmp(cache->color,color,(n-1)*sizeof(uint16_t))) {
      const h2_lua_cached_span_t *spans=(const h2_lua_cached_span_t *)(c+1);
      size_t first=0;
      for(size_t i=0;i+1<n;++i) {
        for(size_t j=first;j<cache->ends[i];++j) {
          const h2_lua_cached_span_t *p=spans+j;
          fill_span(job,p->y,p->left,p->right,p->color);mark_dirty_rect(job,p->left,p->y,p->right-p->left+1,1);
        }
        first=cache->ends[i];
        if(cache->center[i])display_clipped_line(job,x[i],y[i],x[i+1],y[i+1],color[i],offset,top,bottom);
      }
      lua_pushboolean(state,1);lua_pushinteger(state,0);return 2;
    }
    cache->n=n;memcpy(cache->x,x,n*sizeof(double));memcpy(cache->y,y,n*sizeof(double));
    memcpy(cache->width,width,(n-1)*sizeof(double));memcpy(cache->color,color,(n-1)*sizeof(uint16_t));
    c->offset=offset;c->top=top;c->bottom=bottom;c->count=0;c->valid=1;
  }
  /* Outlined paths draw the same points with two different widths. Share
   * exact, width-independent double normals for fallback segments. Ownership
   * follows the point table, and every coordinate is checked before reuse. */
  double *lengths=NULL,*normal_x=NULL,*normal_y=NULL;
  if(use_fast) {
    static const char normals_key=0;
    lua_rawgetp(state,1,&normals_key);
    h2_lua_stroke_normals_t *normals=luaL_testudata(state,-1,H2_LUA_NORMALS_META);
    int fresh=!normals || normals->n!=n;
    if(fresh) {
      lua_pop(state,1);normals=lua_newuserdatauv(state,sizeof(*normals)+5*n*sizeof(double),0);normals->n=n;
      luaL_newmetatable(state,H2_LUA_NORMALS_META);lua_setmetatable(state,-2);
      lua_pushvalue(state,-1);lua_rawsetp(state,1,&normals_key);
    }
    lengths=normals->data+2*n;normal_x=normals->data+3*n;normal_y=normals->data+4*n;
    if(fresh || memcmp(normals->data,x,n*sizeof(double)) || memcmp(normals->data+n,y,n*sizeof(double))) {
      memcpy(normals->data,x,n*sizeof(double));memcpy(normals->data+n,y,n*sizeof(double));
      for(size_t i=0;i+1<n;++i)lengths[i]=-1;
    }
  }
  for(size_t i=0;i+1<n;++i) {
    if(use_fast && stroke_fast_quad(job,x[i],y[i],x[i+1],y[i+1],width[i],top,bottom,color[i],offset,cache?&cache->spans:NULL)) {
      ++fast_count;
      display_clipped_line(job,x[i],y[i],x[i+1],y[i+1],color[i],offset,top,bottom);
      if(cache){cache->ends[i]=cache->spans.count;cache->center[i]=1;}
      continue;
    }
    double dx=x[i+1]-x[i],dy=y[i+1]-y[i],len,w=width[i],ux=0,uy=0;
    if(lengths && lengths[i]>=0) {len=lengths[i];ux=normal_x[i];uy=normal_y[i];}
    else {
      len=sqrt(dx*dx+dy*dy);
      if(len>=.01){ux=-dy/len;uy=dx/len;}
      if(lengths){lengths[i]=len;normal_x[i]=ux;normal_y[i]=uy;}
    }
    if(len<.01) {
      int left=(int)floor(x[i]-w/2+offset+.5),row=(int)floor(y[i]-w/2+.5),side=(int)floor(w+.5);
      for(int j=row;j<row+side;++j) if(j>=top && j<bottom){fill_span(job,j,left,left+side-1,color[i]);mark_dirty_rect(job,left,j,side,1);}
      if(cache)cache->spans.valid=0; /* rare degenerate squares use direct path */
      continue;
    }
    double nx=ux*w/2,ny=uy*w/2;
    double px[]={x[i]+nx,x[i+1]+nx,x[i+1]-nx,x[i]-nx};
    double py[]={y[i]+ny,y[i+1]+ny,y[i+1]-ny,y[i]-ny};
    display_raster_polygon_capture(job,px,py,4,color[i],offset,top,bottom,cache?&cache->spans:NULL);
    display_clipped_line(job,x[i],y[i],x[i+1],y[i+1],color[i],offset,top,bottom);
    if(cache){cache->ends[i]=cache->spans.count;cache->center[i]=1;}
  }
  lua_pushboolean(state,0);lua_pushinteger(state,fast_count);return 2;
}

typedef struct h2_lua_pixel_command { int kind,x,y,a,b; uint16_t color; } h2_lua_pixel_command_t;
/* VM-owned RGB565 snapshots of procedural art, not external image assets. */
typedef struct h2_lua_region_row { int left,right; size_t offset; } h2_lua_region_row_t;
typedef struct h2_lua_pixel_region { int width,height,masked; uint16_t key; size_t pixel_count; h2_lua_region_row_t rows[]; } h2_lua_pixel_region_t;
#define H2_LUA_REGION_META "h2.display.pixel_region"
static int display_capture_region(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  int x=check_pixel_number(s,1),y=check_pixel_number(s,2);
  int w=check_pixel_number(s,3),h=check_pixel_number(s,4);
  if(!job->display_open || x<0 || y<0 || w<=0 || h<=0 ||
     w>job->display_info.width || h>job->display_info.height ||
     x>job->display_info.width-w || y>job->display_info.height-h)
    return luaL_error(s,"invalid capture region");
  int masked=!lua_isnoneornil(s,5);uint16_t key=masked?check_color(s,5):0;
  size_t count=0;
  for(int row=0;row<h;++row) {
    int left=0,right=w;const uint16_t *p=job->framebuffer+(size_t)(y+row)*job->display_info.width+x;
    if(masked) {while(left<right && p[left]==key)++left;while(right>left && p[right-1]==key)--right;}
    count+=(size_t)(right-left);
  }
  size_t tiles=w==job->display_info.width && h==job->display_info.height ? (size_t)((w+15)/16)*((h+15)/16):0;
  size_t bytes=sizeof(h2_lua_pixel_region_t)+(size_t)h*sizeof(h2_lua_region_row_t)+count*2+tiles;
  h2_lua_pixel_region_t *r;
  if(!lua_isnoneornil(s,6)) {
    r=luaL_checkudata(s,6,H2_LUA_REGION_META);
    if(masked || r->masked || r->width!=w || r->height!=h || r->pixel_count!=count || lua_rawlen(s,6)<bytes)
      return luaL_error(s,"capture reuse requires a matching opaque region");
    lua_pushvalue(s,6);
  } else r=lua_newuserdatauv(s,bytes,0);
  r->width=w;r->height=h;r->masked=masked;r->key=key;r->pixel_count=count;
  uint16_t *pixels=(uint16_t *)(r->rows+h);size_t offset=0;
  if(tiles)memset(pixels+count,0,tiles);
  /* Omit transparent margins entirely. Opaque or different-key replay below
   * reconstructs those margins, so capture/draw semantics remain lossless. */
  for(int row=0;row<h;++row) {
    int left=0,right=w;const uint16_t *p=job->framebuffer+(size_t)(y+row)*job->display_info.width+x;
    if(masked) {while(left<right && p[left]==key)++left;while(right>left && p[right-1]==key)--right;}
    r->rows[row]=(h2_lua_region_row_t){left,right,offset};
    memcpy(pixels+offset,p+left,(size_t)(right-left)*2);offset+=(size_t)(right-left);
  }
  luaL_newmetatable(s,H2_LUA_REGION_META);lua_setmetatable(s,-2);return 1;
}
static int display_draw_region(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  h2_lua_pixel_region_t *r=luaL_checkudata(s,1,H2_LUA_REGION_META);
  int x=check_pixel_number(s,2),y=check_pixel_number(s,3);
  lua_Integer top_arg=luaL_optinteger(s,4,0),bottom_arg=luaL_optinteger(s,5,job->display_info.height);
  if(!job->display_open || x<-100000 || x>100000 || y<-100000 || y>100000 ||
     top_arg<0 || bottom_arg>job->display_info.height || top_arg>bottom_arg)
    return luaL_error(s,"invalid region clip");
  int top=(int)top_arg,bottom=(int)bottom_arg,masked=!lua_isnoneornil(s,6);
  uint16_t key=masked?check_color(s,6):0;
  int left=x<0?0:x,right=x+r->width;
  if(right>job->display_info.width)right=job->display_info.width;
  int first=y>top?y:top,last=y+r->height<bottom?y+r->height:bottom;
  if(left>=right || first>=last)return 0;
  const uint16_t *pixels=(const uint16_t *)(r->rows+r->height);
  for(int row=first;row<last;++row) {
    int l=left,rr=right;
    const h2_lua_region_row_t *span=r->rows+(row-y);
    int start=x+span->left,end=x+span->right;
    if(r->masked && (!masked || key!=r->key)) {
      fill_span(job,row,left,(start<right?start:right)-1,r->key);
      fill_span(job,row,end>left?end:left,right-1,r->key);
    }
    if(l<start)l=start;
    if(rr>end)rr=end;
    if(l>=rr)continue;
    const uint16_t *src=pixels+span->offset+(l-start);
    uint16_t *dst=job->framebuffer+(size_t)row*job->display_info.width+l;
    if(!masked)memcpy(dst,src,(size_t)(rr-l)*2);
    else for(int col=0;col<rr-l;++col)if(src[col]!=key)dst[col]=src[col];
  }
  mark_dirty_rect(job,left,first,right-left,last-first);return 0;
}
static int display_restore_background(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  h2_lua_pixel_region_t *r=luaL_checkudata(s,1,H2_LUA_REGION_META);
  if(!job->display_open || r->masked || r->width!=job->display_info.width || r->height!=job->display_info.height)
    return luaL_error(s,"background must be an opaque full-screen capture");
  int width=r->width,height=r->height,columns=(width+15)/16,rows=(height+15)/16;
  const uint16_t *pixels=(const uint16_t *)(r->rows+height);
  uint8_t *mask=(uint8_t *)(pixels+r->pixel_count);
  if(job->active_background==r && job->presented_valid) {
    for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx)if(mask[ty*columns+tx]) {
      int x=tx*16,y=ty*16,w=width-x<16?width-x:16,h=height-y<16?height-y:16;
      for(int row=y;row<y+h;++row)memcpy(job->framebuffer+(size_t)row*width+x,pixels+(size_t)row*width+x,(size_t)w*2);
      mark_dirty_rect(job,x,y,w,h);
    }
  } else {
    memcpy(job->framebuffer,pixels,(size_t)width*height*2);
    mark_dirty_rect(job,0,0,width,height);
  }
  if(job->active_background!=r) {
    if(job->active_background)luaL_unref(s,LUA_REGISTRYINDEX,job->background_ref);
    lua_pushvalue(s,1);job->background_ref=luaL_ref(s,LUA_REGISTRYINDEX);
    job->active_background=r;
  }
  job->background_dirty_tiles=mask;
  memset(mask,0,(size_t)columns*rows);
  return 0;
}
typedef struct h2_lua_pixel_commands { size_t count; } h2_lua_pixel_commands_t;
#define H2_LUA_COMMANDS_META "h2.display.pixel_commands"
static int display_compile_commands(lua_State *state) {
  luaL_checktype(state,1,LUA_TTABLE);size_t n=lua_rawlen(state,1);
  if(n>16384) return luaL_error(state,"too many pixel commands");
  h2_lua_pixel_commands_t *list=lua_newuserdatauv(state,sizeof(*list)+n*sizeof(h2_lua_pixel_command_t),0);
  list->count=n;h2_lua_pixel_command_t *commands=(h2_lua_pixel_command_t *)(list+1);
  for(size_t i=0;i<n;++i) {
    lua_rawgeti(state,1,(lua_Integer)i+1);luaL_checktype(state,-1,LUA_TTABLE);
    int values[5];
    for(int j=0;j<5;++j) {lua_rawgeti(state,-1,j+1);values[j]=check_pixel_number(state,-1);lua_pop(state,1);
      if(values[j]<-100000 || values[j]>100000) return luaL_error(state,"pixel command coordinate out of range");}
    lua_rawgeti(state,-1,6);uint16_t color=check_color(state,-1);lua_pop(state,2);
    if(values[0]!=0 && values[0]!=1) return luaL_error(state,"invalid pixel command");
    if(values[0]==0 && (values[3]<0 || values[4]<0)) return luaL_error(state,"invalid command rectangle");
    commands[i]=(h2_lua_pixel_command_t){values[0],values[1],values[2],values[3],values[4],color};
  }
  luaL_newmetatable(state,H2_LUA_COMMANDS_META);lua_setmetatable(state,-2);return 1;
}
static int display_draw_commands(lua_State *state) {
  h2_lua_job_t *job=lua_touserdata(state,lua_upvalueindex(1));
  h2_lua_pixel_commands_t *list=luaL_checkudata(state,1,H2_LUA_COMMANDS_META);
  int top=(int)luaL_optinteger(state,2,0),bottom=(int)luaL_optinteger(state,3,job->display_info.height);
  if(!job->display_open || top<0 || bottom>job->display_info.height || top>bottom) return luaL_error(state,"invalid command clip");
  const h2_lua_pixel_command_t *commands=(h2_lua_pixel_command_t *)(list+1);
  for(size_t i=0;i<list->count;++i) {
    const h2_lua_pixel_command_t *c=commands+i;
    if(c->kind==0) {
      int first=c->y>top?c->y:top,last=c->y+c->b<bottom?c->y+c->b:bottom;
      for(int y=first;y<last;++y) fill_span(job,y,c->x,c->x+c->a-1,c->color);
      mark_dirty_rect(job,c->x,first,c->a,last-first);
    } else display_clipped_line(job,c->x,c->y,c->a,c->b,c->color,0,top,bottom);
  }
  return 0;
}

typedef struct h2_lua_mesh_face { uint32_t first; uint16_t count, color; } h2_lua_mesh_face_t;
typedef struct h2_lua_pixel_mesh {
  uint32_t faces,vertices;
  double x,y,scale,angle;
  int position_valid;
} h2_lua_pixel_mesh_t;
#define H2_LUA_PIXEL_MESH_META "h2.display.pixel_mesh"
static int display_compile_mesh(lua_State *state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1), total = 0;
  if (count == 0 || count > 4096) return luaL_error(state, "invalid mesh face count");
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i+1); luaL_checktype(state, -1, LUA_TTABLE);
    lua_getfield(state, -1, "p"); luaL_checktype(state, -1, LUA_TTABLE);
    size_t n = lua_rawlen(state, -1);
    if (n < 3 || n > 128 || total+n > 65536) return luaL_error(state, "invalid mesh vertices");
    total += n; lua_pop(state, 2);
  }
  h2_lua_pixel_mesh_t *mesh = lua_newuserdatauv(state, sizeof(*mesh)+count*sizeof(h2_lua_mesh_face_t)+total*2*(sizeof(double)+sizeof(int32_t)), 0);
  mesh->faces = (uint32_t)count; mesh->vertices = (uint32_t)total;mesh->position_valid=0;
  h2_lua_mesh_face_t *faces = (h2_lua_mesh_face_t *)(mesh+1);
  double *vertices = (double *)(faces+count); size_t at = 0;
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i+1);
    lua_getfield(state, -1, "c"); faces[i].color = check_color(state, -1); lua_pop(state, 1);
    lua_getfield(state, -1, "p"); size_t n = lua_rawlen(state, -1);
    faces[i].first=(uint32_t)at; faces[i].count=(uint16_t)n;
    for (size_t j = 0; j < n; ++j) {
      lua_rawgeti(state, -1, (lua_Integer)j+1); luaL_checktype(state, -1, LUA_TTABLE);
      lua_rawgeti(state, -1, 1); vertices[at*2]=check_geometry_number(state,-1);lua_pop(state,1);
      lua_rawgeti(state, -1, 2); vertices[at*2+1]=check_geometry_number(state,-1);lua_pop(state,2);
      ++at;
    }
    lua_pop(state,2);
  }
  luaL_newmetatable(state,H2_LUA_PIXEL_MESH_META);lua_setmetatable(state,-2);
  return 1;
}
static int display_draw_mesh(lua_State *state) {
  static const char cache_key;
  h2_lua_job_t *job=lua_touserdata(state,lua_upvalueindex(1));
  h2_lua_pixel_mesh_t *mesh=luaL_checkudata(state,1,H2_LUA_PIXEL_MESH_META);
  double x=check_geometry_number(state,2),y=check_geometry_number(state,3);
  double scale=check_geometry_number(state,4),angle=check_geometry_number(state,5);
  double offset=lua_isnoneornil(state,6)?0:check_geometry_number(state,6);
  int top=(int)luaL_optinteger(state,7,0),bottom=(int)luaL_optinteger(state,8,job->display_info.height);
  if(!job->display_open || scale<=0 || scale>100 || top<0 || bottom>job->display_info.height || top>bottom)
    return luaL_error(state,"invalid draw_mesh");
  h2_lua_mesh_face_t *faces=(h2_lua_mesh_face_t *)(mesh+1);
  double *vertices=(double *)(faces+mesh->faces);
  int32_t *positions=(int32_t *)(vertices+mesh->vertices*2);
  int changed=!mesh->position_valid;
  if(!mesh->position_valid || mesh->x!=x || mesh->y!=y || mesh->scale!=scale || mesh->angle!=angle) {
    double ca=cos(angle),sa=sin(angle),grid=scale>3?2:1;
    for(size_t i=0;i<mesh->vertices;++i) {
      double vx=vertices[i*2],vy=vertices[i*2+1];
      float fx=((float)x+((float)vx*(float)ca-(float)vy*(float)sa)*(float)scale)/(float)grid;
      float fy=((float)y+((float)vx*(float)sa+(float)vy*(float)ca)*(float)scale)/(float)grid;
      float error=64*FLT_EPSILON*(fabsf((float)x)+fabsf((float)y)+(fabsf((float)vx)+fabsf((float)vy))*(float)scale+1);
      int32_t px,py;
      if(error<.25f && fabsf(fx-floorf(fx)-.5f)>error)px=(int32_t)(floorf(fx+.5f)*(float)grid);
      else px=(int32_t)(floor((x+(vx*ca-vy*sa)*scale)/grid+.5)*grid);
      if(error<.25f && fabsf(fy-floorf(fy)-.5f)>error)py=(int32_t)(floorf(fy+.5f)*(float)grid);
      else py=(int32_t)(floor((y+(vx*sa+vy*ca)*scale)/grid+.5)*grid);
      if(!mesh->position_valid || positions[i*2]!=px || positions[i*2+1]!=py)changed=1;
      positions[i*2]=px;positions[i*2+1]=py;
    }
    mesh->x=x;mesh->y=y;mesh->scale=scale;mesh->angle=angle;mesh->position_valid=1;
  }
  /* One VM-local scanline cache, independent of the number of reel models.
   * This stores procedural drawing commands, never a bitmap or a texture. */
  lua_rawgetp(state,LUA_REGISTRYINDEX,&cache_key);
  h2_lua_span_cache_t *cache=lua_isuserdata(state,-1)?lua_touserdata(state,-1):NULL;
  if(cache && cache->valid && cache->source==mesh && !changed && cache->offset==offset && cache->top==top && cache->bottom==bottom) {
    h2_lua_cached_span_t *spans=(h2_lua_cached_span_t *)(cache+1);
    for(size_t i=0;i<cache->count;++i) {
      const h2_lua_cached_span_t *p=spans+i;
      fill_span(job,p->y,p->left,p->right,p->color);mark_dirty_rect(job,p->left,p->y,p->right-p->left+1,1);
    }
    return 0;
  }
  size_t capacity=0;
  for(size_t i=0;i<mesh->faces;++i) {
    const h2_lua_mesh_face_t *f=faces+i;int lo=bottom,hi=top;
    for(size_t j=0;j<f->count;++j) {int py=positions[(f->first+j)*2+1];if(py<lo)lo=py;if(py>hi)hi=py;}
    if(lo<top)lo=top;
    if(hi>=bottom)hi=bottom-1;
    if(hi>=lo)capacity+=(size_t)(hi-lo+1)*(f->count/2);
    if(capacity>16384){capacity=16384;break;}
  }
  if(!cache || cache->capacity<capacity) {
    lua_pop(state,1);
    cache=lua_newuserdatauv(state,sizeof(*cache)+capacity*sizeof(h2_lua_cached_span_t),1);
    memset(cache,0,sizeof(*cache));cache->capacity=capacity;
    lua_pushvalue(state,-1);lua_rawsetp(state,LUA_REGISTRYINDEX,&cache_key);
  }
  lua_pushvalue(state,1);lua_setiuservalue(state,-2,1);
  cache->source=mesh;cache->offset=offset;cache->top=top;cache->bottom=bottom;cache->count=0;cache->valid=1;
  double px[128],py[128];
  for(size_t i=0;i<mesh->faces;++i) {
    const h2_lua_mesh_face_t *f=faces+i;
    for(size_t j=0;j<f->count;++j) {px[j]=positions[(f->first+j)*2];py[j]=positions[(f->first+j)*2+1];}
    display_raster_polygon_capture(job,px,py,f->count,f->color,offset,top,bottom,cache);
  }
  return 0;
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

/* Compact game-owned bitmap format used by embedded Lua Apps.
 *
 *   byte 0..3  "H2A4"
 *   byte 4..5  source width, little endian
 *   byte 6..7  source height, little endian
 *   byte 8..   little-endian ARGB4444 pixels, row major
 *
 * Assets remain immutable Host resources and never enter Lua VM memory. The
 * renderer performs clipping, nearest-neighbour scaling, and RGB565 alpha
 * blending directly into the job framebuffer.
 */
static const h2_lua_resource_t *display_find_resource(const h2_lua_job_t *job,
                                                      const char *name) {
  size_t index;
  for (index = 0u; index < job->host->config.resource_count; ++index) {
    const h2_lua_resource_t *resource = &job->host->config.resources[index];
    if (resource->name != NULL && strcmp(resource->name, name) == 0) {
      return resource;
    }
  }
  return NULL;
}

static unsigned atlas_u16(const uint8_t *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8u);
}

static uint32_t atlas_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

/* H2LF: little-endian width,height,lamp-count,level-count after magic;
 * each lamp has x,y,w,h then offset/size pairs for zlib RGB888 gain tiles.
 * Animation lives in Lua. Static gain samples retain subpixel coverage and
 * nonlinear glow; interpolation is performed before a single RGB565 resolve.
 * Scratch is a VM-budgeted userdata, retained per job and freed with its VM.
 */
static int display_draw_light_atlas(lua_State *state) {
  static char scratch_key;
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const h2_lua_resource_t *resource =
      display_find_resource(job, luaL_checkstring(state, 1));
  luaL_checktype(state, 2, LUA_TTABLE);
  if (!job->display_open || resource == NULL || resource->source_size < 12u ||
      memcmp(resource->source, "H2LF", 4u) != 0) {
    return luaL_error(state, "invalid light atlas");
  }
  const uint8_t *data = resource->source;
  unsigned width = atlas_u16(data + 4), height = atlas_u16(data + 6);
  unsigned count = atlas_u16(data + 8), levels = atlas_u16(data + 10);
  int transformed = !lua_isnoneornil(state,3);
  double scale=1, offset_x=0, offset_y=0;
  if(transformed) {
    luaL_checktype(state,3,LUA_TTABLE);
    double *values[]={&scale,&offset_x,&offset_y};
    for(int i=0;i<3;i++){lua_rawgeti(state,3,i+1);*values[i]=luaL_checknumber(state,-1);lua_pop(state,1);}
    if(!isfinite(scale) || scale<=0 || scale>8 || !isfinite(offset_x) || !isfinite(offset_y) ||
        fabs(offset_x)>1000000 || fabs(offset_y)>1000000)return luaL_error(state,"invalid light atlas transform");
  }
  if ((!transformed && (width != (unsigned)job->display_info.width ||
      height != (unsigned)job->display_info.height)) ||
      width == 0u || height == 0u || width > 4096u || height > 4096u ||
      count == 0u || count > 256u || levels < 2u || levels > 257u ||
      lua_rawlen(state, 2) != count) {
    return luaL_error(state, "invalid light atlas dimensions or gains");
  }
  size_t record_size = 8u + levels * 8u;
  size_t directory_end = 12u + count * record_size;
  if (directory_end > resource->source_size) {
    return luaL_error(state, "truncated light atlas directory");
  }
  double gains[256];
  size_t tile_capacity = 0u;
  for (unsigned i = 0u; i < count; ++i) {
    const uint8_t *record = data + 12u + i * record_size;
    unsigned x = atlas_u16(record), y = atlas_u16(record + 2);
    unsigned w = atlas_u16(record + 4), h = atlas_u16(record + 6);
    if (w == 0u || h == 0u || x >= width || y >= height ||
        w > width - x || h > height - y) {
      return luaL_error(state, "invalid light atlas tile bounds");
    }
    size_t tile_size = (size_t)w * h * 3u;
    if (tile_size > tile_capacity) tile_capacity = tile_size;
    for (unsigned level = 0u; level < levels; ++level) {
      const uint8_t *entry = record + 8u + level * 8u;
      size_t offset = atlas_u32(entry), length = atlas_u32(entry + 4);
      if (offset < directory_end || offset > resource->source_size ||
          length == 0u || length > resource->source_size - offset) {
        return luaL_error(state, "invalid light atlas payload bounds");
      }
    }
    lua_rawgeti(state, 2, i + 1u);
    gains[i] = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    if (!isfinite(gains[i]) || gains[i] < 0.0 || gains[i] > 1.0) {
      return luaL_error(state, "light atlas gains must be finite and 0..1");
    }
  }
  size_t pixel_count = (size_t)width * height;
  size_t frame_size = pixel_count * 3u;
  size_t scratch_size = frame_size + tile_capacity * 2u;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &scratch_key);
  if (!lua_isuserdata(state, -1) || lua_rawlen(state, -1) < scratch_size) {
    lua_pop(state, 1);
    /* Drop the obsolete buffer before allocating: a larger atlas can require
     * GC to reclaim it under the job's VM memory budget. */
    lua_pushnil(state);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &scratch_key);
    lua_newuserdatauv(state, scratch_size, 0);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &scratch_key);
  }
  uint8_t *frame = lua_touserdata(state, -1);
  uint8_t *low = frame + frame_size, *high = low + tile_capacity;
  if(transformed)memset(frame,0,frame_size);
  else for (size_t p = 0u; p < pixel_count; ++p) {
    uint16_t color = job->framebuffer[p];
    unsigned r = color >> 11u, g = (color >> 5u) & 63u, b = color & 31u;
    frame[p * 3u] = (uint8_t)((r << 3u) | (r >> 2u));
    frame[p * 3u + 1u] = (uint8_t)((g << 2u) | (g >> 4u));
    frame[p * 3u + 2u] = (uint8_t)((b << 3u) | (b >> 2u));
  }
  for (unsigned i = 0u; i < count; ++i) {
    if (gains[i] == 0.0) continue;
    const uint8_t *record = data + 12u + i * record_size;
    unsigned x = atlas_u16(record), y = atlas_u16(record + 2);
    unsigned w = atlas_u16(record + 4), h = atlas_u16(record + 6);
    double position = gains[i] * (levels - 1u);
    unsigned level = (unsigned)position;
    if (level >= levels - 1u) level = levels - 2u;
    unsigned fraction = (unsigned)((position - level) * 256.0 + 0.5);
    for (unsigned sample = 0u; sample < 2u; ++sample) {
      const uint8_t *entry = record + 8u + (level + sample) * 8u;
      uLongf length = (uLongf)w * h * 3u;
      int result = uncompress(sample == 0u ? low : high, &length,
                              data + atlas_u32(entry), atlas_u32(entry + 4));
      if (result != Z_OK || length != (uLongf)w * h * 3u) {
        return luaL_error(state, "corrupt light atlas tile");
      }
    }
    for (unsigned row = 0u; row < h; ++row) {
      size_t source = (size_t)row * w * 3u;
      size_t destination = ((size_t)(y + row) * width + x) * 3u;
      for (unsigned c = 0u; c < w * 3u; ++c) {
        unsigned value = ((unsigned)low[source + c] * (256u - fraction) +
                          (unsigned)high[source + c] * fraction + 128u) >> 8u;
        value += frame[destination + c];
        frame[destination + c] = (uint8_t)(value > 255u ? 255u : value);
      }
    }
  }
  if(transformed) {
    int dw=job->display_info.width,dh=job->display_info.height;
    for(int y=0;y<dh;y++)for(int x=0;x<dw;x++) {
      double sx=(x+.5-offset_x)/scale-.5,sy=(y+.5-offset_y)/scale-.5;
      if(sx<-.5 || sy<-.5 || sx>=width-.5 || sy>=height-.5)continue;
      sx=fmax(0,fmin(width-1,sx));sy=fmax(0,fmin(height-1,sy));
      unsigned ix=(unsigned)sx,iy=(unsigned)sy,ix1=ix+1<width?ix+1:ix,iy1=iy+1<height?iy+1:iy;
      double u=sx-ix,v=sy-iy,weights[]={(1-u)*(1-v),u*(1-v),(1-u)*v,u*v};
      size_t samples[]={((size_t)iy*width+ix)*3,((size_t)iy*width+ix1)*3,
          ((size_t)iy1*width+ix)*3,((size_t)iy1*width+ix1)*3};
      uint16_t old=job->framebuffer[(size_t)y*dw+x];
      unsigned r=old>>11,g=(old>>5)&63,b=old&31;
      unsigned rgb[]={(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)};
      for(int c=0;c<3;c++) {
        double value=rgb[c];for(int i=0;i<4;i++)value+=weights[i]*frame[samples[i]+c];
        rgb[c]=(unsigned)fmin(255,floor(value+.5));
      }
      job->framebuffer[(size_t)y*dw+x]=(uint16_t)((rgb[0]>>3)<<11|(rgb[1]>>2)<<5|(rgb[2]>>3));
    }
  } else for (size_t p = 0u; p < pixel_count; ++p) {
    job->framebuffer[p] = (uint16_t)((frame[p * 3u] >> 3u) << 11u |
        (frame[p * 3u + 1u] >> 2u) << 5u | (frame[p * 3u + 2u] >> 3u));
  }
  mark_dirty_rect(job, 0, 0, job->display_info.width, job->display_info.height);
  lua_pop(state, 1);
  return 0;
}

static int display_draw_asset(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const char *name = luaL_checkstring(state, 1);
  int x = check_pixel_number(state, 2);
  int y = check_pixel_number(state, 3);
  const h2_lua_resource_t *resource;
  const uint8_t *bytes;
  size_t source_pixel_count;
  int source_width;
  int source_height;
  int destination_width;
  int destination_height;
  unsigned opacity = 255u;
  int destination_y;

  if (!job->display_open || !point_is_bounded(job, x, y)) {
    return luaL_error(state, "invalid draw_asset");
  }
  resource = display_find_resource(job, name);
  if (resource == NULL || resource->source_size < 8u) {
    return luaL_error(state, "unknown display asset '%s'", name);
  }
  bytes = resource->source;
  if (memcmp(bytes, "H2A4", 4u) != 0) {
    return luaL_error(state, "invalid display asset '%s'", name);
  }
  source_width = (int)((unsigned)bytes[4] | ((unsigned)bytes[5] << 8u));
  source_height = (int)((unsigned)bytes[6] | ((unsigned)bytes[7] << 8u));
  if (source_width <= 0 || source_height <= 0 ||
      (size_t)source_width > SIZE_MAX / (size_t)source_height) {
    return luaL_error(state, "invalid display asset '%s'", name);
  }
  source_pixel_count = (size_t)source_width * (size_t)source_height;
  if (source_pixel_count > (resource->source_size - 8u) / 2u ||
      resource->source_size != 8u + source_pixel_count * 2u) {
    return luaL_error(state, "truncated display asset '%s'", name);
  }

  destination_width = source_width;
  destination_height = source_height;
  if (!lua_isnoneornil(state, 4)) {
    lua_Integer requested_opacity;
    luaL_checktype(state, 4, LUA_TTABLE);
    lua_getfield(state, 4, "width");
    if (!lua_isnil(state, -1))
      destination_width = check_pixel_number(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 4, "height");
    if (!lua_isnil(state, -1))
      destination_height = check_pixel_number(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 4, "opacity");
    requested_opacity =
        lua_isnil(state, -1) ? 255 : luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (requested_opacity < 0 || requested_opacity > 255) {
      return luaL_error(state, "display asset opacity must be 0..255");
    }
    opacity = (unsigned)requested_opacity;
  }
  if (destination_width <= 0 || destination_height <= 0 ||
      destination_width > job->display_info.width ||
      destination_height > job->display_info.height ||
      !rect_is_bounded(job, x, y, destination_width, destination_height)) {
    return luaL_error(state, "invalid draw_asset destination");
  }

  mark_dirty_rect(job, x, y, destination_width, destination_height);
  for (destination_y = 0; destination_y < destination_height; ++destination_y) {
    int source_y =
        (int)((int64_t)destination_y * source_height / destination_height);
    int destination_x;
    for (destination_x = 0; destination_x < destination_width;
         ++destination_x) {
      int source_x =
          (int)((int64_t)destination_x * source_width / destination_width);
      size_t offset =
          8u +
          ((size_t)source_y * (size_t)source_width + (size_t)source_x) * 2u;
      uint16_t argb4444 = (uint16_t)bytes[offset] |
                          (uint16_t)((uint16_t)bytes[offset + 1u] << 8u);
      unsigned alpha = ((argb4444 >> 12u) & 0x0fu) * 17u;
      unsigned red = (argb4444 >> 8u) & 0x0fu;
      unsigned green = (argb4444 >> 4u) & 0x0fu;
      unsigned blue = argb4444 & 0x0fu;
      uint16_t rgb565;
      alpha = (alpha * opacity + 127u) / 255u;
      if (alpha == 0u)
        continue;
      rgb565 = (uint16_t)(((red << 1u) | (red >> 3u)) << 11u |
                          ((green << 2u) | (green >> 2u)) << 5u |
                          ((blue << 1u) | (blue >> 3u)));
      blend_pixel(job, x + destination_x, y + destination_y, rgb565, alpha);
    }
  }
  return 0;
}

static int display_begin_frame(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  if (!job->display_open || job->frame_open) {
    return luaL_error(state, "invalid begin_frame");
  }
  job->frame_open = 1;
  if (!lua_isnoneornil(state, 1)) {
    int clear;
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "clear");
    clear = lua_toboolean(state, -1);
    lua_pop(state, 1);
    if (clear) {
      uint16_t color = 0u;
      lua_getfield(state, 1, "color");
      if (!lua_isnil(state, -1))
        color = check_color(state, -1);
      lua_pop(state, 1);
      display_clear_pixels(job, color);
    }
  }
  return 0;
}

/* Exact RGB565 comparison against the last successful display update. Opt-in:
 * display.present({retained=true}); subsequent present calls retain the mode.
 * Dirty tile runs preserve all pixels, including erasure of previous geometry. */
static int display_tile_changed(h2_lua_job_t *job, int x, int y, int w, int h) {
  for (int row = 0; row < h; ++row) {
    size_t offset = (size_t)(y + row) * (size_t)job->display_info.width + (size_t)x;
    if (memcmp(job->framebuffer + offset, job->presented_framebuffer + offset,
               (size_t)w * sizeof(uint16_t)) != 0)
      return 1;
  }
  return 0;
}
static int display_present(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_pal_result_t result = H2_PAL_OK;
  size_t pixels = 0, rectangles = 0;
  int merge_gap=0,direct_bounds=0;
  if (!job->display_open)
    return luaL_error(state, "display is not open");
  if (lua_istable(state, 1)) {
    lua_getfield(state,1,"bounds");direct_bounds=lua_toboolean(state,-1);lua_pop(state,1);
    lua_getfield(state,1,"merge_gap");
    lua_Integer gap=luaL_optinteger(state,-1,0);lua_pop(state,1);
    if(gap<0 || gap>8)return luaL_error(state,"invalid retained merge gap");
    merge_gap=(int)gap;
    lua_getfield(state, 1, "retained");
    int retained = lua_toboolean(state, -1);
    lua_pop(state, 1);
    if (retained && !job->presented_framebuffer) {
      size_t bytes = (size_t)job->display_info.width *
                     (size_t)job->display_info.height * sizeof(uint16_t);
      size_t tiles = ((size_t)job->display_info.width + 15u) / 16u *
                     (((size_t)job->display_info.height + 15u) / 16u);
      job->presented_framebuffer = h2_pal_mem_alloc(job->host->config.runtime->mem, bytes + tiles*2);
      if (!job->presented_framebuffer)
        return luaL_error(state, "retained display allocation failed");
      job->dirty_tiles=(uint8_t *)job->presented_framebuffer+bytes+tiles;
      memset(job->dirty_tiles,1,tiles);
      job->presented_valid = 0;
      job->dirty_valid = 1;
    }
  }
  if (job->dirty_valid) {
    int width = job->display_info.width, height = job->display_info.height;
    if (job->presented_framebuffer && job->presented_valid && !direct_bounds) {
      const int columns=(width+15)/16,rows=(height+15)/16;
      uint8_t *changed=(uint8_t *)(job->presented_framebuffer+(size_t)width*height);
      /* Compare the whole immutable frame before starting panel writes. Mixing
       * tile comparison with synchronous transfers stretches the visible tear
       * window. Join touching runs vertically as well as horizontally. */
      for(int ty=0;ty<rows;++ty) for(int tx=0;tx<columns;++tx) {
        int x=tx*16,y=ty*16,w=width-x<16?width-x:16,h=height-y<16?height-y:16;
        changed[ty*columns+tx]=(uint8_t)((!job->active_background || job->dirty_tiles[ty*columns+tx]) && x<=job->dirty_max_x && x+w>job->dirty_min_x &&
            y<=job->dirty_max_y && y+h>job->dirty_min_y && display_tile_changed(job,x,y,w,h));
      }
      for(int ty=0;ty<rows && result==H2_PAL_OK;++ty) for(int tx=0;tx<columns;) {
        if(!changed[ty*columns+tx]) {++tx;continue;}
        int end=tx+1;
        while(end<columns) {
          int next=end;
          while(next<columns && !changed[ty*columns+next] && next-end<merge_gap)++next;
          if(next>=columns || !changed[ty*columns+next])break;
          end=next+1;
        }
        int bottom=ty+1;
        while(bottom<rows) {
          int clean=0,dirty=0;
          for(int k=tx;k<end;++k) {if(changed[bottom*columns+k])++dirty;else ++clean;}
          if(!dirty || clean>merge_gap)break;
          ++bottom;
        }
        int x=tx*16,y=ty*16,right=end*16<width?end*16:width,low=bottom*16<height?bottom*16:height;
        h2_display_rect_t rect={x,y,right-x,low-y};
        size_t offset=(size_t)y*width+x;
        result=(h2_pal_result_t)h2_pal_display_draw_bitmap(job->host->config.runtime->display,
            &rect,job->framebuffer+offset,(size_t)width*sizeof(uint16_t),H2_DISPLAY_PIXEL_RGB565);
        if(result!=H2_PAL_OK)break;
        pixels+=(size_t)rect.width*rect.height;++rectangles;
        for(int row=0;row<rect.height;++row) {
          size_t at=offset+(size_t)row*width;
          memcpy(job->presented_framebuffer+at,job->framebuffer+at,(size_t)rect.width*sizeof(uint16_t));
        }
        for(int yy=ty;yy<bottom;++yy)for(int xx=tx;xx<end;++xx)changed[yy*columns+xx]=0;
        tx=end;
      }
    } else {
      h2_display_rect_t rect = job->presented_framebuffer && !job->presented_valid
          ? (h2_display_rect_t){0, 0, width, height}
          : (h2_display_rect_t){job->dirty_min_x, job->dirty_min_y,
              job->dirty_max_x - job->dirty_min_x + 1,
              job->dirty_max_y - job->dirty_min_y + 1};
      result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
          job->host->config.runtime->display, &rect,
          job->framebuffer + (size_t)rect.y * (size_t)width + (size_t)rect.x,
          (size_t)width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
      pixels = (size_t)rect.width * (size_t)rect.height;
      rectangles = 1;
      if (result == H2_PAL_OK && job->presented_framebuffer)
        for(int row=rect.y;row<rect.y+rect.height;++row) {
          size_t at=(size_t)row*width+rect.x;
          memcpy(job->presented_framebuffer+at,job->framebuffer+at,(size_t)rect.width*sizeof(uint16_t));
        }
    }
  }
  if (result == H2_PAL_OK)
    result = (h2_pal_result_t)h2_pal_display_present(job->host->config.runtime->display);
  if (result != H2_PAL_OK) {
    job->presented_valid = 0;
    return luaL_error(state, "display present failed: %d", result);
  }
  if (job->presented_framebuffer) job->presented_valid = 1;
  if(job->dirty_tiles)memset(job->dirty_tiles,0,(size_t)((job->display_info.width+15)/16)*((job->display_info.height+15)/16));
  job->dirty_valid = 0;
  lua_pushinteger(state, (lua_Integer)pixels);
  lua_pushinteger(state, (lua_Integer)rectangles);
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

static int display_close(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_lua_canvas_reset(state);
  if (job->display_open) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    h2_pal_mem_free(job->host->config.runtime->mem, job->framebuffer);
    job->framebuffer = NULL;
    h2_pal_mem_free(job->host->config.runtime->mem, job->presented_framebuffer);
    job->presented_framebuffer = NULL;
    job->dirty_tiles=NULL;job->background_dirty_tiles=NULL;
    if(job->active_background)luaL_unref(state,LUA_REGISTRYINDEX,job->background_ref);
    job->active_background=NULL;
    job->presented_valid = 0;
    job->display_open = 0;
    job->frame_open = 0;
    job->dirty_valid = 0;
  }
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
  lua_createtable(state, 0, 26);
  set_function(state, "clear", display_clear, job);
  set_function(state, "fill_rect", display_fill_rect, job);
  set_function(state, "fill_polygon", display_fill_polygon, job);
  set_function(state, "compile_mesh", display_compile_mesh, job);
  set_function(state, "compile_commands", display_compile_commands, job);
  set_function(state, "draw_commands", display_draw_commands, job);
  set_function(state, "capture_region", display_capture_region, job);
  set_function(state, "draw_region", display_draw_region, job);
  set_function(state, "stroke_path", display_stroke_path, job);
  set_function(state, "projected_line", display_projected_line, job);
  set_function(state, "restore_background", display_restore_background, job);
  set_function(state, "depth_path", display_depth_path, job);
  set_function(state, "draw_mesh", display_draw_mesh, job);
  set_function(state, "fill_ellipse", display_fill_ellipse, job);
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
  set_function(state, "draw_asset", display_draw_asset, job);
  set_function(state, "draw_light_atlas", display_draw_light_atlas, job);
  h2_lua_canvas_register(state, job);
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
