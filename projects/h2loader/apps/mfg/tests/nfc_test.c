#define _POSIX_C_SOURCE 200809L
#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"
#include "h2_sqlite.h"
#include "h2_pal_unsupported.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Actual Runtime mapping, acquisition workers, event queue and state snapshots.
 * The fake reader only supplies the next scan; MFG never calls it directly. */
typedef struct fixture {
  h2_runtime_t* rt;
  h2_mfg_t* m;
  h2_loader_t loader;
  h2_pal_mutex_t* operation;
  int run_result;
  h2_pal_mutex_t* script_mutex;
  h2_pal_nfc_scan_t scan;
  int scan_result;
  unsigned scans, saves;
  h2_runtime_component_t kind;
  h2_pal_periph_type_t type;
  int mapped;
  h2_pal_periph_api_t periph;
  h2_pal_nfc_api_t nfc;
  h2_runtime_component_mapper_t mapper;
} fixture_t;

static const h2_mfg_step_t step = {"nfc", H2_MFG_NFC, 0};
static const h2_mfg_profile_t profile = {
    .board = "test", .revision = 1, .steps = &step, .total = 1};

static int periph_get(void* user, h2_pal_periph_id_t id,
                      h2_pal_periph_info_t* out) {
  fixture_t* f = user;
  if (id != 201) return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_periph_info_t){.id = id, .type = f->type};
  return 0;
}
static int periph_list(void* user, h2_pal_periph_type_t type,
                       h2_pal_periph_cb_t cb, void* cb_user) {
  fixture_t* f = user;
  h2_pal_periph_info_t info;
  assert(periph_get(f, 201, &info) == 0);
  return type == 0 || type == f->type ? cb(cb_user, &info) : 0;
}
static int mappings(void* user, h2_runtime_component_t kind,
                    h2_runtime_component_mapping_cb_t cb, void* cb_user) {
  fixture_t* f = user;
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = H2_MFG_NFC_COMPONENT_ID, .periph_id = 201};
  return f->mapped && kind == f->kind ? cb(cb_user, &entry) : 0;
}
static int scan(void* user, h2_pal_periph_id_t id, h2_pal_nfc_scan_t* out) {
  fixture_t* f = user;
  const h2_pal_sync_api_t* sync = h2_desktop_platform_sync_api();
  assert(id == 201);
  assert(h2_pal_mutex_lock(sync, f->script_mutex) == 0);
  *out = f->scan;
  int rc = f->scan_result;
  ++f->scans;
  assert(h2_pal_mutex_unlock(sync, f->script_mutex) == 0);
  return rc;
}
static const h2_pal_periph_vtable_t periph_ops = {.list = periph_list,
                                                  .get = periph_get};
static const h2_pal_nfc_vtable_t nfc_ops = {.scan_nfc_reader = scan};
static const h2_runtime_component_mapper_vtable_t mapper_ops = {.list =
                                                                    mappings};

static void runtime_init(fixture_t* f, int mapped, int wrong_kind) {
  memset(f, 0, sizeof(*f));
  f->mapped = mapped;
  f->kind = wrong_kind ? H2_RUNTIME_COMPONENT_BUTTON
                       : H2_RUNTIME_COMPONENT_NFC_READER;
  f->type = wrong_kind ? H2_PAL_PERIPH_TYPE_SINGLE_BUTTON
                       : H2_PAL_PERIPH_TYPE_NFC_READER;
  f->periph = (h2_pal_periph_api_t){.user = f, .vtable = &periph_ops};
  f->nfc = (h2_pal_nfc_api_t){.user = f, .vtable = &nfc_ops};
  f->mapper = (h2_runtime_component_mapper_t){.user = f, .vtable = &mapper_ops};
  h2_runtime_config_t config = {
      .board = "test",
      .target = "host",
      .chip = "host",
      .mem = h2_desktop_platform_default_allocator(),
      .sync = h2_desktop_platform_sync_api(),
      .time = h2_desktop_platform_time_api(),
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .periph = &f->periph,
      .nfc = &f->nfc,
      .component_mapper = &f->mapper,
      .event_queue_capacity = 128,
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .log = h2_pal_unsupported_log_api(),
      .timer = h2_pal_unsupported_timer_api(),
      .fs = h2_pal_unsupported_fs_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_pal_unsupported_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_pal_unsupported_display_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  assert(h2_runtime_init(&config, &f->rt) == 0);
  h2_pal_mutex_config_t mc = {.name = "script", .allocator = f->rt->mem};
  assert(h2_pal_mutex_create(f->rt->sync, &mc, &f->script_mutex) == 0);
}

static int persist(void* user, const h2_loader_mfg_summary_t* summary) {
  fixture_t* f = user;
  assert(summary->total == 1);
  ++f->saves;
  return 0;
}
static h2_runtime_nfc_state_t snapshot(fixture_t* f) {
  h2_runtime_nfc_state_t state;
  assert(h2_runtime_component_state_nfc(f->rt, H2_MFG_NFC_COMPONENT_ID,
                                        &state) == 0);
  return state;
}
static void await_state(fixture_t* f, h2_runtime_nfc_state_status_t status,
                        uint64_t after) {
  for (unsigned i = 0; i < 400; ++i) {
    h2_runtime_nfc_state_t s = snapshot(f);
    if (s.status == status && s.updated_at_ms > after) return;
    assert(h2_pal_time_sleep_ms(f->rt->time, 5) == 0);
  }
  assert(!"Runtime NFC snapshot deadline");
}
static void set_scan(fixture_t* f, int present, int error) {
  /* Distinct publication milliseconds even on fast host runs. */
  assert(h2_pal_time_sleep_ms(f->rt->time, 5) == 0);
  assert(h2_pal_mutex_lock(f->rt->sync, f->script_mutex) == 0);
  f->scan = (h2_pal_nfc_scan_t){
      .stage = present ? H2_PAL_NFC_STAGE_DISCOVERED : H2_PAL_NFC_STAGE_ABSENT,
      .tag_type =
          present ? H2_PAL_NFC_TAG_TYPE_NTAG : H2_PAL_NFC_TAG_TYPE_UNKNOWN,
      .uid_len = present ? 4 : 0,
      .uid = {1, 2, 3, 4}};
  f->scan_result = error;
  assert(h2_pal_mutex_unlock(f->rt->sync, f->script_mutex) == 0);
}
static int enter(fixture_t* f) {
  uint64_t now;
  assert(h2_pal_time_get_monotonic_ms(f->rt->time, &now) == 0);
  f->m->hardware_ticket = f->m->model.ticket;
  return h2_mfg_hardware_enter(f->m, now);
}
static int tick(fixture_t* f, const char** action) {
  uint64_t now;
  assert(h2_pal_time_get_monotonic_ms(f->rt->time, &now) == 0);
  h2_mfg_drain_events(f->m);
  return h2_mfg_hardware_tick(f->m, now, action);
}
static void setup(fixture_t* f, int present) {
  runtime_init(f, 1, 0);
  set_scan(f, present, 0);
  assert(h2_mfg_create(f->rt, &profile, &f->m) == 0);
  h2_loader_mfg_summary_t summary = {.total = 1};
  assert(h2_mfg_model_init(&f->m->model, &profile, &summary, 1, persist, f) ==
         0);
  f->m->running = f->m->initialized = 1;
  h2_runtime_input_poll_config_t poll = {.tick_ms = 2,
                                         .nfc_poll_interval_ms = 5};
  assert(h2_runtime_input_start(f->rt, &poll) == 0);
  await_state(
      f,
      present ? H2_RUNTIME_NFC_STATE_DISCOVERED : H2_RUNTIME_NFC_STATE_ABSENT,
      0);
  assert(enter(f) == 0);
}
static int command(fixture_t* f, const char* action) {
  char ticket[9], output[512];
  if (f->operation) assert(h2_pal_mutex_lock(f->rt->sync, f->operation) == 0);
  (void)snprintf(ticket, sizeof(ticket), "%08" PRIx32, f->m->model.ticket);
  const char* args[] = {action, "nfc", ticket};
  int rc = h2_mfg_command(f->m, H2_LOADER_COMMAND_TRANSPORT_SERIAL, 3, args,
                          output, sizeof(output));
  if (f->operation) assert(h2_pal_mutex_unlock(f->rt->sync, f->operation) == 0);
  return rc;
}
static void cleanup(fixture_t* f) {
  if (f->m) {
    f->m->running = 0;
    assert(h2_mfg_destroy(&f->m) == 0);
  }
  assert(h2_runtime_input_stop(f->rt) == 0);
  assert(h2_pal_mutex_destroy(f->rt->sync, f->script_mutex) == 0);
  h2_runtime_deinit(f->rt);
}
static void present(fixture_t* f) {
  uint64_t before = snapshot(f).updated_at_ms;
  set_scan(f, 1, 0);
  await_state(f, H2_RUNTIME_NFC_STATE_DISCOVERED, before);
  const char* action = NULL;
  assert(tick(f, &action) == 0 && action == NULL && f->m->model.ready);
}

static void mapping_errors(void) {
  fixture_t f;
  runtime_init(&f, 0, 0);
  assert(h2_mfg_create(f.rt, &profile, &f.m) == H2_PAL_ERR_NOT_FOUND && !f.m);
  cleanup(&f);
  runtime_init(&f, 1, 1);
  assert(h2_mfg_create(f.rt, &profile, &f.m) == H2_PAL_ERR_INVALID_ARG && !f.m);
  cleanup(&f);
}
static void empty_baseline_and_retry(void) {
  fixture_t f;
  setup(&f, 0);
  uint64_t empty_ms = snapshot(&f).updated_at_ms;
  assert(f.m->nfc.armed && !f.m->model.ready);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_time_sleep_ms(f.rt->time, 25) == 0);
  assert(snapshot(&f).updated_at_ms ==
         empty_ms); /* Runtime suppresses unchanged ABSENT. */
  present(&f);
  assert(command(&f, "confirm") == 0 && f.saves == 1);
  assert(command(&f, "retry") == 0 && enter(&f) == 0);
  const char* action = NULL;
  assert(tick(&f, &action) == 0 && !f.m->model.ready && !f.m->nfc.armed);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  uint64_t before = snapshot(&f).updated_at_ms;
  set_scan(&f, 0, 0);
  await_state(&f, H2_RUNTIME_NFC_STATE_ABSENT, before);
  assert(tick(&f, &action) == 0 && f.m->nfc.armed);
  present(&f);
  assert(command(&f, "confirm") == 0);
  assert(h2_mfg_hardware_leave(f.m) == 0 && !f.m->nfc.active &&
         !f.m->nfc.evidence);
  cleanup(&f);
}
static void queued_baseline_and_errors(void) {
  fixture_t f;
  setup(&f, 1); /* DISCOVERED remains queued from before attempt. */
  const char* action = NULL;
  assert(tick(&f, &action) == 0 && !f.m->model.ready);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  uint64_t before = snapshot(&f).updated_at_ms;
  set_scan(&f, 0, 0);
  await_state(&f, H2_RUNTIME_NFC_STATE_ABSENT, before);
  assert(tick(&f, &action) == 0);
  present(&f);
  /* New error, no intervening MFG tick: CLI must revalidate current health. */
  before = snapshot(&f).updated_at_ms;
  set_scan(&f, 0, H2_PAL_ERR_UNSUPPORTED);
  await_state(&f, H2_RUNTIME_NFC_STATE_ERROR, before);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  assert(f.saves == 0 && !f.m->model.ready &&
         f.m->nfc.error == H2_PAL_ERR_UNSUPPORTED);
  assert(strstr(f.m->detail, "state_rc=0") != NULL &&
         strstr(f.m->detail, "result=-3") != NULL);
  char first_fault[sizeof(f.m->detail)];
  memcpy(first_fault, f.m->detail, sizeof(first_fault));
  before = snapshot(&f).updated_at_ms;
  set_scan(&f, 0, 0);
  await_state(&f, H2_RUNTIME_NFC_STATE_ABSENT, before);
  assert(tick(&f, &action) == H2_PAL_ERR_UNSUPPORTED);
  assert(strcmp(first_fault, f.m->detail) == 0);
  cleanup(&f);
}
static h2_runtime_event_t event_for(fixture_t* f, h2_runtime_nfc_state_t* s) {
  *s = (h2_runtime_nfc_state_t){.status = H2_RUNTIME_NFC_STATE_DISCOVERED,
                                .stage = H2_PAL_NFC_STAGE_DISCOVERED,
                                .tag_type = H2_PAL_NFC_TAG_TYPE_NTAG,
                                .uid_len = 4,
                                .uid = {1, 2, 3, 4},
                                .updated_at_ms = f->m->nfc.baseline_ms + 1};
  return (h2_runtime_event_t){.kind = H2_RUNTIME_COMPONENT_EVENT_NFC_STATE,
                              .component = H2_RUNTIME_COMPONENT_NFC_READER,
                              .component_id = H2_MFG_NFC_COMPONENT_ID,
                              .timestamp_ms = s->updated_at_ms,
                              .sequence = 100,
                              .payload = s,
                              .payload_size = sizeof(*s),
                              .payload_capacity = sizeof(*s)};
}
static void malformed_and_stale_events(void) {
  for (unsigned which = 0; which < 7; ++which) {
    fixture_t f;
    setup(&f, 0);
    h2_runtime_nfc_state_t s;
    h2_runtime_event_t event = event_for(&f, &s);
    if (which == 0) --event.payload_size;
    if (which == 1) s.uid_len = H2_PAL_NFC_UID_MAX_LEN + 1;
    if (which == 2) s.uid_len = 0;
    if (which == 3) s.tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
    if (which == 4) s.stage = H2_PAL_NFC_STAGE_ABSENT;
    if (which == 5) s.result = H2_PAL_ERR_IO;
    if (which == 6) --s.updated_at_ms;
    h2_mfg_nfc_event(f.m, &event);
    assert(f.m->nfc.error && !f.m->model.ready);
    assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
    cleanup(&f);
  }
  fixture_t f;
  setup(&f, 0);
  h2_runtime_nfc_state_t s;
  h2_runtime_event_t event = event_for(&f, &s);
  event.component_id = 99;
  h2_mfg_nfc_event(f.m, &event);
  assert(!f.m->nfc.evidence && !f.m->nfc.error);
  event.component_id = H2_MFG_NFC_COMPONENT_ID;
  event.component = H2_RUNTIME_COMPONENT_BUTTON;
  h2_mfg_nfc_event(f.m, &event);
  assert(!f.m->nfc.evidence && !f.m->nfc.error);
  event.component = H2_RUNTIME_COMPONENT_NFC_READER;
  event.timestamp_ms = s.updated_at_ms = f.m->nfc.baseline_ms;
  h2_mfg_nfc_event(f.m, &event);
  assert(!f.m->nfc.evidence && !f.m->nfc.error);
  event = event_for(&f, &s);
  h2_mfg_nfc_event(f.m, &event);
  assert(f.m->nfc.evidence);
  /* Duplicate sequence cannot add a removal and re-arm retained evidence. */
  s.status = H2_RUNTIME_NFC_STATE_ABSENT;
  s.stage = H2_PAL_NFC_STAGE_ABSENT;
  s.tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
  s.uid_len = 0;
  ++s.updated_at_ms;
  ++event.timestamp_ms;
  h2_mfg_nfc_event(f.m, &event);
  assert(f.m->nfc.evidence);
  assert(command(&f, "confirm") ==
         H2_PAL_ERR_INVALID_STATE); /* snapshot contradicts injected tag */
  cleanup(&f);
}

static void dispatch_budget_drop_and_stopped_producer(void) {
  fixture_t f;
  setup(&f, 0);
  present(&f);
  const h2_runtime_custom_event_t custom = {
      .id = H2_RUNTIME_CUSTOM_EVENT_ID(0x4d46, 1)};
  for (unsigned i = 0; i < 80; ++i)
    assert(h2_runtime_post_custom_event(f.rt, &custom) == 0);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  assert(f.m->runtime_events_pending && !f.m->model.ready);
  /* A new wake survives the bounded dispatch, even after the old wake is taken.
   */
  assert(h2_runtime_wait_notify(f.rt, 0) == 0);
  assert(command(&f, "confirm") == 0);
  cleanup(&f);

  setup(&f, 0);
  present(&f);
  unsigned posted = 0;
  while (h2_runtime_post_custom_event(f.rt, &custom) == 0) ++posted;
  assert(posted == 128);
  uint64_t before = snapshot(&f).updated_at_ms;
  set_scan(&f, 0, H2_PAL_ERR_IO);
  await_state(&f, H2_RUNTIME_NFC_STATE_ERROR, before);
  before = snapshot(&f).updated_at_ms;
  set_scan(&f, 1, 0);
  await_state(&f, H2_RUNTIME_NFC_STATE_DISCOVERED, before);
  uint32_t dropped = 0;
  assert(h2_runtime_dropped_event_count(f.rt, &dropped) == 0 && dropped > 0);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE && f.m->nfc.error);
  cleanup(&f);

  setup(&f, 0);
  present(&f);
  assert(h2_runtime_input_stop(f.rt) == 0);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE &&
         !f.m->model.ready);
  cleanup(&f);
}

static int random_bytes(void* user, uint8_t* out, size_t size) {
  (void)user;
  memset(out, 0x12, size);
  return 0;
}
static int partition(void* user, h2_pal_power_boot_partition_t* out) {
  (void)user;
  *out = (h2_pal_power_boot_partition_t){.id = 1};
  return 0;
}
static const h2_pal_crypto_vtable_t crypto_ops = {.random = random_bytes};
static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_ops};
static const h2_pal_power_vtable_t power_ops = {
    .get_running_boot_partition = partition,
    .get_next_boot_partition = partition};
static const h2_pal_power_api_t power = {.vtable = &power_ops};
static void* run_session(void* user) {
  fixture_t* f = user;
  f->run_result =
      h2_mfg_run(f->m, &f->loader, f->rt->sync, f->operation, NULL, NULL);
  return NULL;
}
static void await_session(fixture_t* f, int ready, int complete) {
  for (unsigned i = 0; i < 400; ++i) {
    assert(h2_pal_mutex_lock(f->rt->sync, f->m->mutex) == 0);
    int match = f->m->initialized &&
                f->m->hardware_ticket == f->m->model.ticket &&
                f->m->model.ready == ready &&
                h2_mfg_model_complete(&f->m->model) == complete;
    assert(h2_pal_mutex_unlock(f->rt->sync, f->m->mutex) == 0);
    if (match) return;
    assert(h2_pal_time_sleep_ms(f->rt->time, 5) == 0);
  }
  assert(!"MFG session deadline");
}
static void full_session_persistence_and_stop(void) {
  fixture_t f;
  runtime_init(&f, 1, 0);
  char path[] = "/tmp/gizos-mfg-nfc-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0 && close(fd) == 0);
  h2_sqlite_t* sqlite = NULL;
  h2_sqlite_config_t config = {.path = path};
  assert(h2_sqlite_create(&config, &sqlite) == 0);
  f.rt->pref = h2_sqlite_pref_api(sqlite);
  f.rt->crypto = &crypto;
  h2_pal_pref_namespace_t* ns;
  assert(h2_pal_pref_open(f.rt->pref, "h2loader", H2_PAL_PREF_OPEN_READ_WRITE,
                          &ns) == 0);
  assert(ns->set_u32(ns, "boot_intent", H2_LOADER_BOOT_INTENT_LOADER) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  f.loader.config.pref = f.rt->pref;
  f.loader.config.package.allocator = f.rt->mem;
  f.loader.config.power = &power;
  f.loader.config.h2loader_partition_id = 1;
  f.loader.config.app_partition_id = 2;
  f.loader.config.board = "test";
  f.loader.config.target = "host";
  assert(h2_atomic_int_init(&f.loader.mfg_gate_bypass, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&f.loader.implemented_commands, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&f.loader.command_availability, 0) == H2_ATOMIC_OK);
  h2_pal_mutex_config_t mc = {.name = "operation", .allocator = f.rt->mem};
  assert(h2_pal_mutex_create(f.rt->sync, &mc, &f.operation) == 0);
  assert(h2_mfg_create(f.rt, &profile, &f.m) == 0);
  set_scan(&f, 0, 0);
  h2_runtime_input_poll_config_t poll = {.tick_ms = 2,
                                         .nfc_poll_interval_ms = 5};
  assert(h2_runtime_input_start(f.rt, &poll) == 0);
  await_state(&f, H2_RUNTIME_NFC_STATE_ABSENT, 0);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, run_session, &f) == 0);
  await_session(&f, 0, 0);
  set_scan(&f, 1, 0);
  await_session(&f, 1, 0);
  assert(command(&f, "confirm") == 0);
  await_session(&f, 0, 1);
  h2_loader_mfg_summary_t summary;
  int found = 0;
  assert(h2_loader_mfg_read(f.rt->pref, f.rt->mem, &summary, &found) == 0);
  assert(found && summary.step_status[0] == H2_LOADER_MFG_STEP_PASSED);
  assert(command(&f, "retry") == 0);
  await_session(&f, 0, 0);
  assert(command(&f, "confirm") == H2_PAL_ERR_INVALID_STATE);
  /* Stop uses the actual session callback, including its bounded completion
   * wait. */
  assert(h2_mfg_before_disruptive(f.m, H2_LOADER_DISRUPTIVE_REBOOT_LOADER) ==
         0);
  assert(pthread_join(thread, NULL) == 0 && f.run_result == H2_PAL_EXIT);
  assert(h2_pal_mutex_destroy(f.rt->sync, f.operation) == 0);
  f.operation = NULL;
  cleanup(&f);
  h2_atomic_int_destroy(&f.loader.mfg_gate_bypass);
  h2_atomic_int_destroy(&f.loader.implemented_commands);
  h2_atomic_int_destroy(&f.loader.command_availability);
  h2_sqlite_destroy(sqlite);
  assert(unlink(path) == 0);
}

int main(void) {
  full_session_persistence_and_stop();
  dispatch_budget_drop_and_stopped_producer();
  mapping_errors();
  empty_baseline_and_retry();
  queued_baseline_and_errors();
  malformed_and_stale_events();
  return 0;
}
