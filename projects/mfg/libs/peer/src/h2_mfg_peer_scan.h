#ifndef H2_MFG_PEER_SCAN_H
#define H2_MFG_PEER_SCAN_H
#include "h2/pal/hal/h2_pal_ble.h"
#include <string.h>

typedef struct h2_mfg_peer_scan {
  uint8_t address[6];
  h2_pal_ble_addr_t observed;
  h2_pal_ble_addr_t rejected[8];
  unsigned rejected_count;
  unsigned rejected_next;
  int found;
  int connectable;
  int has_service;
  int any_address;
  unsigned reports;
} h2_mfg_peer_scan_t;

static inline void h2_mfg_peer_scan_retry_all(h2_mfg_peer_scan_t* state) {
  state->rejected_count = state->rejected_next = 0;
}

static inline void h2_mfg_peer_scan_reject_current(h2_mfg_peer_scan_t* state) {
  if (!state->any_address || !state->found) return;
  for (unsigned i = 0; i < state->rejected_count; ++i)
    if (state->rejected[i].type == state->observed.type &&
        memcmp(state->rejected[i].value, state->observed.value, 6) == 0)
      return;
  state->rejected[state->rejected_next] = state->observed;
  state->rejected_next = (state->rejected_next + 1) % 8;
  if (state->rejected_count < 8) ++state->rejected_count;
}

static inline int h2_mfg_peer_scan_set_uid(h2_mfg_peer_scan_t* state,
                                           const char* uid) {
  if (state == NULL || uid == NULL || strlen(uid) != 12)
    return H2_PAL_ERR_INVALID_ARG;
  for (unsigned i = 0; i < 6; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      char c = uid[i * 2 + j];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return H2_PAL_ERR_INVALID_ARG;
      value = value * 16u + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    /* Native Bluetooth addresses use least-significant octet first. */
    state->address[5u - i] = (uint8_t)value;
  }
  state->any_address = strcmp(uid, "000000000000") == 0;
  h2_mfg_peer_scan_retry_all(state);
  return H2_PAL_OK;
}

static inline void h2_mfg_peer_scan_begin(h2_mfg_peer_scan_t* state) {
  state->found = state->connectable = state->has_service = 0;
  state->reports = 0;
}

static inline bool h2_mfg_peer_scan_has_service(
    const h2_pal_ble_scan_result_t* result) {
  static const uint8_t uuid[16] = {0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf,
                                   0x0b, 0xaa, 0xe2, 0x53, 0xd8, 0x3e,
                                   0x70, 0xb5, 0xa4, 0x71};
  if (result == NULL) return false;
  for (size_t i = 0; i < result->service_uuid_count; ++i)
    if (result->service_uuids[i].len == 16 &&
        memcmp(result->service_uuids[i].data, uuid, 16) == 0)
      return true;
  return false;
}

static inline bool h2_mfg_peer_scan_update(
    h2_mfg_peer_scan_t* state, const h2_pal_ble_scan_result_t* result) {
  if (result == NULL) return false;
  if (state->any_address)
    for (unsigned i = 0; i < state->rejected_count; ++i)
      if (state->rejected[i].type == result->addr.type &&
          memcmp(state->rejected[i].value, result->addr.value, 6) == 0)
        return false;
  if (memcmp(result->addr.value, state->address, 6) != 0) {
    if (!state->any_address || !h2_mfg_peer_scan_has_service(result))
      return false;
    memcpy(state->address, result->addr.value, 6);
    state->connectable = state->has_service = 0;
  }
  state->observed = result->addr;
  ++state->reports;
  state->connectable |= result->connectable;
  state->has_service |= h2_mfg_peer_scan_has_service(result);
  state->found = state->connectable && state->has_service;
  return state->found != 0;
}
#endif
