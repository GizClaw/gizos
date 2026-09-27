#include "h2_bloomspeaker_controller.h"

#include <stddef.h>

#define H2_BLOOMSPEAKER_LEVEL_SCALE 65535u
#define H2_BLOOMSPEAKER_STATE_BITS 4u
#define H2_BLOOMSPEAKER_STATE_MASK UINT64_C(0x0f)

static uint32_t level_to_fixed(float level) {
  if (level <= 0.0f) {
    return 0u;
  }
  if (level >= 1.0f) {
    return H2_BLOOMSPEAKER_LEVEL_SCALE;
  }
  return (uint32_t)(level * (float)H2_BLOOMSPEAKER_LEVEL_SCALE + 0.5f);
}

static float fixed_to_level(uint32_t level) {
  return (float)level / (float)H2_BLOOMSPEAKER_LEVEL_SCALE;
}

static uint64_t make_state_word(h2_bloomspeaker_state_t state,
                                uint64_t now_ms) {
  return (now_ms << H2_BLOOMSPEAKER_STATE_BITS) | (uint64_t)state;
}

static h2_bloomspeaker_state_t state_from_word(uint64_t word) {
  return (h2_bloomspeaker_state_t)(word & H2_BLOOMSPEAKER_STATE_MASK);
}

static bool state_is_valid(h2_bloomspeaker_state_t state) {
  return (unsigned int)state <= (unsigned int)H2_BLOOMSPEAKER_STATE_ERROR;
}

static uint64_t entered_ms_from_word(uint64_t word) {
  return word >> H2_BLOOMSPEAKER_STATE_BITS;
}

static void controller_lock(h2_bloomspeaker_controller_t *controller) {
  while (h2_atomic_flag_test_and_set(&controller->lock, H2_ATOMIC_ACQUIRE)) {
  }
}

static void controller_unlock(h2_bloomspeaker_controller_t *controller) {
  h2_atomic_flag_clear(&controller->lock, H2_ATOMIC_RELEASE);
}

static void set_metadata(h2_bloomspeaker_controller_t *controller,
                         uint64_t peer_tag, int error) {
  controller->peer_tag = peer_tag;
  controller->last_error = error;
}

bool h2_bloomspeaker_hold_tracker_update(
    h2_bloomspeaker_hold_tracker_t *tracker, bool pressed, uint64_t now_ms,
    uint32_t hold_ms) {
  if (tracker == NULL) {
    return false;
  }
  if (!pressed) {
    *tracker = (h2_bloomspeaker_hold_tracker_t){0};
    return false;
  }
  if (!tracker->pressed || now_ms < tracker->pressed_since_ms) {
    tracker->pressed = true;
    tracker->pressed_since_ms = now_ms;
    tracker->triggered = false;
    return false;
  }
  if (tracker->triggered || now_ms - tracker->pressed_since_ms < hold_ms) {
    return false;
  }
  tracker->triggered = true;
  return true;
}

bool h2_bloomspeaker_controller_init(h2_bloomspeaker_controller_t *controller,
                                     uint64_t now_ms) {
  if (controller == NULL) {
    return false;
  }
  *controller = (h2_bloomspeaker_controller_t){0};
  if (h2_atomic_flag_init(&controller->lock) != H2_ATOMIC_OK) {
    return false;
  }
  controller->state_word = make_state_word(H2_BLOOMSPEAKER_STATE_IDLE, now_ms);
  return true;
}

void h2_bloomspeaker_controller_destroy(h2_bloomspeaker_controller_t *controller) {
  if (controller != NULL) {
    h2_atomic_flag_destroy(&controller->lock);
  }
}

void h2_bloomspeaker_controller_long_press(
    h2_bloomspeaker_controller_t *controller, uint64_t now_ms) {
  if (controller == NULL) {
    return;
  }
  controller_lock(controller);
  h2_bloomspeaker_state_t state = state_from_word(controller->state_word);
  h2_bloomspeaker_state_t next = state;
  switch (state) {
  case H2_BLOOMSPEAKER_STATE_IDLE:
  case H2_BLOOMSPEAKER_STATE_ERROR:
    next = H2_BLOOMSPEAKER_STATE_PAIRING;
    break;
  case H2_BLOOMSPEAKER_STATE_TALKING:
    next = H2_BLOOMSPEAKER_STATE_DISCONNECTING;
    break;
  default:
    controller_unlock(controller);
    return;
  }
  controller->state_word = make_state_word(next, now_ms);
  set_metadata(controller, 0u, 0);
  controller_unlock(controller);
}

void h2_bloomspeaker_controller_hold_release(
    h2_bloomspeaker_controller_t *controller, uint64_t now_ms) {
  if (controller == NULL) {
    return;
  }
  const h2_bloomspeaker_state_t pairing_states[] = {
      H2_BLOOMSPEAKER_STATE_PAIRING,
      H2_BLOOMSPEAKER_STATE_CLAIMING,
      H2_BLOOMSPEAKER_STATE_CONNECTING,
      H2_BLOOMSPEAKER_STATE_SECURING,
  };
  for (size_t index = 0u;
       index < sizeof(pairing_states) / sizeof(pairing_states[0]); ++index) {
    if (h2_bloomspeaker_controller_transition(
            controller, pairing_states[index], H2_BLOOMSPEAKER_STATE_IDLE,
            now_ms, 0u, 0)) {
      return;
    }
  }
}

void h2_bloomspeaker_controller_set_state(
    h2_bloomspeaker_controller_t *controller, h2_bloomspeaker_state_t state,
    uint64_t now_ms, uint64_t peer_tag, int error) {
  if (controller == NULL || !state_is_valid(state)) {
    return;
  }
  controller_lock(controller);
  set_metadata(controller, peer_tag, error);
  controller->state_word = make_state_word(state, now_ms);
  controller_unlock(controller);
}

bool h2_bloomspeaker_controller_transition(
    h2_bloomspeaker_controller_t *controller,
    h2_bloomspeaker_state_t expected, h2_bloomspeaker_state_t next,
    uint64_t now_ms, uint64_t peer_tag, int error) {
  if (controller == NULL || !state_is_valid(expected) ||
      !state_is_valid(next)) {
    return false;
  }
  controller_lock(controller);
  const bool matches = state_from_word(controller->state_word) == expected;
  if (matches) {
    controller->state_word = make_state_word(next, now_ms);
    set_metadata(controller, peer_tag, error);
  }
  controller_unlock(controller);
  return matches;
}

void h2_bloomspeaker_controller_set_levels(
    h2_bloomspeaker_controller_t *controller, float local_level,
    float local_peak, float remote_level, float remote_peak) {
  if (controller == NULL) {
    return;
  }
  h2_bloomspeaker_controller_set_local_levels(controller, local_level,
                                               local_peak);
  h2_bloomspeaker_controller_set_remote_levels(controller, remote_level,
                                                remote_peak);
}

void h2_bloomspeaker_controller_set_local_levels(
    h2_bloomspeaker_controller_t *controller, float level, float peak) {
  if (controller == NULL) {
    return;
  }
  controller_lock(controller);
  controller->local_level = level_to_fixed(level);
  controller->local_peak = level_to_fixed(peak);
  controller_unlock(controller);
}

void h2_bloomspeaker_controller_set_remote_levels(
    h2_bloomspeaker_controller_t *controller, float level, float peak) {
  if (controller == NULL) {
    return;
  }
  controller_lock(controller);
  controller->remote_level = level_to_fixed(level);
  controller->remote_peak = level_to_fixed(peak);
  controller_unlock(controller);
}

void h2_bloomspeaker_controller_set_native_audio(
    h2_bloomspeaker_controller_t *controller, bool enabled) {
  if (controller != NULL) {
    controller_lock(controller);
    controller->native_audio = enabled;
    controller_unlock(controller);
  }
}

void h2_bloomspeaker_controller_snapshot(
    h2_bloomspeaker_controller_t *controller,
    h2_bloomspeaker_snapshot_t *out_snapshot) {
  if (controller == NULL || out_snapshot == NULL) {
    return;
  }
  controller_lock(controller);
  const uint64_t state_word = controller->state_word;
  out_snapshot->state = state_from_word(state_word);
  out_snapshot->state_entered_ms = entered_ms_from_word(state_word);
  out_snapshot->peer_tag = controller->peer_tag;
  out_snapshot->last_error = controller->last_error;
  out_snapshot->local_level = fixed_to_level(controller->local_level);
  out_snapshot->local_peak = fixed_to_level(controller->local_peak);
  out_snapshot->remote_level = fixed_to_level(controller->remote_level);
  out_snapshot->remote_peak = fixed_to_level(controller->remote_peak);
  out_snapshot->native_audio = controller->native_audio;
  controller_unlock(controller);
}

const char *h2_bloomspeaker_state_name(h2_bloomspeaker_state_t state) {
  switch (state) {
  case H2_BLOOMSPEAKER_STATE_IDLE:
    return "idle";
  case H2_BLOOMSPEAKER_STATE_PAIRING:
    return "pairing";
  case H2_BLOOMSPEAKER_STATE_CLAIMING:
    return "claiming";
  case H2_BLOOMSPEAKER_STATE_CONNECTING:
    return "connecting";
  case H2_BLOOMSPEAKER_STATE_SECURING:
    return "securing";
  case H2_BLOOMSPEAKER_STATE_TALKING:
    return "talking";
  case H2_BLOOMSPEAKER_STATE_DISCONNECTING:
    return "disconnecting";
  case H2_BLOOMSPEAKER_STATE_ERROR:
    return "error";
  }
  return "error";
}
