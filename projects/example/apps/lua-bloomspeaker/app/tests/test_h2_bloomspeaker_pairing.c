#include "h2_bloomspeaker_pairing.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <string.h>

static h2_bloomspeaker_pairing_beacon_t beacon(uint64_t device,
                                                uint64_t ticket,
                                                uint32_t epoch) {
  return (h2_bloomspeaker_pairing_beacon_t){
      .device_tag = device,
      .ticket = ticket,
      .epoch = epoch,
      .state = H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE,
  };
}

static void observe_twice(h2_bloomspeaker_pairing_t *pairing,
                          const h2_bloomspeaker_pairing_beacon_t *remote) {
  assert(h2_bloomspeaker_pairing_observe(pairing, remote, -50, 100u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_observe(pairing, remote, -49, 800u) ==
         H2_PAL_OK);
}

static void test_beacon_round_trip(void) {
  h2_bloomspeaker_pairing_beacon_t input = beacon(0x0102030405u,
                                                   UINT64_C(0x8877665544332211),
                                                   0xaabbccddu);
  input.state = H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED;
  input.claim_target = 0x0a0b0c0d0eu;
  uint8_t data[H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE];
  assert(h2_bloomspeaker_pairing_encode(&input, data) == H2_PAL_OK);
  h2_bloomspeaker_pairing_beacon_t output;
  assert(h2_bloomspeaker_pairing_decode(data, sizeof(data), &output) ==
         H2_PAL_OK);
  assert(input.device_tag == output.device_tag);
  assert(input.ticket == output.ticket);
  assert(input.claim_target == output.claim_target);
  assert(input.epoch == output.epoch);
  assert(input.state == output.state);
  data[0] ^= 1u;
  assert(h2_bloomspeaker_pairing_decode(data, sizeof(data), &output) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_bloomspeaker_pairing_decode(data, sizeof(data) - 1u, &output) ==
         H2_PAL_ERR_FORMAT);
}

static void test_two_devices_mutually_select_and_lock(void) {
  h2_bloomspeaker_pairing_t left;
  h2_bloomspeaker_pairing_t right;
  h2_bloomspeaker_pairing_init(&left, 1u, 10u, 101u);
  h2_bloomspeaker_pairing_init(&right, 2u, 20u, 202u);
  observe_twice(&left, &right.local);
  observe_twice(&right, &left.local);
  assert(h2_bloomspeaker_pairing_select(&left, 800u) == 2u);
  assert(h2_bloomspeaker_pairing_select(&right, 800u) == 1u);
  h2_bloomspeaker_pairing_set_claim(&left, 2u);
  h2_bloomspeaker_pairing_set_claim(&right, 1u);
  assert(h2_bloomspeaker_pairing_observe(&left, &right.local, -48, 900u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_observe(&right, &left.local, -48, 900u) ==
         H2_PAL_OK);
  uint64_t peer = 0u;
  assert(h2_bloomspeaker_pairing_mutual_claim(&left, 900u, &peer));
  assert(peer == 2u);
  assert(h2_bloomspeaker_pairing_mutual_claim(&right, 900u, &peer));
  assert(peer == 1u);
  assert(!h2_bloomspeaker_pairing_lock(&left, 1u));
  assert(h2_bloomspeaker_pairing_lock(&left, 2u));
  assert(left.local.state == H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED);
  assert(h2_bloomspeaker_pairing_observe(&right, &left.local, -47, 950u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_mutual_claim(&right, 950u, &peer));
  assert(peer == 1u);
  assert(h2_bloomspeaker_pairing_role(&left, 2u) ==
         H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL);
  assert(h2_bloomspeaker_pairing_role(&right, 1u) ==
         H2_BLOOMSPEAKER_PAIRING_ROLE_CENTRAL);
}

static void test_three_devices_create_one_pair(void) {
  h2_bloomspeaker_pairing_t nodes[3];
  h2_bloomspeaker_pairing_init(&nodes[0], 1u, 10u, 1u);
  h2_bloomspeaker_pairing_init(&nodes[1], 2u, 30u, 2u);
  h2_bloomspeaker_pairing_init(&nodes[2], 3u, 20u, 3u);
  for (size_t local = 0u; local < 3u; ++local) {
    for (size_t remote = 0u; remote < 3u; ++remote) {
      if (local != remote) {
        observe_twice(&nodes[local], &nodes[remote].local);
      }
    }
  }
  assert(h2_bloomspeaker_pairing_select(&nodes[0], 800u) == 3u);
  assert(h2_bloomspeaker_pairing_select(&nodes[2], 800u) == 1u);
  assert(h2_bloomspeaker_pairing_select(&nodes[1], 800u) == 0u);
  h2_bloomspeaker_pairing_set_claim(&nodes[0], 3u);
  h2_bloomspeaker_pairing_set_claim(&nodes[2], 1u);
  for (size_t local = 0u; local < 3u; ++local) {
    for (size_t remote = 0u; remote < 3u; ++remote) {
      if (local != remote) {
        assert(h2_bloomspeaker_pairing_observe(
                   &nodes[local], &nodes[remote].local, -45, 900u) ==
               H2_PAL_OK);
      }
    }
  }
  uint64_t peer;
  assert(h2_bloomspeaker_pairing_mutual_claim(&nodes[0], 900u, &peer));
  assert(peer == 3u);
  assert(h2_bloomspeaker_pairing_mutual_claim(&nodes[2], 900u, &peer));
  assert(peer == 1u);
  assert(!h2_bloomspeaker_pairing_mutual_claim(&nodes[1], 900u, &peer));
}

static void test_visibility_and_claim_guards(void) {
  h2_bloomspeaker_pairing_t local;
  h2_bloomspeaker_pairing_init(&local, 9u, 9u, 1u);
  h2_bloomspeaker_pairing_beacon_t remote = beacon(8u, 8u, 2u);
  assert(h2_bloomspeaker_pairing_observe(&local, &remote, -90, 0u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_observe(&local, &remote, -90, 700u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_select(&local, 700u) == 0u);
  assert(h2_bloomspeaker_pairing_observe(&local, &remote, -50, 800u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_select(&local, 800u) == 8u);
  h2_bloomspeaker_pairing_set_claim(&local, 7u);
  assert(!h2_bloomspeaker_pairing_mutual_claim(&local, 800u, NULL));
  h2_bloomspeaker_pairing_set_claim(&local, 8u);
  remote.state = H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED;
  remote.claim_target = 7u;
  assert(h2_bloomspeaker_pairing_observe(&local, &remote, -50, 900u) ==
         H2_PAL_OK);
  assert(!h2_bloomspeaker_pairing_mutual_claim(&local, 900u, NULL));
  remote.claim_target = 9u;
  assert(h2_bloomspeaker_pairing_observe(&local, &remote, -50, 1000u) ==
         H2_PAL_OK);
  assert(h2_bloomspeaker_pairing_mutual_claim(&local, 1000u, NULL));
  assert(h2_bloomspeaker_pairing_select(&local, 2601u) == 0u);

  h2_bloomspeaker_pairing_beacon_t invalid = beacon(7u, 7u, 3u);
  invalid.claim_target = 9u;
  assert(h2_bloomspeaker_pairing_observe(&local, &invalid, -50, 2700u) ==
         H2_PAL_ERR_INVALID_ARG);
  const uint64_t previous_claim = local.local.claim_target;
  h2_bloomspeaker_pairing_set_claim(&local, local.local.device_tag);
  assert(local.local.claim_target == previous_claim);
}

int main(void) {
  test_beacon_round_trip();
  test_two_devices_mutually_select_and_lock();
  test_three_devices_create_one_pair();
  test_visibility_and_claim_guards();
  return 0;
}
