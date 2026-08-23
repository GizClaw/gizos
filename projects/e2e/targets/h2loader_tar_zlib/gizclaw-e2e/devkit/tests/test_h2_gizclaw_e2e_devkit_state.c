#include "h2_gizclaw_e2e_devkit_state.h"

#include <assert.h>
#include <stdint.h>

static void test_runner_starts_only_after_network_and_clock(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);

  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, false,
                                                         false));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, false));
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, false, true));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, false));
  assert(state.runner_started);
  assert(state.wifi_has_ip);
  assert(state.clock_ready);
}

static void test_summary_replay_is_not_early(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  h2_gizclaw_e2e_devkit_state_complete(&state, 100u, 10000u);

  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 10099u,
                                                          10000u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 10100u,
                                                         10000u));
  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 20099u,
                                                          10000u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 20100u,
                                                         10000u));
}

static void test_summary_replay_handles_monotonic_wrap(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  h2_gizclaw_e2e_devkit_state_complete(&state, UINT64_MAX - 5u, 10u);

  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 3u, 10u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 4u, 10u));
}

int main(void) {
  test_runner_starts_only_after_network_and_clock();
  test_summary_replay_is_not_early();
  test_summary_replay_handles_monotonic_wrap();
  return 0;
}
