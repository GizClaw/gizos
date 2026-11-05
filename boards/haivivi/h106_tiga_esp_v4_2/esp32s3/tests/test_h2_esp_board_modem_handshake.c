#include "h2_esp_board_modem_handshake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { TIMEOUT = -2, FAILURE = -3 };
typedef struct {
  int high[2], low, qsclk[2], cold, power_error;
  unsigned high_calls, qsclk_calls, cycles, fresh_calls;
  bool powered, aborted;
  char abort_stage, sequence[64];
  unsigned count;
} fixture_t;

static void record(fixture_t* f, char stage) {
  assert(f->count + 1u < sizeof(f->sequence));
  f->sequence[f->count++] = stage;
  f->sequence[f->count] = '\0';
  if (stage == f->abort_stage) f->aborted = true;
}
static int high(void* user, unsigned attempts) {
  fixture_t* f = user;
  assert(f->high_calls < 2u);
  assert(attempts == (f->high_calls == 0u ? 3u : 2u));
  record(f, 'H');
  return f->high[f->high_calls++];
}
static void cld(void* user) { record(user, 'C'); }
static int low(void* user) {
  fixture_t* f = user;
  record(f, 'L');
  return f->low;
}
static bool aborted(void* user) { return ((fixture_t*)user)->aborted; }
static void report(void* user, bool after_cld, int rc) {
  (void)rc;
  record(user, after_cld ? 'c' : 'r');
}
static int power(void* user, int level) {
  fixture_t* f = user;
  record(f, level ? 'I' : 'O');
  if (f->power_error == (level ? 2 : 1)) return FAILURE;
  f->powered = level != 0;
  return 0;
}
static void wait_ms(unsigned ms) { assert(ms == 1200u); }
static int cold(void* user) {
  fixture_t* f = user;
  assert(f->powered);
  record(f, 'B');
  return f->cold;
}
static void cycle_report(void* user, int rc) {
  (void)rc;
  record(user, 'b');
}
static int cycle(void* user) {
  fixture_t* f = user;
  assert(++f->cycles == 1u);
  const h2_modem_power_cycle_ops_t ops = {power, wait_ms, cold, cycle_report};
  return h2_modem_handshake_power_cycle(&ops, user);
}
static void fresh(void* user, bool is_cold, bool recovery) {
  fixture_t* f = user;
  ++f->fresh_calls;
  if (recovery) assert(is_cold && f->cycles == 1u && f->qsclk_calls == 1u);
  record(f, is_cold ? 'F' : 'W');
}
static int qsclk(void* user) {
  fixture_t* f = user;
  assert(f->qsclk_calls < 2u);
  record(f, 'Q');
  return f->qsclk[f->qsclk_calls++];
}
static const h2_modem_handshake_ops_t ops = {high,   cld,   low,   aborted,
                                             report, cycle, fresh, qsclk};

static void check(fixture_t f, int expected, const char* sequence,
                  unsigned cycles, bool powered) {
  assert(h2_modem_handshake_open(&ops, &f, TIMEOUT) == expected);
  assert(strcmp(f.sequence, sequence) == 0);
  assert(f.cycles == cycles);
  assert(f.powered == powered);
}

int main(void) {
  /* RI/high-rate answer: preserve the session, no cold boot work. */
  check((fixture_t){.powered = true}, 0, "HrWQ", 0, true);
  check((fixture_t){.powered = true, .high = {TIMEOUT, 0}}, 0, "HCHcrWQ", 0,
        true);
  check((fixture_t){.powered = true, .high = {TIMEOUT, TIMEOUT}}, 0, "HCHcLrFQ",
        0, true);
  /* Silent probes: one real cycle, cold-ready, then fresh configuration. */
  check(
      (fixture_t){.powered = true, .high = {TIMEOUT, TIMEOUT}, .low = TIMEOUT},
      0, "HCHcLrOIBbFQ", 1, true);
  check((fixture_t){.powered = true,
                    .high = {TIMEOUT, TIMEOUT},
                    .low = TIMEOUT,
                    .cold = TIMEOUT},
        TIMEOUT, "HCHcLrOIBbO", 1, false);
  /* Probe responds, QSCLK times out: recover once, including cold boot work. */
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, 0}}, 0, "HrWQOIBbFQ", 1,
        true);
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, 0}, .cold = FAILURE},
        FAILURE, "HrWQOIBbO", 1, false);
  /* Both triggers must enforce at most one cycle per open. The caller retains
   * its existing transport cleanup when QSCLK fails after a successful cycle.
   */
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, TIMEOUT}}, TIMEOUT,
        "HrWQOIBbFQ", 1, true);
  check((fixture_t){.powered = true,
                    .high = {TIMEOUT, TIMEOUT},
                    .low = TIMEOUT,
                    .qsclk = {TIMEOUT, 0}},
        TIMEOUT, "HCHcLrOIBbFQ", 1, true);
  check((fixture_t){.powered = true, .qsclk = {FAILURE, 0}}, FAILURE, "HrWQ", 0,
        true);
  /* Exhausted open budget is not a completed silent probe/AT timeout. */
  check(
      (fixture_t){
          .powered = true, .high = {TIMEOUT, TIMEOUT}, .abort_stage = 'H'},
      TIMEOUT, "H", 0, true);
  check((fixture_t){.powered = true,
                    .high = {TIMEOUT, TIMEOUT},
                    .low = TIMEOUT,
                    .abort_stage = 'L'},
        TIMEOUT, "HCHcL", 0, true);
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, 0}, .abort_stage = 'Q'},
        TIMEOUT, "HrWQ", 0, true);
  /* Failed rail operations must attempt cleanup, without a second cycle. */
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, 0}, .power_error = 2},
        FAILURE, "HrWQOIbO", 1, false);
  check((fixture_t){.powered = true, .qsclk = {TIMEOUT, 0}, .power_error = 1},
        FAILURE, "HrWQObO", 1, true);
  puts("15 handshake regression cases passed");
  return 0;
}
