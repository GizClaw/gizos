#include "h2_bloomspeaker_pairing.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <limits.h>
#include <string.h>

#define H2_BLOOMSPEAKER_PAIRING_MAGIC 0xb7u
#define H2_BLOOMSPEAKER_PAIRING_VERSION 1u
#define H2_BLOOMSPEAKER_PAIRING_DEFAULT_OBSERVATION_MS 600u
#define H2_BLOOMSPEAKER_PAIRING_DEFAULT_STALE_MS 1500u
#define H2_BLOOMSPEAKER_PAIRING_DEFAULT_MIN_RSSI (-75)

typedef struct h2_bloomspeaker_pairing_rank {
  uint64_t device_tag;
  uint64_t ticket;
} h2_bloomspeaker_pairing_rank_t;

static void write_little_endian(uint8_t *out, uint64_t value, size_t bytes) {
  for (size_t index = 0u; index < bytes; ++index) {
    out[index] = (uint8_t)(value >> (index * 8u));
  }
}

static uint64_t read_little_endian(const uint8_t *data, size_t bytes) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < bytes; ++index) {
    value |= (uint64_t)data[index] << (index * 8u);
  }
  return value;
}

static bool beacon_state_valid(h2_bloomspeaker_pairing_beacon_state_t state) {
  return state == H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE ||
         state == H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED ||
         state == H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED;
}

static bool beacon_valid(const h2_bloomspeaker_pairing_beacon_t *beacon) {
  return beacon != NULL && beacon->device_tag != 0u &&
         beacon->device_tag <= H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK &&
         beacon->claim_target <= H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK &&
         beacon_state_valid(beacon->state) &&
         ((beacon->state == H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE &&
           beacon->claim_target == 0u) ||
          (beacon->state != H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE &&
           beacon->claim_target != 0u));
}

static bool rank_before(const h2_bloomspeaker_pairing_rank_t *left,
                        const h2_bloomspeaker_pairing_rank_t *right) {
  return left->ticket < right->ticket ||
         (left->ticket == right->ticket &&
          left->device_tag < right->device_tag);
}

static bool candidate_eligible(
    const h2_bloomspeaker_pairing_t *pairing,
    const h2_bloomspeaker_pairing_candidate_t *candidate, uint64_t now_ms) {
  return pairing != NULL && candidate != NULL && candidate->occupied &&
         candidate->seen_count >= 2u &&
         candidate->rssi >= pairing->minimum_rssi &&
         now_ms >= candidate->first_seen_ms &&
         now_ms - candidate->first_seen_ms >= pairing->minimum_observation_ms &&
         now_ms >= candidate->last_seen_ms &&
         now_ms - candidate->last_seen_ms <= pairing->stale_after_ms;
}

static const h2_bloomspeaker_pairing_candidate_t *find_candidate(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t device_tag) {
  for (size_t index = 0u;
       index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES; ++index) {
    const h2_bloomspeaker_pairing_candidate_t *candidate =
        &pairing->candidates[index];
    if (candidate->occupied &&
        candidate->beacon.device_tag == device_tag) {
      return candidate;
    }
  }
  return NULL;
}

void h2_bloomspeaker_pairing_init(h2_bloomspeaker_pairing_t *pairing,
                                  uint64_t device_tag, uint64_t ticket,
                                  uint32_t epoch) {
  if (pairing == NULL) {
    return;
  }
  memset(pairing, 0, sizeof(*pairing));
  pairing->local.device_tag =
      device_tag & H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK;
  pairing->local.ticket = ticket;
  pairing->local.epoch = epoch;
  pairing->local.state = H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE;
  pairing->minimum_observation_ms =
      H2_BLOOMSPEAKER_PAIRING_DEFAULT_OBSERVATION_MS;
  pairing->stale_after_ms = H2_BLOOMSPEAKER_PAIRING_DEFAULT_STALE_MS;
  pairing->minimum_rssi = H2_BLOOMSPEAKER_PAIRING_DEFAULT_MIN_RSSI;
}

int h2_bloomspeaker_pairing_encode(
    const h2_bloomspeaker_pairing_beacon_t *beacon,
    uint8_t out[H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE]) {
  if (!beacon_valid(beacon) || out == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out[0] = H2_BLOOMSPEAKER_PAIRING_MAGIC;
  out[1] = H2_BLOOMSPEAKER_PAIRING_VERSION;
  out[2] = (uint8_t)beacon->state;
  write_little_endian(out + 3u, beacon->device_tag, 5u);
  write_little_endian(out + 8u, beacon->ticket, 8u);
  write_little_endian(out + 16u, beacon->epoch, 4u);
  write_little_endian(out + 20u, beacon->claim_target, 5u);
  return H2_PAL_OK;
}

int h2_bloomspeaker_pairing_decode(
    const uint8_t *data, size_t len,
    h2_bloomspeaker_pairing_beacon_t *out_beacon) {
  if (data == NULL || out_beacon == NULL ||
      len != H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE ||
      data[0] != H2_BLOOMSPEAKER_PAIRING_MAGIC ||
      data[1] != H2_BLOOMSPEAKER_PAIRING_VERSION) {
    return H2_PAL_ERR_FORMAT;
  }
  h2_bloomspeaker_pairing_beacon_t decoded = {
      .device_tag = read_little_endian(data + 3u, 5u),
      .ticket = read_little_endian(data + 8u, 8u),
      .epoch = (uint32_t)read_little_endian(data + 16u, 4u),
      .claim_target = read_little_endian(data + 20u, 5u),
      .state = (h2_bloomspeaker_pairing_beacon_state_t)data[2],
  };
  uint8_t canonical[H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE];
  if (h2_bloomspeaker_pairing_encode(&decoded, canonical) != H2_PAL_OK ||
      memcmp(canonical, data, sizeof(canonical)) != 0) {
    return H2_PAL_ERR_FORMAT;
  }
  *out_beacon = decoded;
  return H2_PAL_OK;
}

int h2_bloomspeaker_pairing_observe(
    h2_bloomspeaker_pairing_t *pairing,
    const h2_bloomspeaker_pairing_beacon_t *beacon, int rssi,
    uint64_t now_ms) {
  if (pairing == NULL || !beacon_valid(beacon) ||
      beacon->device_tag == pairing->local.device_tag) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_bloomspeaker_pairing_candidate_t *free_candidate = NULL;
  h2_bloomspeaker_pairing_candidate_t *oldest_candidate = NULL;
  for (size_t index = 0u;
       index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES; ++index) {
    h2_bloomspeaker_pairing_candidate_t *candidate =
        &pairing->candidates[index];
    if (candidate->occupied &&
        candidate->beacon.device_tag == beacon->device_tag) {
      if (candidate->beacon.epoch != beacon->epoch ||
          candidate->beacon.ticket != beacon->ticket) {
        candidate->first_seen_ms = now_ms;
        candidate->seen_count = 1u;
      } else if (candidate->seen_count < UINT8_MAX) {
        candidate->seen_count++;
      }
      candidate->beacon = *beacon;
      candidate->last_seen_ms = now_ms;
      candidate->rssi = rssi;
      return H2_PAL_OK;
    }
    if (!candidate->occupied && free_candidate == NULL) {
      free_candidate = candidate;
    }
    if (candidate->occupied &&
        (oldest_candidate == NULL ||
         candidate->last_seen_ms < oldest_candidate->last_seen_ms)) {
      oldest_candidate = candidate;
    }
  }
  h2_bloomspeaker_pairing_candidate_t *candidate =
      free_candidate != NULL ? free_candidate : oldest_candidate;
  if (candidate == NULL) {
    return H2_PAL_ERR_FULL;
  }
  *candidate = (h2_bloomspeaker_pairing_candidate_t){
      .beacon = *beacon,
      .first_seen_ms = now_ms,
      .last_seen_ms = now_ms,
      .rssi = rssi,
      .seen_count = 1u,
      .occupied = true,
  };
  return H2_PAL_OK;
}

uint64_t h2_bloomspeaker_pairing_select(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t now_ms) {
  if (pairing == NULL || pairing->local.device_tag == 0u) {
    return 0u;
  }
  h2_bloomspeaker_pairing_rank_t
      ranks[H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES + 1u];
  size_t count = 1u;
  ranks[0] = (h2_bloomspeaker_pairing_rank_t){pairing->local.device_tag,
                                               pairing->local.ticket};
  for (size_t index = 0u;
       index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES; ++index) {
    const h2_bloomspeaker_pairing_candidate_t *candidate =
        &pairing->candidates[index];
    if (candidate_eligible(pairing, candidate, now_ms) &&
        candidate->beacon.state != H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED) {
      ranks[count++] = (h2_bloomspeaker_pairing_rank_t){
          candidate->beacon.device_tag, candidate->beacon.ticket};
    }
  }
  for (size_t index = 1u; index < count; ++index) {
    h2_bloomspeaker_pairing_rank_t value = ranks[index];
    size_t position = index;
    while (position > 0u && rank_before(&value, &ranks[position - 1u])) {
      ranks[position] = ranks[position - 1u];
      position--;
    }
    ranks[position] = value;
  }
  for (size_t index = 0u; index < count; ++index) {
    if (ranks[index].device_tag != pairing->local.device_tag) {
      continue;
    }
    if ((index & 1u) == 0u) {
      return index + 1u < count ? ranks[index + 1u].device_tag : 0u;
    }
    return ranks[index - 1u].device_tag;
  }
  return 0u;
}

void h2_bloomspeaker_pairing_set_claim(h2_bloomspeaker_pairing_t *pairing,
                                       uint64_t target) {
  if (pairing == NULL || target > H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK ||
      target == pairing->local.device_tag) {
    return;
  }
  pairing->local.claim_target = target;
  pairing->local.state = target == 0u
                             ? H2_BLOOMSPEAKER_PAIRING_BEACON_PAIRABLE
                             : H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED;
}

bool h2_bloomspeaker_pairing_lock(h2_bloomspeaker_pairing_t *pairing,
                                  uint64_t target) {
  if (pairing == NULL || target == 0u ||
      pairing->local.state != H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED ||
      pairing->local.claim_target != target) {
    return false;
  }
  pairing->local.state = H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED;
  return true;
}

bool h2_bloomspeaker_pairing_mutual_claim(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t now_ms,
    uint64_t *out_peer) {
  if (out_peer != NULL) {
    *out_peer = 0u;
  }
  if (pairing == NULL || pairing->local.claim_target == 0u ||
      pairing->local.state != H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED) {
    return false;
  }
  const h2_bloomspeaker_pairing_candidate_t *candidate =
      find_candidate(pairing, pairing->local.claim_target);
  if (!candidate_eligible(pairing, candidate, now_ms) ||
      (candidate->beacon.state != H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED &&
       candidate->beacon.state != H2_BLOOMSPEAKER_PAIRING_BEACON_LOCKED) ||
      candidate->beacon.claim_target != pairing->local.device_tag) {
    return false;
  }
  if (out_peer != NULL) {
    *out_peer = candidate->beacon.device_tag;
  }
  return true;
}

h2_bloomspeaker_pairing_role_t h2_bloomspeaker_pairing_role(
    const h2_bloomspeaker_pairing_t *pairing, uint64_t peer) {
  if (pairing == NULL || peer == 0u) {
    return H2_BLOOMSPEAKER_PAIRING_ROLE_NONE;
  }
  const h2_bloomspeaker_pairing_candidate_t *candidate =
      find_candidate(pairing, peer);
  if (candidate == NULL) {
    return H2_BLOOMSPEAKER_PAIRING_ROLE_NONE;
  }
  const h2_bloomspeaker_pairing_rank_t local = {
      pairing->local.device_tag, pairing->local.ticket};
  const h2_bloomspeaker_pairing_rank_t remote = {
      candidate->beacon.device_tag, candidate->beacon.ticket};
  return rank_before(&local, &remote)
             ? H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL
             : H2_BLOOMSPEAKER_PAIRING_ROLE_CENTRAL;
}
