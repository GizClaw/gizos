#ifndef H2_BLOOMSPEAKER_PAIRING_H
#define H2_BLOOMSPEAKER_PAIRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES 8u
#define H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE 25u
#define H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK UINT64_C(0xffffffffff)

typedef enum h2_bloomspeaker_pairing_beacon_state {
  H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE = 1,
  H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED = 2,
  H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED = 3,
} h2_bloomspeaker_pairing_beacon_state_t;

typedef enum h2_bloomspeaker_pairing_role {
  H2_BLOOMSPEAKER_PAIRING_ROLE_NONE = 0,
  H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL = 1,
  H2_BLOOMSPEAKER_PAIRING_ROLE_CENTRAL = 2,
} h2_bloomspeaker_pairing_role_t;

typedef struct h2_bloomspeaker_pairing_beacon {
  uint64_t device_tag;
  uint64_t ticket;
  uint64_t claim_target;
  uint32_t epoch;
  h2_bloomspeaker_pairing_beacon_state_t state;
} h2_bloomspeaker_pairing_beacon_t;

typedef struct h2_bloomspeaker_pairing_candidate {
  h2_bloomspeaker_pairing_beacon_t beacon;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  int rssi;
  uint8_t seen_count;
  bool occupied;
} h2_bloomspeaker_pairing_candidate_t;

typedef struct h2_bloomspeaker_pairing {
  h2_bloomspeaker_pairing_beacon_t local;
  h2_bloomspeaker_pairing_candidate_t
      candidates[H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES];
  uint64_t minimum_observation_ms;
  uint64_t stale_after_ms;
  int minimum_rssi;
} h2_bloomspeaker_pairing_t;

void h2_bloomspeaker_pairing_init(h2_bloomspeaker_pairing_t *pairing,
                                  uint64_t device_tag, uint64_t ticket,
                                  uint32_t epoch);

int h2_bloomspeaker_pairing_encode(
    const h2_bloomspeaker_pairing_beacon_t *beacon,
    uint8_t out[H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE]);

int h2_bloomspeaker_pairing_decode(
    const uint8_t *data, size_t len,
    h2_bloomspeaker_pairing_beacon_t *out_beacon);

int h2_bloomspeaker_pairing_observe(
    h2_bloomspeaker_pairing_t *pairing,
    const h2_bloomspeaker_pairing_beacon_t *beacon, int rssi,
    uint64_t now_ms);

uint64_t h2_bloomspeaker_pairing_select(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t now_ms);

void h2_bloomspeaker_pairing_set_claim(
    h2_bloomspeaker_pairing_t *pairing, uint64_t target);

bool h2_bloomspeaker_pairing_lock(h2_bloomspeaker_pairing_t *pairing,
                                  uint64_t target);

bool h2_bloomspeaker_pairing_mutual_claim(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t now_ms,
    uint64_t *out_peer);

h2_bloomspeaker_pairing_role_t h2_bloomspeaker_pairing_role(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t peer);

#ifdef __cplusplus
}
#endif

#endif
