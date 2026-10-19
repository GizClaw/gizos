#include "ledger.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
  static const char line[] = "H2_GIZCLAW_E2E stage=coverage-begin case=resource\n";
  char buffer[512] = {0};
  h2_gizclaw_e2e_ledger_t ledger;
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_OK);
  assert(ledger.records == 1u && ledger.size == sizeof(line) - 1u);
  assert(memcmp(ledger.data, line, ledger.size) == 0);
  assert(ledger.crc32 == 0xf03ce93fu); /* independent Python zlib vector */
  uint32_t checksum = ledger.crc32;
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error != H2_PAL_OK && ledger.crc32 == checksum && ledger.records == 1u);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, 10u) == H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_ERR_NO_SPACE && ledger.size == 0u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_NO_SPACE);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) == H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, NULL, 0u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) != H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) == H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 2u);
  assert(ledger.error != H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_init(NULL, buffer, sizeof(buffer)) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
