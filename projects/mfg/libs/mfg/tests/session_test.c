#define _POSIX_C_SOURCE 200809L
#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"
#include "h2_sqlite.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the real controller, locks and persistent Loader record. Optical
 * and acoustic behavior belongs to device acceptance, so hardware is bounded
 * here and can inject a failed join deterministically. */
static int close_result;
static unsigned closes;
static unsigned enters;

int h2_mfg_hardware_enter(h2_mfg_t* m, uint64_t now) {
  (void)now;
  ++enters;
  m->model.ready = m->model.current == 0;
  return 0;
}
int h2_mfg_hardware_tick(h2_mfg_t* m, uint64_t now, const char** action) {
  (void)now;
  (void)action;
  if (m->model.current == 1) m->model.ready = m->ble_probed;
  return 0;
}
int h2_mfg_hardware_leave(h2_mfg_t* m) {
  (void)m;
  return 0;
}
int h2_mfg_hardware_close(h2_mfg_t* m) {
  (void)m;
  ++closes;
  return close_result;
}
int h2_mfg_ui_draw(h2_mfg_t* m, uint64_t now) {
  (void)m;
  (void)now;
  return 0;
}

static h2_pal_result_t random_bytes(void* user, uint8_t* out, size_t size) {
  (void)user;
  memset(out, 0x12, size);
  return 0;
}
static h2_pal_result_t partition(void* user,
                                 h2_pal_power_boot_partition_t* out) {
  *out = (h2_pal_power_boot_partition_t){.id = user == NULL ? 1u
                                                            : *(uint32_t*)user};
  return 0;
}
static const h2_pal_power_vtable_t power_vtable = {
    .get_running_boot_partition = partition,
    .get_next_boot_partition = partition,
};
static const h2_pal_power_api_t power = {.vtable = &power_vtable};
static const h2_pal_crypto_vtable_t crypto_vtable = {.random = random_bytes};
static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_vtable};
static const h2_mfg_step_t steps[] = {{"display", H2_MFG_DISPLAY, 0},
                                      {"ble", H2_MFG_BLE, 0}};
static const h2_mfg_profile_t profile = {
    .board = "test", .revision = 1, .steps = steps, .total = 2};

typedef struct test_run {
  h2_runtime_t runtime;
  h2_loader_t loader;
  h2_mfg_t* mfg;
  h2_sqlite_t* sqlite;
  char pref_path[80];
  h2_pal_mutex_t* operation;
  pthread_t thread;
  int result;
} test_run_t;

static void setup(test_run_t* run) {
  memset(run, 0, sizeof(*run));
  (void)snprintf(run->pref_path, sizeof(run->pref_path),
                 "/tmp/gizos-mfg-session-XXXXXX");
  int fd = mkstemp(run->pref_path);
  assert(fd >= 0 && close(fd) == 0);
  const h2_sqlite_config_t sqlite = {.path = run->pref_path};
  assert(h2_sqlite_create(&sqlite, &run->sqlite) == 0);
  h2_pal_pref_namespace_t* ns;
  assert(h2_pal_pref_open(h2_sqlite_pref_api(run->sqlite), "h2loader",
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "boot_intent", H2_LOADER_BOOT_INTENT_LOADER) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  run->runtime = (h2_runtime_t){.board = "test",
                                .mem = h2_desktop_platform_default_allocator(),
                                .sync = h2_desktop_platform_sync_api(),
                                .time = h2_desktop_platform_time_api(),
                                .log = h2_desktop_platform_log_api(),
                                .crypto = &crypto,
                                .pref = h2_sqlite_pref_api(run->sqlite)};
  run->loader.config.pref = run->runtime.pref;
  run->loader.config.package.allocator = run->runtime.mem;
  run->loader.config.power = &power;
  run->loader.config.h2loader_partition_id = 1;
  run->loader.config.app_partition_id = 2;
  run->loader.config.board = "test";
  run->loader.config.target = "host";
  assert(h2_atomic_int_init(&run->loader.mfg_gate_bypass, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&run->loader.implemented_commands, 0) ==
         H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&run->loader.command_availability, 0) ==
         H2_ATOMIC_OK);
  const h2_pal_mutex_config_t mc = {.name = "test-operation",
                                    .allocator = run->runtime.mem};
  assert(h2_pal_mutex_create(run->runtime.sync, &mc, &run->operation) == 0);
  assert(h2_mfg_create(&run->runtime, &profile, &run->mfg) == 0);
}

static void* run_session(void* user) {
  test_run_t* r = user;
  r->result =
      h2_mfg_run(r->mfg, &r->loader, r->runtime.sync, r->operation, NULL, NULL);
  return NULL;
}

static void await_step(test_run_t* r, unsigned current) {
  for (unsigned i = 0; i < 200; ++i) {
    assert(h2_pal_mutex_lock(r->runtime.sync, r->mfg->mutex) == 0);
    int ready = r->mfg->initialized && r->mfg->model.current == current &&
                r->mfg->hardware_ticket == r->mfg->model.ticket;
    assert(h2_pal_mutex_unlock(r->runtime.sync, r->mfg->mutex) == 0);
    if (ready) return;
    assert(h2_pal_time_sleep_ms(r->runtime.time, 10) == 0);
  }
  assert(h2_pal_mutex_lock(r->runtime.sync, r->mfg->mutex) == 0);
  fprintf(stderr,
          "MFG wait: expected=%u initialized=%d running=%d stopping=%d "
          "current=%u error=%d cleanup=%d ticket=%u hardware_ticket=%u\n",
          current, r->mfg->initialized, r->mfg->running, r->mfg->stopping,
          r->mfg->model.current, r->mfg->model.error, r->mfg->cleanup_result,
          r->mfg->model.ticket, r->mfg->hardware_ticket);
  assert(h2_pal_mutex_unlock(r->runtime.sync, r->mfg->mutex) == 0);
  assert(!"session did not enter expected step");
}

static int control(test_run_t* r, const char* action, const char* step,
                   h2_loader_command_transport_t transport) {
  assert(h2_pal_mutex_lock(r->runtime.sync, r->operation) == 0);
  char ticket[9], output[768];
  assert(h2_pal_mutex_lock(r->runtime.sync, r->mfg->mutex) == 0);
  (void)snprintf(ticket, sizeof(ticket), "%08" PRIx32, r->mfg->model.ticket);
  assert(h2_pal_mutex_unlock(r->runtime.sync, r->mfg->mutex) == 0);
  const char* argv[] = {action, step, ticket};
  int rc = h2_mfg_command(r->mfg, transport, 3, argv, output, sizeof(output));
  assert(strstr(output, "H2_MFG_STATUS") != NULL);
  assert(h2_pal_mutex_unlock(r->runtime.sync, r->operation) == 0);
  return rc;
}

static void teardown(test_run_t* r) {
  assert(h2_mfg_destroy(&r->mfg) == 0 && r->mfg == NULL);
  assert(h2_pal_mutex_destroy(r->runtime.sync, r->operation) == 0);
  h2_atomic_int_destroy(&r->loader.mfg_gate_bypass);
  h2_atomic_int_destroy(&r->loader.implemented_commands);
  h2_atomic_int_destroy(&r->loader.command_availability);
  h2_sqlite_destroy(r->sqlite);
  assert(unlink(r->pref_path) == 0);
}

static void stop_with_operation_lock_and_retry_failed_join(void) {
  test_run_t r;
  setup(&r);
  close_result = 0;
  assert(pthread_create(&r.thread, NULL, run_session, &r) == 0);
  await_step(&r, 0);
  assert(h2_mfg_destroy(&r.mfg) == H2_PAL_ERR_BUSY && r.mfg != NULL);
  assert(control(&r, "confirm", "display",
                 H2_LOADER_COMMAND_TRANSPORT_SERIAL) == 0);
  await_step(&r, 1);
  assert(control(&r, "probe", "ble", H2_LOADER_COMMAND_TRANSPORT_SERIAL) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(control(&r, "probe", "ble", H2_LOADER_COMMAND_TRANSPORT_BLE) == 0);
  assert(control(&r, "confirm", "ble", H2_LOADER_COMMAND_TRANSPORT_BLE) == 0);
  h2_loader_mfg_summary_t summary;
  int present;
  assert(h2_loader_mfg_read(r.runtime.pref, r.runtime.mem, &summary,
                            &present) == 0);
  assert(present && summary.total == 2 && summary.step_status[0] == 1 &&
         summary.step_status[1] == 1);
  assert(memcmp(&r.loader.status.mfg, &summary, sizeof(summary)) == 0);
  assert(h2_pal_mutex_lock(r.runtime.sync, r.operation) == 0);
  close_result = H2_PAL_ERR_BUSY;
  assert(h2_mfg_before_disruptive(r.mfg, H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE) ==
         H2_PAL_ERR_BUSY);
  assert(pthread_join(r.thread, NULL) == 0 && r.result == H2_PAL_ERR_BUSY);
  close_result = 0;
  assert(h2_mfg_before_disruptive(r.mfg, H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE) ==
         0);
  assert(h2_pal_mutex_unlock(r.runtime.sync, r.operation) == 0);
  assert(closes >= 2);
  teardown(&r);
}

static void auto_without_recovery_runs_mfg(void) {
  test_run_t r;
  setup(&r);
  h2_pal_pref_namespace_t* ns;
  assert(h2_pal_pref_open(r.runtime.pref, "h2loader",
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "boot_intent", H2_LOADER_BOOT_INTENT_AUTO) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  unsigned prior = enters;
  assert(pthread_create(&r.thread, NULL, run_session, &r) == 0);
  await_step(&r, 0);
  assert(h2_mfg_before_disruptive(r.mfg, H2_LOADER_DISRUPTIVE_REBOOT_LOADER) ==
         0);
  assert(pthread_join(r.thread, NULL) == 0 && r.result == H2_PAL_EXIT);
  assert(enters > prior);
  teardown(&r);
}

static void loader_candidate_bypasses_interactive_gate(void) {
  test_run_t r;
  setup(&r);
  uint32_t running = 2;
  h2_pal_power_api_t candidate_power = {.user = &running,
                                        .vtable = &power_vtable};
  r.loader.config.power = &candidate_power;
  unsigned prior = enters;
  int result =
      h2_mfg_run(r.mfg, &r.loader, r.runtime.sync, r.operation, NULL, NULL);
  if (result != H2_PAL_EXIT)
    fprintf(stderr, "Recovery bypass result=%d\n", result);
  assert(result == H2_PAL_EXIT);
  assert(enters == prior);
  h2_loader_mfg_summary_t summary;
  int present = 1;
  assert(h2_loader_mfg_read(r.runtime.pref, r.runtime.mem, &summary,
                            &present) == 0);
  assert(!present); /* Recovery must not even reset an acceptance record. */
  teardown(&r);
}

static void completed_auto_without_app_keeps_summary_ui(void) {
  test_run_t r;
  setup(&r);
  assert(h2_loader_mfg_ensure_acceptance_revision(r.runtime.pref, 2, 1) == 0);
  h2_loader_mfg_summary_t summary = {
      .total = 2,
      .step_status = {H2_LOADER_MFG_STEP_PASSED, H2_LOADER_MFG_STEP_PASSED}};
  assert(h2_loader_mfg_write(r.runtime.pref, &summary) == 0);
  h2_pal_pref_namespace_t* ns;
  assert(h2_pal_pref_open(r.runtime.pref, "h2loader",
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "boot_intent", H2_LOADER_BOOT_INTENT_AUTO) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  assert(pthread_create(&r.thread, NULL, run_session, &r) == 0);
  await_step(&r, 2);
  assert(h2_mfg_before_disruptive(r.mfg, H2_LOADER_DISRUPTIVE_REBOOT_LOADER) ==
         0);
  assert(pthread_join(r.thread, NULL) == 0 && r.result == H2_PAL_EXIT);
  teardown(&r);
}

int main(void) {
  completed_auto_without_app_keeps_summary_ui();
  loader_candidate_bypasses_interactive_gate();
  stop_with_operation_lock_and_retry_failed_join();
  auto_without_recovery_runs_mfg();
  puts(
      "MFG session: BLE transport proof, durable results, operation-lock "
      "handoff, failed join retention/retry and AUTO convergence passed");
  return 0;
}
