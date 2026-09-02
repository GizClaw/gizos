#ifndef H2_BLOOMSPEAKER_CONTROLLER_H
#define H2_BLOOMSPEAKER_CONTROLLER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bloomspeaker_state {
  H2_BLOOMSPEAKER_STATE_IDLE = 0,
  H2_BLOOMSPEAKER_STATE_PAIRING = 1,
  H2_BLOOMSPEAKER_STATE_CLAIMING = 2,
  H2_BLOOMSPEAKER_STATE_CONNECTING = 3,
  H2_BLOOMSPEAKER_STATE_SECURING = 4,
  H2_BLOOMSPEAKER_STATE_TALKING = 5,
  H2_BLOOMSPEAKER_STATE_DISCONNECTING = 6,
  H2_BLOOMSPEAKER_STATE_ERROR = 7,
} h2_bloomspeaker_state_t;

typedef struct h2_bloomspeaker_snapshot {
  h2_bloomspeaker_state_t state;
  uint64_t state_entered_ms;
  uint64_t peer_tag;
  float local_level;
  float local_peak;
  float remote_level;
  float remote_peak;
  bool native_audio;
  int last_error;
} h2_bloomspeaker_snapshot_t;

typedef struct h2_bloomspeaker_controller {
  _Atomic uint64_t state_word;
  _Atomic uint64_t peer_tag;
  _Atomic uint32_t local_level;
  _Atomic uint32_t local_peak;
  _Atomic uint32_t remote_level;
  _Atomic uint32_t remote_peak;
  _Atomic bool native_audio;
  _Atomic int last_error;
} h2_bloomspeaker_controller_t;

void h2_bloomspeaker_controller_init(h2_bloomspeaker_controller_t *controller,
                                     uint64_t now_ms);

void h2_bloomspeaker_controller_long_press(
    h2_bloomspeaker_controller_t *controller, uint64_t now_ms);

void h2_bloomspeaker_controller_hold_release(
    h2_bloomspeaker_controller_t *controller, uint64_t now_ms);

void h2_bloomspeaker_controller_set_state(
    h2_bloomspeaker_controller_t *controller, h2_bloomspeaker_state_t state,
    uint64_t now_ms, uint64_t peer_tag, int error);

bool h2_bloomspeaker_controller_transition(
    h2_bloomspeaker_controller_t *controller,
    h2_bloomspeaker_state_t expected, h2_bloomspeaker_state_t next,
    uint64_t now_ms, uint64_t peer_tag, int error);

void h2_bloomspeaker_controller_set_levels(
    h2_bloomspeaker_controller_t *controller, float local_level,
    float local_peak, float remote_level, float remote_peak);

void h2_bloomspeaker_controller_set_local_levels(
    h2_bloomspeaker_controller_t *controller, float level, float peak);

void h2_bloomspeaker_controller_set_remote_levels(
    h2_bloomspeaker_controller_t *controller, float level, float peak);

void h2_bloomspeaker_controller_set_native_audio(
    h2_bloomspeaker_controller_t *controller, bool enabled);

void h2_bloomspeaker_controller_snapshot(
    h2_bloomspeaker_controller_t *controller,
    h2_bloomspeaker_snapshot_t *out_snapshot);

const char *h2_bloomspeaker_state_name(h2_bloomspeaker_state_t state);

#ifdef __cplusplus
}
#endif

#endif
