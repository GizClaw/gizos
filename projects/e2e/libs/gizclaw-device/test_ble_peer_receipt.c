#include "ble_peer_receipt.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
  /* Unit data for the actual producer serializer, never a BLE observation.
   * The Python consumer test checks this unchanged stdout plus independently
   * supplied historical collector facts. No Host, clock or radio is opened. */
  const h2_gizclaw_e2e_ble_peer_receipt_t value = {
      .version = "unit-r1", .execution = "11111111111111111111111111111111",
      .window = "h2e2e-aaaaaaaaaaaaaaaa", .revision = 42u, .url_bytes = 112u,
      .url_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .started_utc = "2020-01-01T00:00:02.000000000Z",
      .finished_utc = "2020-01-01T00:00:22.000000000Z", .elapsed_ms = 20000u,
      .completed_connections = 2u, .chunks = {14u, 14u}, .rc = 0, .qualified = true};
  assert(h2_gizclaw_e2e_ble_peer_write_receipt(stdout, &value) > 0);
  assert(fflush(stdout) == 0);
  return 0;
}
