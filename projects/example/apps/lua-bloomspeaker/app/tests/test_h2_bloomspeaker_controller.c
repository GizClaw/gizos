#include "h2_bloomspeaker_controller.h"

#include <assert.h>
#include <string.h>

int main(void) {
  h2_bloomspeaker_controller_t controller;
  h2_bloomspeaker_controller_init(&controller, 10u);
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_IDLE);
  assert(snapshot.state_entered_ms == 10u);
  assert(!snapshot.native_audio);

  h2_bloomspeaker_controller_long_press(&controller, 20u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_PAIRING);
  assert(strcmp(h2_bloomspeaker_state_name(snapshot.state), "pairing") == 0);

  h2_bloomspeaker_controller_long_press(&controller, 30u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_PAIRING);

  h2_bloomspeaker_controller_hold_release(&controller, 31u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_IDLE);

  assert(!h2_bloomspeaker_controller_transition(
      &controller, H2_BLOOMSPEAKER_STATE_PAIRING,
      H2_BLOOMSPEAKER_STATE_CONNECTING, 32u, 999u, 7));
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_IDLE);
  assert(snapshot.state_entered_ms == 31u);
  assert(snapshot.peer_tag == 0u);
  assert(snapshot.last_error == 0);
  assert(h2_bloomspeaker_controller_transition(
      &controller, H2_BLOOMSPEAKER_STATE_IDLE,
      H2_BLOOMSPEAKER_STATE_CONNECTING, 33u, 321u, 0));
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_CONNECTING);
  assert(snapshot.state_entered_ms == 33u);
  assert(snapshot.peer_tag == 321u);

  h2_bloomspeaker_controller_set_state(
      &controller, H2_BLOOMSPEAKER_STATE_TALKING, 40u, 123u, 0);
  h2_bloomspeaker_controller_set_levels(&controller, -1.0f, 0.5f, 0.25f,
                                        2.0f);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.peer_tag == 123u);
  assert(snapshot.local_level == 0.0f);
  assert(snapshot.local_peak > 0.49f && snapshot.local_peak < 0.51f);
  assert(snapshot.remote_level > 0.24f && snapshot.remote_level < 0.26f);
  assert(snapshot.remote_peak == 1.0f);
  h2_bloomspeaker_controller_set_native_audio(&controller, true);
  h2_bloomspeaker_controller_set_local_levels(&controller, 0.75f, 0.8f);
  h2_bloomspeaker_controller_set_remote_levels(&controller, 0.3f, 0.4f);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.native_audio);
  assert(snapshot.local_level > 0.74f && snapshot.local_level < 0.76f);
  assert(snapshot.remote_peak > 0.39f && snapshot.remote_peak < 0.41f);

  h2_bloomspeaker_controller_long_press(&controller, 50u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_DISCONNECTING);
  h2_bloomspeaker_controller_hold_release(&controller, 55u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_DISCONNECTING);
  h2_bloomspeaker_controller_long_press(&controller, 60u);
  h2_bloomspeaker_controller_snapshot(&controller, &snapshot);
  assert(snapshot.state == H2_BLOOMSPEAKER_STATE_DISCONNECTING);
  return 0;
}
