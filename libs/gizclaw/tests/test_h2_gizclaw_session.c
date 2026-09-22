#include "h2_desktop_platform.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_session.h"
#include "h2_gizclaw_session_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

/* These fakes sit at the typed RPC boundary. The real Session owns all state,
 * pagination, catalog memory, workspace preparation and callback forwarding. */
static h2_gizclaw_session_t *session;
static h2_pal_result_t attach_result;
static unsigned attaches;
static bool check_catalog_during_workspace;
static void assert_catalog(void);

enum { CATALOG_TEST_BYTES = 16384u };
typedef struct catalog_test_memory {
  unsigned attempts, allocations, frees, live;
  unsigned buffer_allocations, buffer_frees;
  unsigned fail_at;
  bool reject_allocations;
  struct {
    void *data;
    size_t size;
  } blocks[16];
} catalog_test_memory_t;

static void *catalog_test_alloc(void *user, size_t size) {
  catalog_test_memory_t *memory = user;
  ++memory->attempts;
  if (memory->reject_allocations || memory->attempts == memory->fail_at)
    return NULL;
  void *data = h2_pal_mem_alloc(h2_desktop_platform_default_allocator(), size);
  assert(data != NULL);
  for (size_t i = 0u; i < sizeof(memory->blocks) / sizeof(memory->blocks[0]); ++i) {
    if (memory->blocks[i].data == NULL) {
      memory->blocks[i].data = data;
      memory->blocks[i].size = size;
      ++memory->allocations;
      ++memory->live;
      if (size == CATALOG_TEST_BYTES)
        ++memory->buffer_allocations;
      return data;
    }
  }
  assert(false && "too many live Session allocations");
  return NULL;
}

static void catalog_test_free(void *user, void *data) {
  catalog_test_memory_t *memory = user;
  for (size_t i = 0u; i < sizeof(memory->blocks) / sizeof(memory->blocks[0]); ++i) {
    if (memory->blocks[i].data == data) {
      if (memory->blocks[i].size == CATALOG_TEST_BYTES)
        ++memory->buffer_frees;
      memory->blocks[i].data = NULL;
      ++memory->frees;
      --memory->live;
      h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
      return;
    }
  }
  assert(false && "free must belong to the Session allocator");
}

static const h2_pal_mem_vtable_t catalog_test_mem_vtable = {
    .alloc = catalog_test_alloc, .free = catalog_test_free};
static atomic_uint lists, gets, creates, reloads, conversations;
static bool list_failure, bad_revision, missing, close_during_list,
    reload_failure;
static bool paginated, empty_cycle, get_failure;
static const char *server_revision;
static unsigned closed_after_reload;
static h2_gizclaw_conversation_completion_fn terminal;
static h2_gizclaw_conversation_callback_fn on_event;
static void *terminal_user;
static unsigned terminal_count;
static unsigned audio_starts, audio_ends;
static h2_pal_result_t audio_start_result, audio_end_result;
static char audio_error_log[H2_PAL_LOG_MESSAGE_MAX];
static atomic_int last_cancel_source;
static unsigned end_noops;
static size_t downlink_writes;
static unsigned flushes;
/* Typed RPC order: d=delete, g=get, c=create, r=reload. */
static char rpc_trace[16];
static void trace(char step) {
  const size_t len = strlen(rpc_trace);
  assert(len + 1u < sizeof(rpc_trace));
  rpc_trace[len] = step;
  rpc_trace[len + 1u] = '\0';
}
/* This test mocks Service; capture Session's production ERROR formatting. */
void h2_gizclaw_service_flush_audio_log_internal(
    const h2_gizclaw_service_t *service, const h2_gizclaw_audio_log_t *log) {
  (void)service;
  for (size_t i = 0u; i < log->count; ++i) {
    /* A diagnostic sink may read Session state: the outer lock must be free. */
    h2_gizclaw_session_state_t state;
    assert(h2_gizclaw_session_snapshot(session, &state) == H2_PAL_OK);
    const char *message = log->messages[i];
    if (strstr(message, "stage=audio_end_not_open") != NULL)
      ++end_noops;
    if (log->levels[i] == H2_PAL_LOG_INFO)
      assert(strstr(message, "stage=interrupt_begin") != NULL);
    if (log->levels[i] != H2_PAL_LOG_ERROR)
      continue;
    assert(strlen(message) < sizeof(audio_error_log));
    strcpy(audio_error_log, message);
  }
}

static bool audio_input_empty;

h2_pal_result_t h2_gizclaw_service_audio_control_internal(
    h2_gizclaw_service_t *service, bool start, h2_gizclaw_audio_log_t *log,
    bool *out_empty) {
  if (out_empty != NULL)
    *out_empty = !start && audio_input_empty;
  (void)log;
  return start ? h2_gizclaw_service_audio_start(service)
               : h2_gizclaw_service_audio_end(service);
}

h2_pal_result_t h2_gizclaw_conversation_cancel_internal(
    h2_gizclaw_conversation_t *conversation, h2_gizclaw_audio_log_t *log,
    int source) {
  atomic_store(&last_cancel_source, source);
  (void)log;
  return h2_gizclaw_conversation_cancel(conversation);
}

static atomic_bool cancel_entered;
static atomic_uint_fast64_t now;
static uint64_t list_delay;
static atomic_bool gate_list, list_entered, waiter_entered;
static h2_pal_result_t wait_cond(void *user, h2_pal_cond_t *cond,
                                 h2_pal_mutex_t *mutex, uint32_t timeout) {
  (void)user;
  atomic_store(&waiter_entered, true);
  const h2_pal_sync_api_t *sync = h2_desktop_platform_sync_api();
  return sync->vtable->wait_cond(sync->user, cond, mutex, timeout);
}
static void wait_flag(atomic_bool *flag) {
  for (unsigned i = 0; i < 5000u; ++i) {
    if (atomic_load(flag))
      return;
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  }
  assert(false && "worker did not reach gate");
}
static h2_pal_result_t monotonic(void *user, uint64_t *out) {
  (void)user;
  *out = now;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = monotonic};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};
static char *copy(h2_gizclaw_resp_arena_t *a, const char *text) {
  char *p = h2_pal_mem_alloc(&a->allocator, strlen(text) + 1u);
  assert(p != NULL);
  strcpy(p, text);
  return p;
}
h2_pal_result_t h2_gizclaw_rpc_register(h2_gizclaw_service_t *service,
                                        const char *token, uint32_t timeout,
                                        h2_gizclaw_registration_result_t *out) {
  (void)service;
  (void)token;
  assert(timeout > 0u);
  strcpy(out->runtime_profile_name, "test-profile");
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workflow_list(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t collection,
                                             h2_gizclaw_str_t cursor,
                                             size_t limit, uint32_t timeout,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_workflow_page_t *out) {
  (void)service;
  (void)limit;
  assert(timeout > 0u);
  ++lists;
  atomic_store(&list_entered, true);
  while (atomic_load(&gate_list))
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  now += list_delay;
  if (list_failure)
    return H2_PAL_ERR_IO;
  if (close_during_list)
    assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  h2_gizclaw_session_state_t pending;
  assert(h2_gizclaw_session_snapshot(session, &pending) == H2_PAL_OK);
  assert(!pending.can_start);
  assert(h2_gizclaw_session_refresh(session, 100u) ==
         (close_during_list ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_BUSY));
  h2_gizclaw_resp_arena_t arena;
  assert(h2_gizclaw_resp_arena_begin(storage, &arena) == H2_PAL_OK);
  *out = (h2_gizclaw_workflow_page_t){0};
  out->runtime_profile_name = copy(&arena, "test-profile");
  out->runtime_profile_revision =
      copy(&arena, bad_revision && lists % 2u == 0u ? "v2" : server_revision);
  if (empty_cycle) {
    out->has_next = true;
    out->next_cursor = copy(
        &arena, cursor.len == 0u || strcmp(cursor.data, "a") != 0 ? "a" : "b");
    return h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
  }
  out->items = h2_pal_mem_alloc(&arena.allocator, sizeof(*out->items));
  assert(out->items != NULL);
  out->count = 1u;
  *out->items = (h2_gizclaw_workflow_t){
      .collection = copy(&arena, collection.data),
      .name = copy(&arena, cursor.len != 0u ? "second" : collection.data),
  };
  out->has_next = paginated && cursor.len == 0u;
  if (out->has_next)
    out->next_cursor = copy(&arena, "next");
  return h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
}
h2_pal_result_t
h2_gizclaw_rpc_workspace_get(h2_gizclaw_service_t *service,
                             h2_gizclaw_str_t name, uint32_t timeout,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_workspace_get_result_t *out) {
  (void)service;
  (void)timeout;
  (void)storage;
  ++gets;
  trace('g');
  if (check_catalog_during_workspace) {
    memset(storage->data, 0xa5, storage->capacity);
    assert_catalog();
  }
  if (get_failure)
    return H2_PAL_ERR_IO;
  if (missing)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_gizclaw_workspace_get_result_t){
      .workspace = {.name = (char *)name.data,
                    .workflow_name = "alpha",
                    .available = true},
      .runtime_profile_name = "test-profile",
      .runtime_profile_revision = (char *)server_revision,
  };
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t workflow, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  (void)service;
  (void)collection;
  (void)workflow;
  (void)name;
  (void)timeout;
  (void)storage;
  (void)out;
  ++creates;
  trace('c');
  missing = false;
  /* Server created it but response was lost. Session must reconcile. */
  return H2_PAL_ERR_IO;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_reload_with_options(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    const h2_gizclaw_workspace_parameters_patch_t *parameters, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out) {
  (void)service;
  (void)parameters;
  (void)timeout;
  (void)storage;
  ++reloads;
  trace('r');
  if (check_catalog_during_workspace) {
    memset(storage->data, 0x5a, storage->capacity);
    assert_catalog();
  }
  h2_gizclaw_session_state_t state;
  assert(h2_gizclaw_session_snapshot(session, &state) == H2_PAL_OK);
  assert(!state.can_start);
  assert(strcmp(state.target_workspace, name.data) == 0);
  h2_pal_result_t rc =
      h2_gizclaw_session_workspace_begin_internal(session, name, timeout);
  if (rc != H2_PAL_OK)
    return rc;
  if (reload_failure) {
    h2_gizclaw_session_workspace_finish_internal(session, H2_PAL_ERR_IO, NULL,
                                                 parameters);
    return H2_PAL_ERR_IO;
  }
  if (closed_after_reload)
    assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  *out = (h2_gizclaw_workspace_activation_t){
      .active_workspace_name = (char *)name.data,
      .workflow_name = "alpha",
      .runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING,
  };
  h2_gizclaw_session_workspace_finish_internal(session, H2_PAL_OK, out,
                                               parameters);
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_conversation_create(h2_gizclaw_service_t *service,
                               h2_gizclaw_str_t workspace,
                               h2_gizclaw_conversation_callback_fn callback,
                               h2_gizclaw_conversation_completion_fn completion,
                               void *user, h2_gizclaw_conversation_t **out) {
  (void)service;
  (void)workspace;
  on_event = callback;
  ++conversations;
  terminal = completion;
  terminal_user = user;
  *out = (h2_gizclaw_conversation_t *)&conversations;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service) {
  (void)service;
  ++audio_starts;
  return audio_start_result;
}
h2_pal_result_t h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service) {
  (void)service;
  ++audio_ends;
  return audio_end_result;
}
static unsigned releases, text_sends;
static h2_pal_result_t text_result;
static char text_seen[16];
void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *conversation) {
  assert(conversation == (h2_gizclaw_conversation_t *)&conversations);
  ++releases;
}
h2_pal_result_t h2_gizclaw_conversation_send_text_internal(
    h2_gizclaw_conversation_t *conversation, h2_gizclaw_str_t text,
    h2_gizclaw_audio_log_t *log) {
  assert(conversation == (h2_gizclaw_conversation_t *)&conversations);
  assert(log != NULL && text.len < sizeof(text_seen));
  memcpy(text_seen, text.data, text.len);
  text_seen[text.len] = '\0';
  ++text_sends;
  return text_result;
}
static void completed(void *user, h2_gizclaw_conversation_t *conversation,
                      const h2_gizclaw_operation_result_t *result) {
  (void)user;
  assert(result->result == H2_PAL_OK);
  ++terminal_count;
  h2_gizclaw_session_conversation_release(session, conversation);
}
static h2_gizclaw_session_config_t session_config(size_t collections) {
  static const char *const names[] = {"alpha", "beta"};
  attach_result = H2_PAL_OK;
  attaches = 0u;
  check_catalog_during_workspace = false;
  lists = gets = creates = reloads = conversations = terminal_count = 0u;
  audio_starts = audio_ends = 0u;
  releases = text_sends = 0u;
  text_result = H2_PAL_OK;
  text_seen[0] = '\0';
  flushes = 0u;
  downlink_writes = 0u;
  audio_input_empty = false;
  audio_start_result = audio_end_result = H2_PAL_OK;
  audio_error_log[0] = '\0';
  rpc_trace[0] = '\0';
  atomic_store(&cancel_entered, false);
  list_failure = bad_revision = missing = close_during_list = reload_failure =
      false;
  paginated = empty_cycle = get_failure = false;
  server_revision = "v1";
  closed_after_reload = 0u;
  now = list_delay = 0u;
  atomic_store(&gate_list, false);
  atomic_store(&list_entered, false);
  atomic_store(&waiter_entered, false);
  static h2_pal_sync_vtable_t sync_vtable;
  static h2_pal_sync_api_t sync_api;
  sync_api = *h2_desktop_platform_sync_api();
  sync_vtable = *sync_api.vtable;
  sync_vtable.wait_cond = wait_cond;
  sync_api.vtable = &sync_vtable;
  h2_gizclaw_session_config_t config = {
      .service = (h2_gizclaw_service_t *)&lists,
      .mem = h2_desktop_platform_default_allocator(),
      .sync = &sync_api,
      .time = &time_api,
      .collections = names,
      .collection_count = collections,
      .max_workflows = 4u,
      .catalog_bytes = CATALOG_TEST_BYTES,
  };
  return config;
}
static void setup(size_t collections) {
  h2_gizclaw_session_config_t config = session_config(collections);
  assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_OK);
}
static h2_gizclaw_session_state_t snapshot(void) {
  h2_gizclaw_session_state_t state;
  assert(h2_gizclaw_session_snapshot(session, &state) == H2_PAL_OK);
  return state;
}
static const h2_gizclaw_session_selection_t selection = {
    .collection = "alpha",
    .workflow_name = "alpha",
    .workspace_name = "my-chat",
};
static void teardown(void) {
  assert(h2_gizclaw_session_destroy(&session) == H2_PAL_OK);
  assert(session == NULL);
}

static void assert_catalog(void) {
  uint8_t bytes[4096];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_workflow_page_t catalog;
  assert(h2_gizclaw_session_catalog_copy(session, &storage, &catalog) ==
         H2_PAL_OK);
  assert(catalog.count == 2u);
  assert(strcmp(catalog.runtime_profile_name, "test-profile") == 0);
  assert(strcmp(catalog.runtime_profile_revision, server_revision) == 0);
  assert(strcmp(catalog.items[0].collection, "alpha") == 0);
  assert(strcmp(catalog.items[0].name, "alpha") == 0);
  assert(strcmp(catalog.items[1].collection, "beta") == 0);
  assert(strcmp(catalog.items[1].name, "beta") == 0);
}

static void test_catalog_buffer_lifetime(bool retain, bool separate) {
  catalog_test_memory_t memory = {0};
  const h2_pal_mem_api_t mem = {
      .user = &memory, .vtable = &catalog_test_mem_vtable};
  h2_gizclaw_session_config_t config = session_config(2u);
  config.mem = &mem;
  config.retain_catalog_buffer = retain;
  catalog_test_memory_t retained = {0};
  const h2_pal_mem_api_t retained_mem = {
      .user = &retained, .vtable = &catalog_test_mem_vtable};
  config.retained_allocator = separate ? &retained_mem : NULL;
  catalog_test_memory_t *buffers = retain && separate ? &retained : &memory;
  assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_OK);
  assert(buffers->buffer_allocations == (retain ? 2u : 0u));
  assert(retained.allocations == (retain && separate ? 2u : 0u));
  assert(!separate || !retain || memory.buffer_allocations == 0u);
  const unsigned retained_attempts = retained.attempts;
  const unsigned create_attempts = memory.attempts;
  /* Model fragmentation by rejecting every subsequent allocator request. */
  memory.reject_allocations = retain;
  retained.reject_allocations = true;
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  check_catalog_during_workspace = true;
  for (unsigned i = 0u; i < 4u; ++i) {
    assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_OK);
    assert_catalog();
    h2_gizclaw_session_selection_t other = selection;
    other.workspace_name = i % 2u == 0u ? "first" : "second";
    rpc_trace[0] = '\0';
    assert(h2_gizclaw_session_select(session, &other, 1000u) == H2_PAL_OK);
    assert_catalog();
  }
  assert(reloads == 4u);
  assert(buffers->buffer_allocations == (retain ? 2u : 9u));
  assert(buffers->buffer_frees == (retain ? 0u : 8u));

  /* A failed refresh never publishes its partially written scratch. */
  bad_revision = true;
  assert(h2_gizclaw_session_refresh(session, 1000u) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(snapshot().catalog == H2_GIZCLAW_SESSION_FAILED);
  uint8_t bytes[4096];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_workflow_page_t catalog = {.count = 99u};
  assert(h2_gizclaw_session_catalog_copy(session, &storage, &catalog) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(catalog.items == NULL && catalog.count == 0u);
  bad_revision = false;
  assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_OK);
  assert_catalog();

  rpc_trace[0] = '\0';
  reload_failure = true;
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_ERR_IO);
  assert_catalog();
  reload_failure = false;
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  assert_catalog();

  if (retain) {
    assert(memory.attempts == create_attempts);
    assert(buffers->buffer_allocations == 2u && buffers->buffer_frees == 0u);
  } else {
    memory.reject_allocations = true;
    assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_ERR_NO_MEMORY);
    memory.reject_allocations = false;
    assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_OK);
    assert_catalog();
  }
  const unsigned frees = buffers->buffer_frees;
  assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  assert(buffers->buffer_frees == frees);
  teardown();
  assert(retained.attempts == retained_attempts);
  assert(retained.live == 0u && retained.allocations == retained.frees);
  assert(memory.live == 0u && memory.allocations == memory.frees);
  assert(buffers->buffer_allocations == buffers->buffer_frees);
}

static void test_catalog_buffer_create_failure(void) {
  catalog_test_memory_t memory = {0};
  const h2_pal_mem_api_t mem = {
      .user = &memory, .vtable = &catalog_test_mem_vtable};
  h2_gizclaw_session_config_t config = session_config(1u);
  config.mem = &mem;
  config.retain_catalog_buffer = true;
  assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_OK);
  const unsigned create_attempts = memory.attempts;
  teardown();
  for (unsigned i = 1u; i <= create_attempts; ++i) {
    memory = (catalog_test_memory_t){.fail_at = i};
    attaches = 0u;
    session = (h2_gizclaw_session_t *)&memory;
    assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_ERR_NO_MEMORY);
    assert(session == NULL && attaches == 0u);
    assert(memory.live == 0u && memory.allocations == memory.frees);
  }
  memory = (catalog_test_memory_t){0};
  attach_result = H2_PAL_ERR_BUSY;
  assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_ERR_BUSY);
  assert(session == NULL && attaches == 1u);
  assert(memory.buffer_allocations == 2u && memory.buffer_frees == 2u);
  assert(memory.live == 0u && memory.allocations == memory.frees);
}
static void test_retained_allocator_create_failure(void) {
  catalog_test_memory_t memory = {0}, retained = {0};
  const h2_pal_mem_api_t mem = {
      .user = &memory, .vtable = &catalog_test_mem_vtable};
  const h2_pal_mem_api_t retained_mem = {
      .user = &retained, .vtable = &catalog_test_mem_vtable};
  h2_gizclaw_session_config_t config = session_config(1u);
  config.mem = &mem;
  config.retain_catalog_buffer = true;
  config.retained_allocator = &retained_mem;
  for (unsigned i = 1u; i <= 3u; ++i) {
    memory = (catalog_test_memory_t){0};
    retained = (catalog_test_memory_t){.fail_at = i};
    attaches = 0u;
    attach_result = i == 3u ? H2_PAL_ERR_BUSY : H2_PAL_OK;
    assert(h2_gizclaw_session_create(&config, &session) ==
           (i == 3u ? H2_PAL_ERR_BUSY : H2_PAL_ERR_NO_MEMORY));
    assert(session == NULL && attaches == (i == 3u ? 1u : 0u));
    assert(retained.allocations == i - 1u);
    assert(retained.buffer_allocations == retained.allocations);
    assert(retained.live == 0u && retained.allocations == retained.frees);
    assert(memory.buffer_allocations == 0u);
    assert(memory.live == 0u && memory.allocations == memory.frees);
  }
}

static void register_thread(void *out) {
  *(h2_pal_result_t *)out =
      h2_gizclaw_session_register(session, "token", 1000u);
}
static void select_thread(void *out) {
  *(h2_pal_result_t *)out =
      h2_gizclaw_session_select(session, &selection, 1000u);
}
static void test_waiting_selection(bool cancel, bool retain) {
  h2_gizclaw_session_config_t config = session_config(1u);
  config.retain_catalog_buffer = retain;
  assert(h2_gizclaw_session_create(&config, &session) == H2_PAL_OK);
  atomic_store(&gate_list, true);
  h2_pal_task_t *registration = NULL, *selection_thread = NULL;
  const h2_pal_task_api_t *tasks = h2_desktop_platform_task_api();
  h2_pal_result_t register_result = H2_PAL_ERR_IO,
                  select_result = H2_PAL_ERR_IO;
  assert(h2_pal_task_start(tasks, NULL, register_thread, &register_result,
                           &registration) == H2_PAL_OK);
  wait_flag(&list_entered);
  assert(h2_pal_task_start(tasks, NULL, select_thread, &select_result,
                           &selection_thread) == H2_PAL_OK);
  wait_flag(&waiter_entered);
  assert(!snapshot().can_start);
  assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_session_destroy(&session) == H2_PAL_ERR_BUSY);
  if (cancel)
    assert(h2_gizclaw_session_cancel_pending(session) == H2_PAL_OK);
  atomic_store(&gate_list, false);
  assert(h2_pal_task_join(tasks, registration) == H2_PAL_OK);
  assert(h2_pal_task_join(tasks, selection_thread) == H2_PAL_OK);
  assert(register_result == (cancel ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
  assert(select_result == (cancel ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
  assert(snapshot().can_start == !cancel);
  teardown();
}
typedef struct switch_context {
  h2_gizclaw_session_selection_t selection;
  h2_pal_result_t result;
} switch_context_t;
static void switch_thread(void *user) {
  switch_context_t *context = user;
  context->result =
      h2_gizclaw_session_select(session, &context->selection, 1000u);
}
static void start_thread(void *user) {
  *(h2_pal_result_t *)user = h2_gizclaw_session_audio_start(session);
}

static void test_control_boundaries(void) {
  const h2_pal_task_api_t *tasks = h2_desktop_platform_task_api();
  const h2_gizclaw_workspace_parameters_patch_t ptt = {
      .has_input = true,
      .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
      .has_initiative = true,
      .initiative = H2_GIZCLAW_CONVERSATION_INITIATIVE_PEER,
  };
  const h2_gizclaw_workspace_parameters_patch_t realtime = {
      .has_input = true,
      .input = H2_GIZCLAW_WORKSPACE_INPUT_REALTIME,
  };
  const h2_gizclaw_operation_result_t canceled = {
      .terminal_kind = H2_GIZCLAW_OPERATION_CANCELED,
      .result = H2_PAL_OK,
  };
  const h2_gizclaw_conversation_event_t reply = {
      .kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
  };
  for (unsigned phase = 0; phase < 5u; ++phase) {
    for (unsigned fail = 0; fail < 2u; ++fail) {
      setup(1u);
      assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
      switch_context_t context = {.selection = selection};
      context.selection.parameters = phase == 3u ? &realtime : &ptt;
      h2_gizclaw_conversation_t *conversation = NULL;
      assert(h2_gizclaw_session_conversation_create(
                 session, &context.selection, 1000u, NULL, completed, NULL,
                 &conversation) == H2_PAL_OK);
      assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
      assert(snapshot().conversation ==
             (phase == 3u ? H2_GIZCLAW_SESSION_CONVERSATION_CALLING
                          : H2_GIZCLAW_SESSION_CONVERSATION_RECORDING));
      if (phase == 1u || phase == 2u || phase == 4u) {
        audio_input_empty = phase == 4u;
        assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
        assert(snapshot().conversation ==
               (phase == 4u ? H2_GIZCLAW_SESSION_CONVERSATION_IDLE
                            : H2_GIZCLAW_SESSION_CONVERSATION_WAITING));
        assert(!snapshot().conversation_input_open);
      }
      if (phase == 2u || phase == 3u) {
        /* Text carries no state. */
        assert(on_event(terminal_user, conversation, &reply) == H2_PAL_OK);
        assert(snapshot().conversation ==
               (phase == 3u ? H2_GIZCLAW_SESSION_CONVERSATION_CALLING
                            : H2_GIZCLAW_SESSION_CONVERSATION_WAITING));
      }
      if (phase == 2u) {
        /* The input is sent: its generation completes, the wait goes on. */
        const h2_gizclaw_operation_result_t sent = {
            .terminal_kind = H2_GIZCLAW_OPERATION_FINISHED,
            .result = H2_PAL_OK,
        };
        terminal(terminal_user, conversation, &sent);
        assert(snapshot().conversation ==
               H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
        /* Sound reaching the Track ends WAITING. */
        ++downlink_writes;
        assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
        teardown();
        continue;
      }
      if (phase == 3u) {
        const h2_gizclaw_operation_result_t finished = {
            .terminal_kind = H2_GIZCLAW_OPERATION_FINISHED,
            .result = H2_PAL_OK,
        };
        terminal(terminal_user, conversation, &finished);
        assert(snapshot().conversation ==
               H2_GIZCLAW_SESSION_CONVERSATION_CALLING);
        assert(snapshot().conversation_input_open);
        assert(audio_starts == 2u && terminal_count == 0u);
      }
      context.selection.workspace_name = "switched-chat";
      context.selection.parameters = phase == 3u ? &ptt : &realtime;
      reload_failure = fail != 0u;
      h2_pal_task_t *task = NULL;
      assert(h2_pal_task_start(tasks, NULL, switch_thread, &context, &task) ==
             H2_PAL_OK);
      wait_flag(&cancel_entered);
      assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_WORKSPACE);
      assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
      assert(!snapshot().conversation_input_open);
      assert(audio_ends == 1u);
      /* A late old reply cannot revive the canceled UI while reload waits. */
      assert(on_event(terminal_user, conversation, &reply) == H2_PAL_OK);
      assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
      terminal(terminal_user, conversation, &canceled);
      assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
      assert(context.result == (fail ? H2_PAL_ERR_IO : H2_PAL_OK));
      assert(snapshot().parameters.input ==
             (fail ? (phase == 3u ? realtime.input : ptt.input)
                   : context.selection.parameters->input));
      assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
      if (phase != 3u)
        assert(snapshot().parameters.initiative ==
               H2_GIZCLAW_CONVERSATION_INITIATIVE_PEER);
      teardown();
    }
  }
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  h2_gizclaw_session_selection_t sel = selection;
  sel.parameters = &ptt;
  h2_gizclaw_conversation_t *conversation = NULL;
  assert(h2_gizclaw_session_conversation_create(session, &sel, 1000u, NULL,
                                                completed, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  const unsigned ends_before_noop = audio_ends;
  const unsigned noops_before = end_noops;
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  assert(audio_ends == ends_before_noop && end_noops == noops_before + 1u);
  h2_pal_task_t *task = NULL;
  h2_pal_result_t result = H2_PAL_ERR_IO;
  assert(h2_pal_task_start(tasks, NULL, start_thread, &result, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_RESTART);
  assert(h2_gizclaw_session_run_stop_begin_internal(session, 1000u) ==
         H2_PAL_ERR_BUSY);
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(result == H2_PAL_OK && audio_starts == 2u && terminal_count == 0u);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_RECORDING);
  audio_end_result = H2_PAL_ERR_INVALID_ARG;
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_ERR_INVALID_ARG);
  assert(strstr(audio_error_log, "stage=audio_end rc=-1") != NULL);
  assert(strstr(audio_error_log, "open=1 running=1 restarting=0") != NULL);
  audio_end_result = H2_PAL_OK;
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  audio_start_result = H2_PAL_ERR_INVALID_ARG;
  atomic_store(&cancel_entered, false);
  assert(h2_pal_task_start(tasks, NULL, start_thread, &result, &task) == H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_RESTART);
  assert(h2_gizclaw_session_run_stop_begin_internal(session, 1000u) ==
         H2_PAL_ERR_BUSY);
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(result == H2_PAL_ERR_INVALID_ARG);
  assert(strstr(audio_error_log, "stage=audio_start rc=-1") != NULL);
  assert(strstr(audio_error_log, "session=") != NULL);
  assert(strstr(audio_error_log, "conversation=") != NULL);
  assert(strstr(audio_error_log, "open=0 running=0 restarting=0") != NULL);
  assert(strstr(audio_error_log, "workspace=") != NULL);
  assert(strstr(audio_error_log, "gen=") != NULL);
  h2_gizclaw_session_conversation_release(session, conversation);
  teardown();
}

/* Mirrors the synchronous delete RPC's Session participation at the typed
 * boundary; a successful server delete makes the next get return Not Found. */
static h2_pal_result_t delete_workspace(const char *name,
                                        h2_pal_result_t server_result) {
  bool participating = false;
  h2_pal_result_t rc = h2_gizclaw_session_workspace_delete_begin_internal(
      session, (h2_gizclaw_str_t){name, strlen(name)}, 1000u, &participating);
  if (rc == H2_PAL_OK) {
    trace('d');
    rc = server_result;
    if (rc == H2_PAL_OK && strcmp(name, selection.workspace_name) == 0)
      missing = true;
  }
  if (participating)
    rc = h2_gizclaw_session_workspace_delete_finish_internal(session, rc);
  return rc;
}
typedef struct delete_context {
  const char *name;
  h2_pal_result_t result;
} delete_context_t;
static void delete_thread(void *user) {
  delete_context_t *context = user;
  context->result = delete_workspace(context->name, H2_PAL_OK);
}
static void assert_deleted_empty(void) {
  const h2_gizclaw_session_state_t state = snapshot();
  const h2_gizclaw_workspace_parameters_patch_t none = {0};
  assert(state.workspace == H2_GIZCLAW_SESSION_EMPTY);
  assert(state.current_workspace[0] == '\0');
  assert(state.target_workspace[0] == '\0');
  assert(state.workflow_name[0] == '\0');
  assert(memcmp(&state.parameters, &none, sizeof(none)) == 0);
  assert(state.last_error == H2_PAL_OK);
  assert(state.error_stage == H2_GIZCLAW_SESSION_BLOCK_NONE);
  assert(!state.can_start &&
         state.blocking_reason == H2_GIZCLAW_SESSION_BLOCK_WORKSPACE);
}

/* Typed boundary for stopping the Peer's run. */
static h2_pal_result_t stop_run(h2_pal_result_t result, uint32_t timeout) {
  h2_pal_result_t rc =
      h2_gizclaw_session_run_stop_begin_internal(session, timeout);
  if (rc != H2_PAL_OK) return rc;
  trace('s');
  if (result == H2_PAL_OK)
    h2_gizclaw_conversation_downlink_flush_internal(NULL);
  return h2_gizclaw_session_workspace_delete_finish_internal(session, result);
}
static void stop_run_thread(void* user) {
  *(h2_pal_result_t*)user = stop_run(H2_PAL_OK, 1000u);
}
static void test_run_stop(void) {
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  h2_gizclaw_session_selection_t sel = selection;
  sel.workspace_name = "room-b";
  h2_gizclaw_conversation_t* conversation = NULL;
  assert(h2_gizclaw_session_conversation_create(session, &sel, 1000u, NULL,
                                                completed, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  rpc_trace[0] = '\0';
  unsigned before = flushes;
  h2_pal_result_t result = H2_PAL_ERR_IO;
  h2_pal_task_t* task = NULL;
  const h2_pal_task_api_t* tasks = h2_desktop_platform_task_api();
  assert(h2_pal_task_start(tasks, NULL, stop_run_thread, &result, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_WORKSPACE);
  assert(rpc_trace[0] == '\0');
  assert(stop_run(H2_PAL_OK, 1000u) == H2_PAL_ERR_BUSY);
  const h2_gizclaw_operation_result_t canceled = {
      .terminal_kind = H2_GIZCLAW_OPERATION_CANCELED, .result = H2_PAL_OK};
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(result == H2_PAL_OK && strcmp(rpc_trace, "s") == 0);
  assert(flushes > before);
  assert_deleted_empty();
  /* Even the same name must reload after stop. */
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "sgr") == 0);
  assert(stop_run(H2_PAL_OK, 1000u) == H2_PAL_OK);
  sel.workspace_name = "room-a";
  rpc_trace[0] = '\0';
  assert(h2_gizclaw_session_conversation_create(session, &sel, 1000u, NULL,
                                                completed, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "gr") == 0);
  assert(strcmp(snapshot().current_workspace, "room-a") == 0);
  h2_gizclaw_session_conversation_release(session, conversation);
  const h2_pal_result_t errors[] = {H2_PAL_ERR_IO, H2_PAL_ERR_TIMEOUT};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    rpc_trace[0] = '\0';
    assert(stop_run(errors[i], 1000u) == errors[i]);
    assert(snapshot().workspace == H2_GIZCLAW_SESSION_FAILED);
    assert(snapshot().last_error == errors[i]);
    assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
    assert(strcmp(rpc_trace, "sgr") == 0);
  }
  assert(h2_gizclaw_session_workspace_begin_internal(
             session, (h2_gizclaw_str_t){"room-a", 6u}, 1000u) == H2_PAL_OK);
  assert(stop_run(H2_PAL_OK, 1000u) == H2_PAL_ERR_BUSY);
  h2_gizclaw_session_workspace_delete_finish_internal(session, H2_PAL_OK);
  assert(stop_run(H2_PAL_OK, 1000u) == H2_PAL_OK);
  assert_deleted_empty();
  /* Local cancellation dispatch timeout fails before sending the RPC. */
  assert(h2_gizclaw_session_conversation_create(session, &sel, 1000u, NULL,
                                                completed, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  rpc_trace[0] = '\0';
  assert(stop_run(H2_PAL_OK, 1u) == H2_PAL_ERR_TIMEOUT);
  assert(snapshot().workspace == H2_GIZCLAW_SESSION_FAILED);
  assert(rpc_trace[0] == '\0');
  terminal(terminal_user, conversation, &canceled);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "gr") == 0);
  h2_gizclaw_session_conversation_release(session, conversation);
  assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  assert(stop_run(H2_PAL_OK, 1000u) == H2_PAL_ERR_CLOSED);
  teardown();
}

static void test_workspace_delete(void) {
  const h2_gizclaw_workspace_parameters_patch_t ptt = {
      .has_input = true,
      .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
  };
  h2_gizclaw_session_selection_t sel = selection;
  sel.parameters = &ptt;

  /* Deleting the current Workspace forgets it; select prepares it again. */
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(snapshot().can_start && snapshot().parameters.has_input);
  assert(strcmp(snapshot().workflow_name, "alpha") == 0);
  rpc_trace[0] = '\0';
  uint64_t revision = snapshot().revision;
  assert(delete_workspace("my-chat", H2_PAL_OK) == H2_PAL_OK);
  assert(snapshot().revision > revision);
  assert_deleted_empty();
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "dgcgr") == 0);
  assert(snapshot().can_start);
  assert(strcmp(snapshot().current_workspace, "my-chat") == 0);

  /* Deleting another Workspace keeps READY and the ready fast path. */
  rpc_trace[0] = '\0';
  revision = snapshot().revision;
  assert(delete_workspace("other", H2_PAL_OK) == H2_PAL_OK);
  assert(snapshot().revision == revision && snapshot().can_start);
  assert(strcmp(snapshot().current_workspace, "my-chat") == 0);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "d") == 0);

  /* An uncertain delete fails the Workspace; select re-validates it. */
  rpc_trace[0] = '\0';
  assert(delete_workspace("my-chat", H2_PAL_ERR_TIMEOUT) ==
         H2_PAL_ERR_TIMEOUT);
  assert(snapshot().workspace == H2_GIZCLAW_SESSION_FAILED);
  assert(snapshot().last_error == H2_PAL_ERR_TIMEOUT);
  assert(snapshot().error_stage == H2_GIZCLAW_SESSION_BLOCK_WORKSPACE);
  assert(!snapshot().can_start);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strcmp(rpc_trace, "dgr") == 0 && snapshot().can_start);

  /* A closed Session rejects delete like the other workspace RPCs. */
  assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  rpc_trace[0] = '\0';
  assert(delete_workspace("other", H2_PAL_OK) == H2_PAL_ERR_CLOSED);
  assert(rpc_trace[0] == '\0');
  teardown();

  /* An active conversation is canceled before the delete RPC is sent. */
  const h2_pal_task_api_t *tasks = h2_desktop_platform_task_api();
  const h2_gizclaw_operation_result_t canceled = {
      .terminal_kind = H2_GIZCLAW_OPERATION_CANCELED,
      .result = H2_PAL_OK,
  };
  const h2_gizclaw_conversation_event_t reply = {
      .kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
  };
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  h2_gizclaw_conversation_t *conversation = NULL;
  assert(h2_gizclaw_session_conversation_create(
             session, &sel, 1000u, NULL, completed, NULL, &conversation) ==
         H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_RECORDING);
  rpc_trace[0] = '\0';
  delete_context_t context = {.name = "my-chat", .result = H2_PAL_ERR_IO};
  h2_pal_task_t *task = NULL;
  assert(h2_pal_task_start(tasks, NULL, delete_thread, &context, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_WORKSPACE);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(!snapshot().conversation_input_open && audio_ends == 1u);
  assert(snapshot().workspace == H2_GIZCLAW_SESSION_PREPARING);
  assert(rpc_trace[0] == '\0');
  assert(delete_workspace("my-chat", H2_PAL_OK) == H2_PAL_ERR_BUSY);
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(context.result == H2_PAL_OK && terminal_count == 1u);
  assert(strcmp(rpc_trace, "d") == 0);
  assert_deleted_empty();
  /* Late replies cannot revive the deleted route. */
  assert(on_event(terminal_user, conversation, &reply) == H2_PAL_OK);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_session_conversation_create(
             session, &sel, 1000u, NULL, completed, NULL, &conversation) ==
         H2_PAL_OK);
  assert(strcmp(rpc_trace, "dgcgr") == 0 && conversations == 2u);
  assert(strcmp(snapshot().current_workspace, "my-chat") == 0);
  h2_gizclaw_session_conversation_release(session, conversation);
  teardown();
}

/* Text input is a Session operation: it owns the route like a released
 * push-to-talk input until its completion. */
static void test_send_text(void) {
  const h2_pal_task_api_t *tasks = h2_desktop_platform_task_api();
  const h2_gizclaw_workspace_parameters_patch_t ptt = {
      .has_input = true, .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK};
  const h2_gizclaw_workspace_parameters_patch_t realtime = {
      .has_input = true, .input = H2_GIZCLAW_WORKSPACE_INPUT_REALTIME};
  const h2_gizclaw_operation_result_t sent = {
      .terminal_kind = H2_GIZCLAW_OPERATION_FINISHED, .result = H2_PAL_OK};
  const h2_gizclaw_operation_result_t canceled = {
      .terminal_kind = H2_GIZCLAW_OPERATION_CANCELED, .result = H2_PAL_OK};
  const h2_gizclaw_operation_result_t failed = {
      .terminal_kind = H2_GIZCLAW_OPERATION_FINISHED, .result = H2_PAL_ERR_IO};
  const h2_gizclaw_str_t hello = {"hello", 5u};
  h2_gizclaw_session_selection_t sel = selection;
  sel.parameters = &ptt;
  h2_gizclaw_conversation_t *conversation = NULL;

  /* Arguments, then Session state, are checked before the route. */
  assert(h2_gizclaw_session_send_text(NULL, hello) == H2_PAL_ERR_INVALID_ARG);
  setup(1u);
  const h2_gizclaw_str_t invalid[] = {
      {NULL, 1u}, {"", 0u},
      {"x", H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u}};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(h2_gizclaw_session_send_text(session, invalid[i]) ==
           H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_session_send_text(session, hello) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_send_text(session, hello) ==
         H2_PAL_ERR_INVALID_STATE); /* No conversation route yet. */
  assert(text_sends == 0u);
  assert(h2_gizclaw_session_conversation_create(
             session, &sel, 1000u, NULL, NULL, NULL, &conversation) ==
         H2_PAL_OK);

  /* Accepted text waits for sound; the route stays owned until completion. */
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_OK);
  assert(text_sends == 1u && strcmp(text_seen, "hello") == 0);
  h2_gizclaw_session_state_t state = snapshot();
  assert(state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
  assert(!state.conversation_input_open && !state.can_start);
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_ERR_BUSY);
  assert(text_sends == 1u);
  h2_gizclaw_session_conversation_release(session, conversation);
  assert(releases == 0u);
  assert(h2_gizclaw_session_destroy(&session) == H2_PAL_ERR_BUSY);
  terminal(terminal_user, conversation, &sent);
  assert(releases == 0u); /* Completion frees nothing by itself. */
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
  h2_gizclaw_session_conversation_release(session, conversation);
  assert(releases == 1u);
  ++downlink_writes;
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(snapshot().can_start && snapshot().last_error == H2_PAL_OK);
  teardown();

  /* Open audio input is never interrupted by text. */
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_conversation_create(
             session, &sel, 1000u, NULL, NULL, NULL, &conversation) ==
         H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_ERR_BUSY);
  assert(text_sends == 0u && snapshot().conversation_input_open);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_RECORDING);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &sent);

  /* Route admission failures: busy/backpressure keep state, others record. */
  text_result = H2_PAL_ERR_WOULD_BLOCK;
  uint64_t revision = snapshot().revision;
  assert(h2_gizclaw_session_send_text(session, hello) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(snapshot().revision == revision);
  text_result = H2_PAL_ERR_NO_MEMORY;
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_ERR_NO_MEMORY);
  state = snapshot();
  assert(state.last_error == H2_PAL_ERR_NO_MEMORY &&
         state.error_stage == H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);
  text_result = H2_PAL_OK;

  /* An asynchronous send failure ends WAITING and reports the error. */
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_OK);
  assert(snapshot().last_error == H2_PAL_OK);
  terminal(terminal_user, conversation, &failed);
  state = snapshot();
  assert(state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(state.last_error == H2_PAL_ERR_IO &&
         state.error_stage == H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);

  /* Pressing to talk replaces pending text on the same route. */
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_OK);
  const unsigned starts_before = audio_starts;
  h2_pal_task_t *task = NULL;
  h2_pal_result_t result = H2_PAL_ERR_IO;
  atomic_store(&cancel_entered, false);
  assert(h2_pal_task_start(tasks, NULL, start_thread, &result, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_RESTART);
  assert(h2_gizclaw_session_run_stop_begin_internal(session, 1000u) ==
         H2_PAL_ERR_BUSY);
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(result == H2_PAL_OK && audio_starts == starts_before + 1u);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_RECORDING);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &sent);

  /* A Workspace switch cancels pending text before the reload RPC. */
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_OK);
  const unsigned ends_before = audio_ends;
  switch_context_t context = {.selection = sel};
  context.selection.workspace_name = "switched-chat";
  context.selection.parameters = &realtime;
  atomic_store(&cancel_entered, false);
  assert(h2_pal_task_start(tasks, NULL, switch_thread, &context, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
  assert(atomic_load(&last_cancel_source) == H2_GIZCLAW_CANCEL_WORKSPACE);
  assert(audio_ends == ends_before); /* No audio input was open. */
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(context.result == H2_PAL_OK);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);

  /* Realtime Workspaces accept text while idle; its completion never starts
   * a call. */
  assert(snapshot().parameters.input == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME);
  const unsigned starts_realtime = audio_starts;
  assert(h2_gizclaw_session_send_text(session, hello) == H2_PAL_OK);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
  terminal(terminal_user, conversation, &sent);
  assert(audio_starts == starts_realtime && !snapshot().conversation_input_open);
  now += H2_GIZCLAW_SESSION_WAIT_MS;
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);

  /* A closed Session admits nothing. */
  assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_send_text(session, hello) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_session_conversation_release(session, conversation);
  teardown();
}


/* The TTS speech rate is a patch member like the others: it is range-checked at
 * admission, it makes an otherwise identical selection different so the reload
 * is not skipped, and a confirmed value is published in the snapshot. */
static void test_speech_rate_parameter(void) {
  const h2_gizclaw_workspace_parameters_patch_t slow = {
      .has_input = true,
      .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
      .has_tts_speech_rate_percent = true,
      .tts_speech_rate_percent = 70,
  };
  h2_gizclaw_session_selection_t sel = selection;
  sel.parameters = &slow;

  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(snapshot().parameters.has_tts_speech_rate_percent &&
         snapshot().parameters.tts_speech_rate_percent == 70);

  /* Confirmed parameters make a repeat selection a no-op. */
  rpc_trace[0] = '\0';
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(rpc_trace[0] == '\0');

  /* Changing only the rate must still reload, or the Server would keep the old
   * rate while the Session reported the new one. */
  const h2_gizclaw_workspace_parameters_patch_t faster = {
      .has_input = true,
      .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
      .has_tts_speech_rate_percent = true,
      .tts_speech_rate_percent = 150,
  };
  sel.parameters = &faster;
  assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
  assert(strchr(rpc_trace, 'r') != NULL);
  assert(snapshot().parameters.tts_speech_rate_percent == 150);

  /* A rate-only patch is a complete patch, and the boundaries are accepted. */
  const int32_t accepted[] = {
      H2_GIZCLAW_WORKSPACE_TTS_SPEECH_RATE_MIN_PERCENT, 100,
      H2_GIZCLAW_WORKSPACE_TTS_SPEECH_RATE_MAX_PERCENT};
  for (size_t i = 0u; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
    const h2_gizclaw_workspace_parameters_patch_t rate = {
        .has_tts_speech_rate_percent = true,
        .tts_speech_rate_percent = accepted[i]};
    sel.parameters = &rate;
    assert(h2_gizclaw_session_select(session, &sel, 1000u) == H2_PAL_OK);
    assert(snapshot().parameters.tts_speech_rate_percent == accepted[i]);
    /* input stays confirmed: an absent member preserves the stored value. */
    assert(snapshot().parameters.has_input &&
           snapshot().parameters.input ==
               H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK);
  }

  /* Out of range is refused at admission, with no RPC at all. */
  const int32_t refused[] = {49, 201, 0, -1};
  for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
    const h2_gizclaw_workspace_parameters_patch_t rate = {
        .has_tts_speech_rate_percent = true,
        .tts_speech_rate_percent = refused[i]};
    sel.parameters = &rate;
    rpc_trace[0] = '\0';
    assert(h2_gizclaw_session_select(session, &sel, 1000u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(rpc_trace[0] == '\0');
  }
  teardown();
}

int main(void) {
  test_catalog_buffer_lifetime(false, false);
  test_catalog_buffer_lifetime(true, false);
  test_catalog_buffer_lifetime(false, true);
  test_catalog_buffer_lifetime(true, true);
  test_catalog_buffer_create_failure();
  test_retained_allocator_create_failure();
  test_speech_rate_parameter();
  test_send_text();
  test_control_boundaries();
  test_run_stop();
  test_workspace_delete();
  test_waiting_selection(false, false);
  test_waiting_selection(true, false);
  test_waiting_selection(false, true);
  test_waiting_selection(true, true);
  setup(2u);
  assert(!snapshot().can_start);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(lists == 2u && snapshot().workflow_count == 2u);
  assert(snapshot().catalog == H2_GIZCLAW_SESSION_READY &&
         !snapshot().can_start);
  uint8_t bytes[4096];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_workflow_page_t catalog;
  assert(h2_gizclaw_session_catalog_copy(session, &storage, &catalog) ==
         H2_PAL_OK);
  catalog.items[0].name[0] = 'X';
  storage.used = 0u;
  assert(h2_gizclaw_session_catalog_copy(session, &storage, &catalog) ==
         H2_PAL_OK);
  assert(strcmp(catalog.items[0].name, "alpha") == 0);
  missing = true;
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  assert(creates == 1u && gets == 2u && reloads == 1u && snapshot().can_start);
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  assert(reloads == 1u);
  h2_gizclaw_conversation_t *conversation = NULL;
  assert(h2_gizclaw_session_conversation_create(session, &selection, 1000u,
                                                NULL, completed, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(!snapshot().can_start && conversations == 1u);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(snapshot().conversation_input_open);
  h2_gizclaw_session_conversation_release(session, conversation);
  assert(h2_gizclaw_session_destroy(&session) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  assert(!snapshot().conversation_input_open);
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_destroy(&session) == H2_PAL_ERR_BUSY);
  h2_gizclaw_operation_result_t result = {.result = H2_PAL_OK};
  terminal(terminal_user, conversation, &result);
  assert(terminal_count == 1u && snapshot().can_start);
  reload_failure = true;
  h2_gizclaw_session_selection_t other = selection;
  other.workspace_name = "other";
  assert(h2_gizclaw_session_select(session, &other, 1000u) == H2_PAL_ERR_IO);
  assert(strcmp(snapshot().current_workspace, "my-chat") == 0 &&
         !snapshot().can_start);
  teardown();

  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  other = selection;
  other.collection = "wrong";
  assert(h2_gizclaw_session_select(session, &other, 1000u) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(reloads == 1u && !snapshot().can_start);
  teardown();

  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(h2_gizclaw_session_conversation_create(session, &selection, 1000u,
                                                NULL, NULL, NULL,
                                                &conversation) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &result);
  /* Sent: the wait for sound goes on; without sound it ends silently at
   * the deadline. */
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
  now += H2_GIZCLAW_SESSION_WAIT_MS - 1u;
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
  now += 1u;
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &result);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  h2_gizclaw_operation_result_t remote_error = {
      .result = H2_PAL_ERR_IO,
      .error_code = "RUNTIME_PROFILE_MISMATCH",
      .retryable = true,
  };
  h2_gizclaw_conversation_event_t error_event = {
      .kind = H2_GIZCLAW_CONVERSATION_EVENT_ERROR,
      .error_code = remote_error.error_code,
      .retryable = remote_error.retryable,
  };
  assert(on_event(terminal_user, conversation, &error_event) == H2_PAL_OK);
  assert(strcmp(snapshot().error_code, "RUNTIME_PROFILE_MISMATCH") == 0);
  assert(snapshot().retryable && snapshot().last_error == H2_PAL_ERR_IO);
  terminal(terminal_user, conversation, &remote_error);
  memset(&remote_error, 0, sizeof(remote_error));
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE);
  assert(strcmp(snapshot().error_code, "RUNTIME_PROFILE_MISMATCH") == 0);
  assert(snapshot().retryable);
  assert(snapshot().error_stage == H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(snapshot().error_code[0] == '\0' && !snapshot().retryable);
  assert(snapshot().last_error == H2_PAL_OK);
  terminal(terminal_user, conversation, &result);
  assert(snapshot().error_code[0] == '\0' && !snapshot().retryable);
  h2_gizclaw_session_conversation_release(session, conversation);
  teardown();

  setup(2u);
  bad_revision = true;
  assert(h2_gizclaw_session_register(session, "token", 1000u) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(snapshot().registration == H2_GIZCLAW_SESSION_READY);
  assert(snapshot().catalog == H2_GIZCLAW_SESSION_FAILED &&
         snapshot().workflow_count == 0u);
  teardown();

  setup(1u);
  paginated = true;
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  assert(lists == 2u && snapshot().workflow_count == 2u);
  list_failure = true;
  conversation = (h2_gizclaw_conversation_t *)&conversations;
  assert(h2_gizclaw_session_refresh(session, 1000u) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_session_conversation_create(
             session, &selection, 1000u, NULL, NULL, NULL, &conversation) ==
         H2_PAL_ERR_IO);
  assert(conversation == NULL && conversations == 0u && !snapshot().can_start);
  teardown();

  setup(1u);
  empty_cycle = true;
  assert(h2_gizclaw_session_register(session, "token", 1000u) ==
         H2_PAL_ERR_FORMAT);
  assert(lists == 5u);
  teardown();

  setup(2u);
  list_delay = 100u;
  assert(h2_gizclaw_session_register(session, "token", 50u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(lists == 1u);
  teardown();

  setup(1u);
  close_during_list = true;
  assert(h2_gizclaw_session_register(session, "token", 1000u) ==
         H2_PAL_ERR_CLOSED);
  assert(snapshot().catalog == H2_GIZCLAW_SESSION_CLOSED &&
         snapshot().workflow_count == 0u);
  teardown();

  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  closed_after_reload = 1u;
  assert(h2_gizclaw_session_select(session, &selection, 1000u) ==
         H2_PAL_ERR_CLOSED);
  assert(snapshot().current_workspace[0] == '\0');
  teardown();
  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  server_revision = "v2";
  assert(h2_gizclaw_session_select(session, &selection, 1000u) == H2_PAL_OK);
  assert(lists == 2u && gets == 2u && reloads == 1u);
  assert(strcmp(snapshot().profile_revision, "v2") == 0);
  storage.used = 0u;
  storage.capacity = 1u;
  memset(&catalog, 0xff, sizeof(catalog));
  assert(h2_gizclaw_session_catalog_copy(session, &storage, &catalog) ==
         H2_PAL_ERR_NO_SPACE);
  assert(catalog.items == NULL && storage.used == 0u);
  teardown();

  setup(1u);
  assert(h2_gizclaw_session_register(session, "token", 1000u) == H2_PAL_OK);
  get_failure = true;
  assert(h2_gizclaw_session_select(session, &selection, 1000u) ==
         H2_PAL_ERR_IO);
  assert(creates == 0u);
  teardown();

  setup(1u);
  list_delay = 100u;
  assert(h2_gizclaw_session_register(session, "token", 50u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(snapshot().workflow_count == 0u);
  teardown();
  return 0;
}

h2_pal_result_t
h2_gizclaw_service_attach_session_internal(h2_gizclaw_service_t *service,
                                           h2_gizclaw_session_t *s) {
  (void)service;
  (void)s;
  ++attaches;
  return attach_result;
}
h2_pal_result_t
h2_gizclaw_service_detach_session_internal(h2_gizclaw_service_t *service) {
  (void)service;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *c) {
  (void)c;
  atomic_store(&cancel_entered, true);
  return H2_PAL_OK;
}

/* The downlink lives in the Conversation; the Session only reads how much
 * sound has reached the Track and asks for a flush. */
size_t h2_gizclaw_conversation_downlink_writes_internal(
    h2_gizclaw_service_t *service) {
  (void)service;
  return downlink_writes;
}
void h2_gizclaw_conversation_downlink_flush_internal(
    h2_gizclaw_service_t *service) {
  (void)service;
  ++flushes;
}

h2_pal_result_t h2_gizclaw_conversation_retarget_internal(
    h2_gizclaw_conversation_t *conversation, const char *workspace) {
  (void)conversation;
  assert(workspace != NULL && workspace[0] != '\0');
  return H2_PAL_OK;
}
