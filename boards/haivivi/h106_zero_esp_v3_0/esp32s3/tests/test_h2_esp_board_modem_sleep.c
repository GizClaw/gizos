#include "h2_esp_board_modem_sleep.h"
#include "h2_atomic_static.h"
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

static char sequence[16];
static unsigned count;
static char fail;
static unsigned qsclk_failures;
static int record(char stage) {
  sequence[count++] = stage;
  sequence[count] = '\0';
  return stage == fail ? -1 : 0;
}
static int ppp(void) { return record('P'); }
static int exit_cmux(void) { return record('C'); }
static int ri(void) { return record('R'); }
static int qsclk(const char* command) {
  assert(strcmp(command, "AT+QSCLK=1") == 0);
  const int rc = record('Q');
  if (qsclk_failures != 0u) {
    --qsclk_failures;
    return -2;
  }
  return rc;
}
static int quiet(const char* command) {
  if (strcmp(command, "AT+QCFG=\"urc/ri/other\",\"pulse\",120,1") == 0)
    return record('U');
  assert(strcmp(command, "AT+QSIMSTAT=1") == 0);
  return record('S');
}

static int drain_tx(void) { return record('T'); }
static unsigned waited, probes;
static int ready_after;
static int drive(int level) { return record(level ? 'H' : 'L'); }
static void wait_ms(unsigned ms) {
  assert(ms == 100u || ms == 500u);
  waited += ms;
}
static int ready(void) {
  assert(waited >= 500u && sequence[0] == 'H');
  return ++probes >= (unsigned)ready_after ? 0 : -2;
}
static void test_dtr(void) {
  const h2_zero_esp_modem_dtr_ops_t ops = {drive, drain_tx, ready, wait_ms};
  count = 0;
  fail = 0;
  assert(h2_zero_esp_modem_dtr_gate(&ops, 1) == 0);
  assert(strcmp(sequence, "TL") == 0);
  count = 0;
  fail = 'T';
  assert(h2_zero_esp_modem_dtr_gate(&ops, 1) == -1);
  assert(strcmp(sequence, "T") == 0);
  count = 0;
  fail = 'L';
  assert(h2_zero_esp_modem_dtr_gate(&ops, 1) == -1);
  count = 0;
  fail = 0;
  ready_after = 3;
  probes = waited = 0;
  assert(h2_zero_esp_modem_dtr_gate(&ops, 0) == 0);
  assert(probes == 3u && waited == 700u && strcmp(sequence, "H") == 0);
  count = 0;
  ready_after = 11;
  probes = waited = 0;
  assert(h2_zero_esp_modem_dtr_gate(&ops, 0) == -2);
  assert(probes == 10u && waited == 1400u);
  count = 0;
  fail = 'H';
  probes = waited = 0;
  assert(h2_zero_esp_modem_dtr_gate(&ops, 0) == -1);
  assert(probes == 0u && waited == 0u);
  count = 0;
  fail = 0;
}
static int swaps, fail_swap, fail_enter;
static int data_swap(void) {
  record('X');
  return ++swaps == fail_swap ? -4 : 0;
}
static int data_enter(void) {
  record('D');
  return fail_enter ? -5 : 0;
}
static void test_data_channel(void) {
  const h2_zero_esp_modem_data_ops_t ops = {data_swap, data_enter};
  count = swaps = fail_swap = fail_enter = 0;
  assert(h2_zero_esp_modem_data_start(&ops) == 0);
  assert(strcmp(sequence, "XD") == 0);
  count = swaps = 0;
  fail_enter = 1;
  assert(h2_zero_esp_modem_data_start(&ops) == -5);
  assert(strcmp(sequence, "XDX") == 0);
  count = swaps = 0;
  fail_swap = 2;
  assert(h2_zero_esp_modem_data_start(&ops) == -4);
  assert(strcmp(sequence, "XDX") == 0);
  count = swaps = 0;
  fail_swap = 1;
  assert(h2_zero_esp_modem_data_start(&ops) == -4);
  assert(strcmp(sequence, "X") == 0);
  count = 0;
}

H2_ATOMIC_DEFINE_STATIC(int, sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);

H2_ATOMIC_DEFINE_STATIC(bool, race_start, false);

static void* race_prepare(void* result) {
  while (!h2_atomic_load(&race_start)) sched_yield();
  *(bool*)result = h2_zero_esp_modem_sleep_ready(&sleep_state);
  return NULL;
}

static void* race_commit(void* result) {
  while (!h2_atomic_load(&race_start)) sched_yield();
  *(bool*)result = h2_zero_esp_modem_sleep_commit_session(&sleep_state, true);
  return NULL;
}

static void test_prepare_commit_race(void) {
  /* Race the actual shared receipt helpers on separate host threads. Either
   * preparation wins and retention is allowed, or commit wins and denies it. */
  for (unsigned attempt = 0; attempt < 1024u; ++attempt) {
    bool prepared = false, retained = false;
    pthread_t preparing, committing;
    h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
    h2_atomic_store(&race_start, false);
    assert(pthread_create(&preparing, NULL, race_prepare, &prepared) == 0);
    assert(pthread_create(&committing, NULL, race_commit, &retained) == 0);
    h2_atomic_store(&race_start, true);
    assert(pthread_join(preparing, NULL) == 0);
    assert(pthread_join(committing, NULL) == 0);
    assert(retained == prepared);
    assert(!h2_zero_esp_modem_sleep_ready(&sleep_state));
    assert(!h2_zero_esp_modem_sleep_invalidate(&sleep_state));
  }
}

static void* race_late_operation(void* result) {
  while (!h2_atomic_load(&race_start)) sched_yield();
  *(bool*)result = h2_zero_esp_modem_sleep_admit(&sleep_state);
  return NULL;
}

static void test_prepared_session_rejects_late_operations(void) {
  for (unsigned attempt = 0u; attempt < 512u; ++attempt) {
    bool admitted = true, retained = false;
    pthread_t operation, committing;
    h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
    assert(h2_zero_esp_modem_sleep_admit(&sleep_state));
    assert(h2_zero_esp_modem_sleep_complete(&sleep_state, true));
    assert(!h2_zero_esp_modem_sleep_admit(&sleep_state));
    assert(h2_atomic_load(&sleep_state) == H2_ZERO_ESP_MODEM_SLEEP_READY);
    h2_atomic_store(&race_start, false);
    assert(pthread_create(&operation, NULL, race_late_operation, &admitted) ==
           0);
    assert(pthread_create(&committing, NULL, race_commit, &retained) == 0);
    h2_atomic_store(&race_start, true);
    assert(pthread_join(operation, NULL) == 0);
    assert(pthread_join(committing, NULL) == 0);
    assert(!admitted && retained);
  }
  /* A nonterminal PAL attempt has no sealed receipt and can be retried. */
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_complete(&sleep_state, false));
  assert(h2_zero_esp_modem_sleep_admit(&sleep_state));
}

static void test_receipt_owner(void) {
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_complete(&sleep_state, false));
  /* Ordinary sleep failed/returned; a new supervisor has not prepared yet. */
  assert(!h2_zero_esp_modem_sleep_commit(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_complete(&sleep_state, false));
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_complete(&sleep_state, true));
  assert(h2_zero_esp_modem_sleep_commit(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_complete(&sleep_state, true));
}

static void test_passive_wake_preserves_modem(void) {
  for (unsigned wake = 0u; wake < 3u; ++wake) {
    /* A fresh MCU State inherits the modem, not a live host transport. */
    h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
    assert(h2_zero_esp_modem_sleep_commit_session(&sleep_state, false));
    assert(!h2_zero_esp_modem_sleep_invalidate(&sleep_state));
    assert(!h2_zero_esp_modem_sleep_commit_session(&sleep_state, false));
  }
  /* Once open/close has touched a session, incomplete prepare still denies
   * retention. Passive preservation cannot weaken the active-session guard. */
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(!h2_zero_esp_modem_sleep_commit_session(&sleep_state, true));
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_ready(&sleep_state));
  assert(h2_zero_esp_modem_sleep_commit_session(&sleep_state, true));

  uint32_t marker = 0x12345678u;
  bool powered = false;
  assert(!h2_zero_esp_modem_power_restore(marker, true, &powered));
  for (unsigned on = 0u; on < 2u; ++on) {
    h2_zero_esp_modem_power_record(&marker, on != 0u);
    for (unsigned wake = 0u; wake < 3u; ++wake) {
      assert(h2_zero_esp_modem_power_restore(marker, true, &powered));
      assert(powered == (on != 0u));
    }
    assert(!h2_zero_esp_modem_power_restore(marker, false, &powered));
  }
}

static void test_sleep_receipt(void) {
  /* An unstarted or stuck preparation cannot authorize modem retention. */
  assert(!h2_zero_esp_modem_sleep_commit(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_ready(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_invalidate(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_commit(&sleep_state));

  /* Successful preparation authorizes exactly one terminal commit. */
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_ready(&sleep_state));
  assert(h2_zero_esp_modem_sleep_commit(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_commit(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_invalidate(&sleep_state));

  /* Only a real preparation cleanup failure revokes an earlier receipt. */
  h2_atomic_store(&sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
  assert(h2_zero_esp_modem_sleep_ready(&sleep_state));
  assert(h2_zero_esp_modem_sleep_invalidate(&sleep_state));
  assert(!h2_zero_esp_modem_sleep_commit(&sleep_state));

  uint32_t marker = 0x12345678u;
  assert(!h2_zero_esp_modem_sleep_fallback_take(&marker, true));
  assert(marker == 0u);
  h2_zero_esp_modem_sleep_fallback_set(&marker, true);
  assert(!h2_zero_esp_modem_sleep_fallback_take(&marker, false));
  h2_zero_esp_modem_sleep_fallback_set(&marker, true);
  assert(h2_zero_esp_modem_sleep_fallback_take(&marker, true));
  assert(!h2_zero_esp_modem_sleep_fallback_take(&marker, true));
  h2_zero_esp_modem_sleep_fallback_set(&marker, true);
  h2_zero_esp_modem_sleep_fallback_set(&marker, false);
  assert(!h2_zero_esp_modem_sleep_fallback_take(&marker, true));
}

int main(void) {
  test_sleep_receipt();
  test_receipt_owner();
  test_passive_wake_preserves_modem();
  test_prepare_commit_race();
  test_prepared_session_rejects_late_operations();
  test_data_channel();
  test_dtr();
  const h2_zero_esp_modem_sleep_ops_t ops = {ppp,   exit_cmux, ri,
                                             quiet, qsclk,     drain_tx};
  assert(h2_zero_esp_modem_sleep_prepare(&ops) == 0);
  assert(strcmp(sequence, "PCRUSQT") == 0);
  count = 0;
  qsclk_failures = 1u;
  assert(h2_zero_esp_modem_sleep_prepare(&ops) == 0);
  assert(strcmp(sequence, "PCRUSQQT") == 0);
  count = 0;
  fail = 'Q';
  assert(h2_zero_esp_modem_sleep_prepare(&ops) == -1);
  assert(strcmp(sequence, "PCRUSQQT") == 0);
  const char quiet_failures[] = {'U', 'S'};
  for (unsigned i = 0; i < sizeof(quiet_failures); ++i) {
    count = 0;
    fail = quiet_failures[i];
    assert(h2_zero_esp_modem_sleep_prepare(&ops) == 0);
    assert(strcmp(sequence, "PCRUSQT") == 0);
  }
  const char* prefixes[] = {"PCRUSQT", "PCRUSQT", "PCRUSQT", "PCRUSQT"};
  const char failures[] = {'P', 'C', 'R', 'T'};
  for (unsigned i = 0; i < sizeof(failures); ++i) {
    count = 0;
    fail = failures[i];
    assert(h2_zero_esp_modem_sleep_prepare(&ops) == -1);
    assert(strcmp(sequence, prefixes[i]) == 0);
  }
  /* A failed retry cannot hide an earlier required-step failure. */
  count = 0;
  fail = 'P';
  qsclk_failures = 2u;
  assert(h2_zero_esp_modem_sleep_prepare(&ops) == -1);
  assert(strcmp(sequence, "PCRUSQQT") == 0);
  return 0;
}
