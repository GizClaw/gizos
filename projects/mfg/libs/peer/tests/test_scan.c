#include "h2_mfg_peer_scan.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
  h2_mfg_peer_scan_t parsed = {0};
  assert(h2_mfg_peer_scan_set_uid(&parsed, "b81f3fcad4fa") == 0);
  assert(memcmp(parsed.address, (uint8_t[]){0xfa, 0xd4, 0xca, 0x3f, 0x1f, 0xb8},
                6) == 0);
  assert(h2_mfg_peer_scan_set_uid(&parsed, "b81f3fcad4fZ") ==
         H2_PAL_ERR_INVALID_ARG);
  h2_mfg_peer_scan_t state = {.address = {1, 2, 3, 4, 5, 6}};
  uint8_t uuid[] = {0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf, 0x0b, 0xaa,
                    0xe2, 0x53, 0xd8, 0x3e, 0x70, 0xb5, 0xa4, 0x71};
  h2_pal_ble_uuid_t service = {.data = uuid, .len = sizeof(uuid)};
  h2_pal_ble_scan_result_t adv = {.addr = {.value = {1, 2, 3, 4, 5, 6}},
                                  .connectable = true};
  h2_pal_ble_scan_result_t response = {.addr = {.value = {1, 2, 3, 4, 5, 6}},
                                       .scan_response = true,
                                       .service_uuids = &service,
                                       .service_uuid_count = 1};
  h2_mfg_peer_scan_begin(&state);
  assert(!h2_mfg_peer_scan_update(&state, &adv));
  assert(h2_mfg_peer_scan_update(&state, &response));
  assert(state.reports == 2);
  h2_mfg_peer_scan_begin(&state);
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(h2_mfg_peer_scan_update(&state, &adv));
  h2_mfg_peer_scan_begin(&state);
  response.addr.value[0] = 9;
  assert(!h2_mfg_peer_scan_update(&state, &adv));
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(!state.found && state.reports == 1);
  response.addr.value[0] = 1;
  uuid[0] = 0xff;
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(!h2_mfg_peer_scan_update(&state, NULL));
  uuid[0] = 0x1d;
  assert(h2_mfg_peer_scan_set_uid(&state, "000000000000") == 0);
  assert(state.any_address);
  h2_mfg_peer_scan_begin(&state);
  assert(!h2_mfg_peer_scan_update(&state, &adv));
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(h2_mfg_peer_scan_update(&state, &adv));
  assert(memcmp(state.observed.value, adv.addr.value, 6) == 0);
  /* A new radio address must not inherit the old one's connectable flag. */
  response.addr.value[0] = 9;
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(!state.connectable && state.has_service);
  adv.addr.value[0] = 9;
  assert(h2_mfg_peer_scan_update(&state, &adv));
  assert(state.observed.value[0] == 9);
  /* A failed service candidate must not starve a different nearby board. */
  h2_mfg_peer_scan_reject_current(&state);
  assert(state.rejected_count == 1);
  h2_mfg_peer_scan_begin(&state);
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(!h2_mfg_peer_scan_update(&state, &adv));
  assert(!state.found);
  response.addr.value[0] = adv.addr.value[0] = 8;
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(h2_mfg_peer_scan_update(&state, &adv));
  h2_mfg_peer_scan_reject_current(&state);
  assert(state.rejected_count == 2);
  h2_mfg_peer_scan_retry_all(&state);
  h2_mfg_peer_scan_begin(&state);
  response.addr.value[0] = adv.addr.value[0] = 9;
  assert(!h2_mfg_peer_scan_update(&state, &response));
  assert(h2_mfg_peer_scan_update(&state, &adv));
  /* The rejection ring is bounded and cannot overflow in a busy room. */
  for (unsigned i = 0; i < 16; ++i) {
    response.addr.value[0] = adv.addr.value[0] = (uint8_t)(20 + i);
    h2_mfg_peer_scan_begin(&state);
    assert(!h2_mfg_peer_scan_update(&state, &response));
    assert(h2_mfg_peer_scan_update(&state, &adv));
    h2_mfg_peer_scan_reject_current(&state);
    assert(state.rejected_count <= 8);
  }
  assert(h2_mfg_peer_scan_set_uid(&state, "b81f3fcad4fa") == 0);
  assert(!state.any_address);
  assert(state.rejected_count == 0);
  h2_mfg_peer_scan_reject_current(&state);
  assert(state.rejected_count == 0);
  return 0;
}
