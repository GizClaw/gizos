#include "h2_desktop_platform.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_session.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

/* These fakes sit at the typed RPC boundary. The real Session owns all state,
 * pagination, catalog memory, workspace preparation and callback forwarding. */
static h2_gizclaw_session_t *session;
static atomic_uint lists, gets, creates, reloads, conversations;
static bool list_failure, bad_revision, missing, close_during_list,
    reload_failure;
static bool paginated, empty_cycle, get_failure;
static const char *server_revision;
static unsigned closed_after_reload;
static h2_gizclaw_conversation_completion_fn terminal;
static void *terminal_user;
static unsigned terminal_count;
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
  h2_gizclaw_session_state_t state;
  assert(h2_gizclaw_session_snapshot(session, &state) == H2_PAL_OK);
  assert(!state.can_start);
  assert(strcmp(state.target_workspace, name.data) == 0);
  if (reload_failure)
    return H2_PAL_ERR_IO;
  if (closed_after_reload)
    assert(h2_gizclaw_session_close(session) == H2_PAL_OK);
  *out = (h2_gizclaw_workspace_activation_t){
      .active_workspace_name = (char *)name.data,
      .workflow_name = "alpha",
      .runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING,
  };
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
  (void)callback;
  ++conversations;
  terminal = completion;
  terminal_user = user;
  *out = (h2_gizclaw_conversation_t *)&conversations;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service) {
  (void)service;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service) {
  (void)service;
  return H2_PAL_OK;
}
void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *conversation) {
  assert(conversation == (h2_gizclaw_conversation_t *)&conversations);
}
static void completed(void *user, h2_gizclaw_conversation_t *conversation,
                      const h2_gizclaw_operation_result_t *result) {
  (void)user;
  assert(result->result == H2_PAL_OK);
  ++terminal_count;
  h2_gizclaw_session_conversation_release(session, conversation);
}
static void setup(size_t collections) {
  static const char *const names[] = {"alpha", "beta"};
  lists = gets = creates = reloads = conversations = terminal_count = 0u;
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
      .catalog_bytes = 16384u,
  };
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
static void register_thread(void *out) {
  *(h2_pal_result_t *)out =
      h2_gizclaw_session_register(session, "token", 1000u);
}
static void select_thread(void *out) {
  *(h2_pal_result_t *)out =
      h2_gizclaw_session_select(session, &selection, 1000u);
}
static void test_waiting_selection(bool cancel) {
  setup(1u);
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
int main(void) {
  test_waiting_selection(false);
  test_waiting_selection(true);
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
  assert(h2_gizclaw_session_select(session, &selection, 1000u) ==
         H2_PAL_ERR_BUSY);
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
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &result);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_COMPLETED);
  assert(h2_gizclaw_session_audio_end(session) == H2_PAL_ERR_INVALID_STATE);
  assert(snapshot().conversation == H2_GIZCLAW_SESSION_CONVERSATION_COMPLETED);
  assert(h2_gizclaw_session_audio_start(session) == H2_PAL_OK);
  terminal(terminal_user, conversation, &result);
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
