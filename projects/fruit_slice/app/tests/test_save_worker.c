#include "h2_fruit_slice_save.h"
#include "h2_desktop_platform.h"
#include "h2_fruit_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      exit(1);                                                \
    }                                                         \
  } while (0)

static struct {
  h2_pal_pref_namespace_t ns;
  h2_pal_queue_t* entered;
  h2_pal_queue_t* release;
  uint8_t progress[H2_FRUIT_SLICE_PROGRESS_BYTES];
  uint8_t cards[H2_FRUIT_SLICE_CARDS_BYTES];
  uint8_t staged[H2_FRUIT_SLICE_CARDS_BYTES + H2_FRUIT_SLICE_PROGRESS_BYTES];
  size_t staged_size;
  uint32_t best;
  unsigned kind, active, opens, closes, order[16], count;
  unsigned block, fail_commit, fail_close, fail_start, fail_join, joins;
} state;

static int set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                    const void* data, size_t size) {
  (void)ns;
  state.kind = !strcmp(key, H2_FRUIT_SLICE_PROGRESS_KEY)
                   ? H2_FRUIT_SLICE_SAVE_PROGRESS
                   : H2_FRUIT_SLICE_SAVE_CARDS;
  CHECK(!strcmp(key, H2_FRUIT_SLICE_PROGRESS_KEY) ||
        !strcmp(key, H2_FRUIT_SLICE_CARDS_KEY));
  CHECK(size <= sizeof(state.staged));
  CHECK(state.count < sizeof(state.order) / sizeof(state.order[0]));
  state.order[state.count++] = state.kind;
  if (state.block & state.kind) {
    const h2_pal_queue_api_t* queues = h2_desktop_platform_queue_api();
    CHECK(h2_pal_queue_send(queues, state.entered, &state.kind, 0u) ==
          H2_PAL_OK);
    unsigned token = 0u;
    CHECK(h2_pal_queue_recv(queues, state.release, &token, 5000u) == H2_PAL_OK);
    state.block &= ~state.kind;
  }
  memcpy(state.staged, data, size);
  state.staged_size = size;
  return H2_PAL_OK;
}

static int set_best(h2_pal_pref_namespace_t* ns, const char* key,
                    uint32_t value) {
  (void)ns;
  CHECK(!strcmp(key, H2_FRUIT_SLICE_PREF_BEST_KEY));
  state.kind = H2_FRUIT_SLICE_SAVE_BEST;
  state.order[state.count++] = state.kind;
  memcpy(state.staged, &value, sizeof(value));
  state.staged_size = sizeof(value);
  return H2_PAL_OK;
}

static int commit(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  if (state.fail_commit & state.kind) {
    state.fail_commit &= ~state.kind;
    return H2_PAL_ERR_IO;
  }
  void* destination =
      state.kind == H2_FRUIT_SLICE_SAVE_BEST       ? (void*)&state.best
      : state.kind == H2_FRUIT_SLICE_SAVE_PROGRESS ? (void*)state.progress
                                                   : (void*)state.cards;
  memcpy(destination, state.staged, state.staged_size);
  return H2_PAL_OK;
}

static int close_namespace(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  CHECK(state.active == 1u);
  state.active = 0u;
  ++state.closes;
  if (state.fail_close & state.kind) {
    state.fail_close &= ~state.kind;
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static int open_namespace(void* user, const char* name,
                          h2_pal_pref_open_mode_t mode,
                          h2_pal_pref_namespace_t** out) {
  (void)user;
  CHECK(!strcmp(name, H2_FRUIT_SLICE_PREF_NAMESPACE));
  CHECK(mode == H2_PAL_PREF_OPEN_READ_WRITE && state.active == 0u);
  state.active = 1u;
  ++state.opens;
  *out = &state.ns;
  return H2_PAL_OK;
}

static int start_task(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)user;
  CHECK(!strcmp(options->name, "fruit-slice/save"));
  CHECK(options->min_stack_size == 8192u);
  if (state.fail_start) {
    --state.fail_start;
    *out = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_pal_task_api_t* tasks = h2_desktop_platform_task_api();
  return h2_pal_task_start(tasks, options, entry, ctx, out);
}

static int join_task(void* user, h2_pal_task_t* task) {
  (void)user;
  ++state.joins;
  if (state.fail_join) {
    --state.fail_join;
    return H2_PAL_ERR_TASK;
  }
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}

static const h2_pal_pref_vtable_t pref_vtable = {.open = open_namespace};
static const h2_pal_pref_api_t preferences = {.vtable = &pref_vtable};
static const h2_pal_task_vtable_t task_vtable = {.start = start_task,
                                                 .join = join_task};
static const h2_pal_task_api_t tasks = {.vtable = &task_vtable};

static h2_runtime_t runtime;
static h2_fruit_slice_save_worker_t worker;

static h2_fruit_slice_save_snapshot_t request(unsigned fields) {
  h2_fruit_slice_save_snapshot_t out = {.fields = fields, .best = 41u};
  memset(out.progress, 0x12, sizeof(out.progress));
  memset(out.cards, 0x34, sizeof(out.cards));
  return out;
}

static void setup(void) {
  memset(&state, 0, sizeof(state));
  state.ns = (h2_pal_pref_namespace_t){.set_blob = set_blob,
                                       .set_u32 = set_best,
                                       .commit = commit,
                                       .close = close_namespace};
  runtime = (h2_runtime_t){.task = &tasks,
                           .pref = &preferences,
                           .time = h2_desktop_platform_time_api()};
  CHECK(h2_fruit_slice_save_worker_init(&worker, &runtime) == H2_PAL_OK);
  h2_pal_queue_config_t config = {
      .name = "save-test-barrier",
      .item_size = sizeof(unsigned),
      .item_count = 1u,
      .allocator = h2_desktop_platform_default_allocator()};
  CHECK(h2_pal_queue_create(h2_desktop_platform_queue_api(), &config,
                            &state.entered) == H2_PAL_OK);
  CHECK(h2_pal_queue_create(h2_desktop_platform_queue_api(), &config,
                            &state.release) == H2_PAL_OK);
}

static void teardown(void) {
  CHECK(worker.task == NULL && state.active == 0u);
  CHECK(state.opens == state.closes);
  h2_fruit_slice_save_worker_destroy(&worker);
  CHECK(!worker.initialized);
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), state.entered);
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), state.release);
}

static void test_immutable_ordered_snapshot(void) {
  setup();
  h2_fruit_slice_save_snapshot_t snapshot = request(7u);
  const h2_fruit_slice_save_snapshot_t original = snapshot;
  state.block = H2_FRUIT_SLICE_SAVE_PROGRESS;
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) == H2_PAL_OK);
  unsigned kind = 0u;
  CHECK(h2_pal_queue_recv(h2_desktop_platform_queue_api(), state.entered, &kind,
                          5000u) == H2_PAL_OK);
  CHECK(kind == H2_FRUIT_SLICE_SAVE_PROGRESS);
  memset(&snapshot, 0, sizeof(snapshot));
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) ==
        H2_PAL_ERR_BUSY);
  h2_fruit_slice_save_completion_t result;
  bool completed = true;
  CHECK(h2_fruit_slice_save_worker_poll(&worker, false, &result, &completed) ==
        H2_PAL_OK);
  CHECK(!completed && state.joins == 0u);
  h2_pal_task_t* task = worker.task;
  h2_fruit_slice_save_worker_destroy(&worker);
  CHECK(worker.task == task && worker.initialized);
  CHECK(h2_pal_queue_send(h2_desktop_platform_queue_api(), state.release, &kind,
                          0u) == H2_PAL_OK);
  CHECK(h2_fruit_slice_save_worker_poll(&worker, true, &result, &completed) ==
        H2_PAL_OK);
  CHECK(completed && state.count == 3u && state.joins == 1u);
  CHECK(state.order[0] == 1u && state.order[1] == 2u && state.order[2] == 4u);
  CHECK(state.best == original.best);
  CHECK(!memcmp(state.progress, original.progress, sizeof(state.progress)));
  CHECK(!memcmp(state.cards, original.cards, sizeof(state.cards)));
  CHECK(!memcmp(&result.snapshot, &original, sizeof(original)));
  teardown();
}

static void test_start_and_join_failures(void) {
  setup();
  h2_fruit_slice_save_snapshot_t snapshot = request(6u);
  state.fail_start = 1u;
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) ==
        H2_PAL_ERR_NO_MEMORY);
  CHECK(worker.task == NULL && state.opens == 0u);
  state.fail_commit = H2_FRUIT_SLICE_SAVE_PROGRESS;
  state.fail_join = 1u;
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) == H2_PAL_OK);
  h2_pal_task_t* task = worker.task;
  h2_fruit_slice_save_completion_t result;
  bool completed = false;
  CHECK(h2_fruit_slice_save_worker_poll(&worker, true, &result, &completed) ==
        H2_PAL_ERR_TASK);
  CHECK(!completed && worker.task == task);
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) ==
        H2_PAL_ERR_BUSY);
  CHECK(h2_fruit_slice_save_worker_poll(&worker, true, &result, &completed) ==
        H2_PAL_OK);
  CHECK(completed && result.progress_result == H2_PAL_ERR_IO &&
        result.cards_result == H2_PAL_OK);
  CHECK(state.opens == 2u && state.progress[0] == 0u &&
        state.cards[0] == 0x34u);
  snapshot.fields = H2_FRUIT_SLICE_SAVE_PROGRESS;
  state.fail_close = H2_FRUIT_SLICE_SAVE_PROGRESS;
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) == H2_PAL_OK);
  CHECK(h2_fruit_slice_save_worker_poll(&worker, true, &result, &completed) ==
        H2_PAL_OK);
  CHECK(completed && result.progress_result == H2_PAL_ERR_IO &&
        state.progress[0] == 0x12u);
  CHECK(h2_fruit_slice_save_worker_start(&worker, &snapshot) == H2_PAL_OK);
  CHECK(h2_fruit_slice_save_worker_poll(&worker, true, &result, &completed) ==
        H2_PAL_OK);
  CHECK(completed && result.progress_result == H2_PAL_OK && state.opens == 4u);
  teardown();
}

int main(void) {
  test_immutable_ordered_snapshot();
  test_start_and_join_failures();
  puts("save worker snapshot/order/lifetime tests passed");
  return 0;
}
