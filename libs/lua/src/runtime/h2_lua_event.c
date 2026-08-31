#include "h2_lua_internal.h"

#include <string.h>

/* Keep the Lua-facing Button gesture contract stable without making gesture
 * policy part of the shared Runtime payload.  These values match the existing
 * Lua API: press=1, short_press=2, long_press=3. */
#define H2_LUA_BUTTON_LONG_PRESS_MS 500u

enum h2_lua_button_gesture_kind {
  H2_LUA_BUTTON_GESTURE_PRESS = 1,
  H2_LUA_BUTTON_GESTURE_SHORT_PRESS,
  H2_LUA_BUTTON_GESTURE_LONG_PRESS,
};

_Static_assert(sizeof(h2_runtime_button_down_event_t) <=
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "button down event exceeds Lua delivery storage");
_Static_assert(sizeof(h2_runtime_button_up_event_t) <=
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "button up event exceeds Lua delivery storage");
_Static_assert(sizeof(h2_runtime_button_action_event_t) <=
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "button click event exceeds Lua delivery storage");
_Static_assert(sizeof(h2_runtime_nfc_state_t) <= H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "NFC event exceeds Lua delivery storage");
_Static_assert(sizeof(h2_runtime_imu_gesture_event_t) <=
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "IMU event exceeds Lua delivery storage");
_Static_assert(sizeof(h2_pal_result_t) <= H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "error event exceeds Lua delivery storage");

static int button_action_is_valid(const h2_runtime_event_t *event) {
  const h2_runtime_button_action_event_t *value = event->payload;
  uint64_t elapsed_ms;
  uint32_t expected_duration_ms;
  if (value->click_count == 0u ||
      value->phase < H2_RUNTIME_BUTTON_ACTION_PHASE_PRESSED ||
      value->phase > H2_RUNTIME_BUTTON_ACTION_PHASE_RELEASED) {
    return 0;
  }
  if (value->phase == H2_RUNTIME_BUTTON_ACTION_PHASE_RELEASED) {
    if (value->released_at_ms < value->pressed_at_ms ||
        event->timestamp_ms != value->released_at_ms) {
      return 0;
    }
    elapsed_ms = value->released_at_ms - value->pressed_at_ms;
  } else {
    if (value->released_at_ms != 0u ||
        event->timestamp_ms < value->pressed_at_ms) {
      return 0;
    }
    elapsed_ms = event->timestamp_ms - value->pressed_at_ms;
  }
  expected_duration_ms =
      elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
  return value->duration_ms == expected_duration_ms;
}

static int payload_size_is_valid(const h2_runtime_event_t *event) {
  size_t expected = 0u;
  h2_runtime_component_t expected_component = H2_RUNTIME_COMPONENT_NONE;
  switch (event->kind) {
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN:
    expected = sizeof(h2_runtime_button_down_event_t);
    expected_component = H2_RUNTIME_COMPONENT_BUTTON;
    break;
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP:
    expected = sizeof(h2_runtime_button_up_event_t);
    expected_component = H2_RUNTIME_COMPONENT_BUTTON;
    break;
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION:
    expected = sizeof(h2_runtime_button_action_event_t);
    expected_component = H2_RUNTIME_COMPONENT_BUTTON;
    break;
  case H2_RUNTIME_COMPONENT_EVENT_NFC_STATE:
    expected = sizeof(h2_runtime_nfc_state_t);
    expected_component = H2_RUNTIME_COMPONENT_NFC_READER;
    break;
  case H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE:
    expected = sizeof(h2_runtime_imu_gesture_event_t);
    expected_component = H2_RUNTIME_COMPONENT_IMU;
    break;
  case H2_RUNTIME_COMPONENT_EVENT_ERROR:
    expected = sizeof(h2_pal_result_t);
    break;
  default:
    return 0;
  }
  if (event->payload == NULL || event->payload_size != expected ||
      (expected_component != H2_RUNTIME_COMPONENT_NONE &&
       event->component != expected_component) ||
      (expected_component == H2_RUNTIME_COMPONENT_NONE &&
       event->component == H2_RUNTIME_COMPONENT_NONE)) {
    return 0;
  }
  if (event->kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE &&
      ((const h2_runtime_nfc_state_t *)event->payload)->uid_len >
          H2_PAL_NFC_UID_MAX_LEN) {
    return 0;
  }
  if (event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
      !button_action_is_valid(event)) {
    return 0;
  }
  return 1;
}

h2_pal_result_t h2_lua_dispatch_runtime_event(h2_lua_host_t *host,
                                              h2_lua_job_id_t job_id,
                                              const h2_runtime_event_t *event) {
  h2_lua_job_t *job;
  h2_runtime_component_info_t component_info;
  h2_pal_result_t result;
  size_t tail;
  h2_lua_event_record_t *record;
  if (host == NULL || event == NULL ||
      event->component_id == H2_RUNTIME_COMPONENT_ID_NONE ||
      event->kind == H2_RUNTIME_EVENT_NONE ||
      event->payload_size > event->payload_capacity ||
      (event->payload_size != 0u && event->payload == NULL) ||
      !payload_size_is_valid(event)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result = h2_runtime_component_get(host->config.runtime, event->component_id,
                                    &component_info);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (component_info.kind != event->component) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result = h2_lua_lock_job(host, job_id, &job);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (job->state == H2_LUA_JOB_SUCCEEDED || job->state == H2_LUA_JOB_FAILED ||
      job->state == H2_LUA_JOB_CANCELLED ||
      job->state == H2_LUA_JOB_TIMED_OUT || job->state == H2_LUA_JOB_STOPPED) {
    h2_lua_unlock_job(job);
    return H2_PAL_ERR_CLOSED;
  }
  if (job->event_count == host->config.event_delivery_capacity) {
    h2_lua_unlock_job(job);
    return H2_PAL_ERR_FULL;
  }
  tail = (job->event_head + job->event_count) %
         host->config.event_delivery_capacity;
  record = &job->events[tail];
  memset(record, 0, sizeof(*record));
  record->event = *event;
  record->event.payload = record->payload;
  record->event.payload_capacity = sizeof(record->payload);
  if (event->payload_size != 0u) {
    memcpy(record->payload, event->payload, event->payload_size);
  }
  job->event_count++;
  h2_lua_host_wake_job(job);
  h2_lua_unlock_job(job);
  return H2_PAL_OK;
}

static void push_u64(lua_State *state, const char *name, uint64_t value) {
  lua_pushinteger(state, (lua_Integer)value);
  lua_setfield(state, -2, name);
}

static void push_i32(lua_State *state, const char *name, int32_t value) {
  lua_pushinteger(state, (lua_Integer)value);
  lua_setfield(state, -2, name);
}

static uint64_t button_gesture_kind(
    const h2_runtime_button_action_event_t *value) {
  if (value->duration_ms >= H2_LUA_BUTTON_LONG_PRESS_MS) {
    return H2_LUA_BUTTON_GESTURE_LONG_PRESS;
  }
  if (value->phase == H2_RUNTIME_BUTTON_ACTION_PHASE_RELEASED) {
    return H2_LUA_BUTTON_GESTURE_SHORT_PRESS;
  }
  return H2_LUA_BUTTON_GESTURE_PRESS;
}

static void push_event(lua_State *state, const h2_runtime_event_t *event) {
  lua_createtable(state, 0, 11);
  push_u64(state, "component_id", event->component_id);
  push_u64(state, "component_kind", event->component);
  push_u64(state, "event_type", event->kind);
  push_u64(state, "sequence", event->sequence);
  push_u64(state, "timestamp_ms", event->timestamp_ms);
  switch (event->kind) {
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN: {
    const h2_runtime_button_down_event_t *value = event->payload;
    push_u64(state, "pressed_at_ms", value->pressed_at_ms);
    break;
  }
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP: {
    const h2_runtime_button_up_event_t *value = event->payload;
    push_u64(state, "pressed_at_ms", value->pressed_at_ms);
    push_u64(state, "released_at_ms", value->released_at_ms);
    break;
  }
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION: {
    const h2_runtime_button_action_event_t *value = event->payload;
    push_u64(state, "pressed_at_ms", value->pressed_at_ms);
    push_u64(state, "released_at_ms", value->released_at_ms);
    push_u64(state, "click_count", value->click_count);
    push_u64(state, "action_phase", value->phase);
    push_u64(state, "gesture_kind", button_gesture_kind(value));
    push_u64(state, "duration_ms", value->duration_ms);
    break;
  }
  case H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE: {
    const h2_runtime_imu_gesture_event_t *value = event->payload;
    push_u64(state, "gesture_kind", value->kind);
    if (value->kind == H2_RUNTIME_IMU_GESTURE_SHAKE) {
      push_i32(state, "magnitude_mg", value->gesture.shake.magnitude_mg);
      push_u64(state, "duration_ms", value->gesture.shake.duration_ms);
    } else if (value->kind == H2_RUNTIME_IMU_GESTURE_TILT) {
      push_i32(state, "x_mg", value->gesture.tilt.x_mg);
      push_i32(state, "y_mg", value->gesture.tilt.y_mg);
      push_i32(state, "z_mg", value->gesture.tilt.z_mg);
    } else if (value->kind == H2_RUNTIME_IMU_GESTURE_FLIP) {
      push_i32(state, "gyro_z_mdps", value->gesture.flip.gyro_z_mdps);
    } else if (value->kind == H2_RUNTIME_IMU_GESTURE_FREE_FALL) {
      push_u64(state, "duration_ms", value->gesture.free_fall.duration_ms);
      push_i32(state, "magnitude_mg", value->gesture.free_fall.magnitude_mg);
    }
    break;
  }
  case H2_RUNTIME_COMPONENT_EVENT_NFC_STATE: {
    const h2_runtime_nfc_state_t *value = event->payload;
    push_u64(state, "status", value->status);
    push_u64(state, "stage", value->stage);
    push_u64(state, "tag_type", value->tag_type);
    lua_pushlstring(state, (const char *)value->uid, value->uid_len);
    lua_setfield(state, -2, "uid");
    push_i32(state, "result", value->result);
    push_u64(state, "updated_at_ms", value->updated_at_ms);
    break;
  }
  case H2_RUNTIME_COMPONENT_EVENT_ERROR: {
    const h2_pal_result_t *value = event->payload;
    push_i32(state, "result", *value);
    break;
  }
  default:
    break;
  }
}

void h2_lua_deliver_events(h2_lua_job_t *job) {
  while (job->event_count != 0u) {
    h2_lua_event_record_t *record = &job->events[job->event_head];
    size_t i;
    for (i = record->next_callback_index; i < job->callback_count; ++i) {
      h2_lua_callback_t *callback = &job->callbacks[i];
      uint32_t task_id = 0u;
      int function_index;
      if (!callback->active ||
          callback->component_id != record->event.component_id ||
          callback->kind != record->event.kind) {
        continue;
      }
      lua_rawgeti(job->vm->state, LUA_REGISTRYINDEX, callback->lua_ref);
      push_event(job->vm->state, &record->event);
      function_index = lua_absindex(job->vm->state, -2);
      if (h2_lua_spawn_task(job, job->vm->state, function_index, 1, &task_id) !=
          H2_PAL_OK) {
        lua_pop(job->vm->state, 2);
        return;
      }
      h2_lua_find_task(job, task_id)->release_when_done = 1;
      lua_pop(job->vm->state, 2);
      record->next_callback_index = i + 1u;
    }
    job->event_head =
        (job->event_head + 1u) % job->host->config.event_delivery_capacity;
    job->event_count--;
  }
}
