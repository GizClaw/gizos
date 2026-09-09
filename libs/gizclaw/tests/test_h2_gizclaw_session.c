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
/* This test mocks Service; capture Session's production ERROR formatting. */
void h2_gizclaw_service_flush_audio_log_internal(
    const h2_gizclaw_service_t *service, const h2_gizclaw_audio_log_t *log) {
  (void)service;
  for (size_t i = 0u; i < log->count; ++i) {
    /* A diagnostic sink may read Session state: the outer lock must be free. */
    h2_gizclaw_session_state_t state;
    assert(h2_gizclaw_session_snapshot(session, &state) == H2_PAL_OK);
    const char *message = log->messages[i];
    if (log->levels[i] == H2_PAL_LOG_INFO)
      assert(strstr(message, "stage=interrupt_begin") != NULL);
    if (log->levels[i] != H2_PAL_LOG_ERROR)
      continue;
    assert(strlen(message) < sizeof(audio_error_log));
    strcpy(audio_error_log, message);
  }
}

h2_pal_result_t h2_gizclaw_service_audio_control_internal(
    h2_gizclaw_service_t *service, bool start, h2_gizclaw_audio_log_t *log) {
  (void)log;
  return start ? h2_gizclaw_service_audio_start(service)
               : h2_gizclaw_service_audio_end(service);
}

h2_pal_result_t h2_gizclaw_conversation_cancel_internal(
    h2_gizclaw_conversation_t *conversation, h2_gizclaw_audio_log_t *log) {
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
  audio_starts = audio_ends = 0u;
  audio_start_result = audio_end_result = H2_PAL_OK;
  audio_error_log[0] = '\0';
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
  for (unsigned phase = 0; phase < 4u; ++phase) {
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
      if (phase == 1u || phase == 2u) {
        assert(h2_gizclaw_session_audio_end(session) == H2_PAL_OK);
        assert(snapshot().conversation ==
               H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
      }
      if (phase == 2u || phase == 3u) {
        assert(on_event(terminal_user, conversation, &reply) == H2_PAL_OK);
        assert(snapshot().conversation ==
               (phase == 3u ? H2_GIZCLAW_SESSION_CONVERSATION_CALLING
                            : H2_GIZCLAW_SESSION_CONVERSATION_REPLYING));
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
  h2_pal_task_t *task = NULL;
  h2_pal_result_t result = H2_PAL_ERR_IO;
  assert(h2_pal_task_start(tasks, NULL, start_thread, &result, &task) ==
         H2_PAL_OK);
  wait_flag(&cancel_entered);
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
  terminal(terminal_user, conversation, &canceled);
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
  assert(result == H2_PAL_ERR_INVALID_ARG);
  assert(strstr(audio_error_log, "stage=audio_start rc=-1") != NULL);
  assert(strstr(audio_error_log, "session=") != NULL);
  assert(strstr(audio_error_log, "conversation=") != NULL);
  assert(strstr(audio_error_log, "open=0 running=0 restarting=0") != NULL);
  assert(strstr(audio_error_log, "workspace=") != NULL);
  h2_gizclaw_session_conversation_release(session, conversation);
  teardown();
}

int main(void) {
  test_control_boundaries();
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
  return H2_PAL_OK;
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

h2_pal_result_t h2_gizclaw_conversation_retarget_internal(
    h2_gizclaw_conversation_t *conversation, const char *workspace) {
  (void)conversation;
  assert(workspace != NULL && workspace[0] != '\0');
  return H2_PAL_OK;
}
